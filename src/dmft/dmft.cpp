#include "dmft.h"

#include <initguid.h>
#include <ksmedia.h>

#include <chrono>
#include <cmath>

#include "../common/camctl.h"
#include "../common/camdefaults.h"
#include "../common/log.h"
#include "../common/settings.h"

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::MakeAndInitialize;

namespace ps5cam {

namespace {

// Frames waiting for ProcessOutput; more means the app stopped reading, so the oldest go.
constexpr size_t kMaxQueued = 3;

bool Describe(IMFMediaType* t, GUID* sub, UINT32* w, UINT32* h, uint32_t* fps)
{
    UINT32 num = 0, den = 0;
    if (!t || FAILED(t->GetGUID(MF_MT_SUBTYPE, sub)) || FAILED(MFGetAttributeSize(t, MF_MT_FRAME_SIZE, w, h)) ||
        FAILED(MFGetAttributeRatio(t, MF_MT_FRAME_RATE, &num, &den)) || !den)
        return false;
    *fps = static_cast<uint32_t>(std::lround(double(num) / den));
    return true;
}

// Same format as far as streaming goes: subtype, frame size and (rounded) frame rate. Apps often
// set a type with fewer or slightly different attributes than the one offered.
bool SameFormat(IMFMediaType* a, IMFMediaType* b)
{
    GUID sa = {}, sb = {};
    UINT32 wa = 0, ha = 0, wb = 0, hb = 0;
    uint32_t fa = 0, fb = 0;
    return Describe(a, &sa, &wa, &ha, &fa) && Describe(b, &sb, &wb, &hb, &fb) && sa == sb && wa == wb && ha == hb &&
           fa == fb;
}

// Same type or both unset (DTM may hand back an equal type as another object).
bool SameType(IMFMediaType* a, IMFMediaType* b)
{
    return a == b || (a && b && SameFormat(a, b));
}

// The sensor mode key of a camera format, empty if it is none of ours.
const wchar_t* ModeKey(IMFMediaType* t)
{
    GUID sub = {};
    UINT32 w = 0, h = 0;
    uint32_t fps = 0;
    SensorMode mode;
    return Describe(t, &sub, &w, &h, &fps) && SensorModeOf(w, h, fps, &mode) ? mode.key : L"";
}

// A DMFT-initiated camera format change that DTM did not answer within this time is given up.
constexpr ULONGLONG kPendingTimeoutMs = 3000;

const wchar_t* StateName(DeviceStreamState s)
{
    switch (s) {
    case DeviceStreamState_Stop: return L"stop";
    case DeviceStreamState_Pause: return L"pause";
    case DeviceStreamState_Run: return L"run";
    default: return L"disabled";
    }
}

std::wstring TypeText(IMFMediaType* t)
{
    GUID sub = {};
    UINT32 w = 0, h = 0;
    uint32_t fps = 0;
    if (!Describe(t, &sub, &w, &h, &fps)) return L"(none)";
    wchar_t fourcc[5] = {wchar_t(sub.Data1 & 0xFF), wchar_t((sub.Data1 >> 8) & 0xFF), wchar_t((sub.Data1 >> 16) & 0xFF),
        wchar_t(sub.Data1 >> 24), 0};
    wchar_t text[64];
    swprintf_s(text, L"%ls %ux%u@%u", fourcc, w, h, fps);
    return text;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// StreamAttributes

HRESULT StreamAttributes::RuntimeClassInitialize(IMFAttributes* source)
{
    HRESULT hr = MFCreateAttributes(&m_store, 8);
    if (SUCCEEDED(hr) && source) hr = source->CopyAllItems(m_store.Get());
    if (source) source->QueryInterface(IID_PPV_ARGS(&m_pin));
    return hr;
}

STDMETHODIMP StreamAttributes::KsProperty(PKSPROPERTY p, ULONG pl, LPVOID d, ULONG dl, ULONG* r)
{
    return m_pin ? m_pin->KsProperty(p, pl, d, dl, r) : HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP StreamAttributes::KsMethod(PKSMETHOD m, ULONG ml, LPVOID d, ULONG dl, ULONG* r)
{
    return m_pin ? m_pin->KsMethod(m, ml, d, dl, r) : HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP StreamAttributes::KsEvent(PKSEVENT e, ULONG el, LPVOID d, ULONG dl, ULONG* r)
{
    return m_pin ? m_pin->KsEvent(e, el, d, dl, r) : HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

// ---------------------------------------------------------------------------------------------
// DeviceMft

HRESULT DeviceMft::RuntimeClassInitialize()
{
    return MFCreateEventQueue(&m_events);
}

DeviceMft::~DeviceMft()
{
    if (m_events) m_events->Shutdown();
}

DeviceMft::Stream* DeviceMft::Find(DWORD id)
{
    for (auto& s : m_streams)
        if (s->id == id) return s.get();
    return nullptr;
}

STDMETHODIMP DeviceMft::InitializeTransform(IMFAttributes* attributes)
{
    if (!attributes) return E_INVALIDARG;
    std::lock_guard lock(m_lock);
    if (m_shutdown) return MF_E_SHUTDOWN;
    try {  // no C++ exception (bad_alloc) may cross into Frame Server
        HRESULT hr = InitializeLocked(attributes);
        if (FAILED(hr)) {
            m_streams.clear();
            m_source.Reset();
            m_sourceKs.Reset();
        }
        return hr;
    } catch (...) {
        m_streams.clear();
        m_source.Reset();
        m_sourceKs.Reset();
        return E_OUTOFMEMORY;
    }
}

HRESULT DeviceMft::InitializeLocked(IMFAttributes* attributes)
{
    ComPtr<IUnknown> filter;
    HRESULT hr = attributes->GetUnknown(MF_DEVICEMFT_CONNECTED_FILTER_KSCONTROL, IID_PPV_ARGS(&filter));
    if (SUCCEEDED(hr)) hr = filter.As(&m_source);
    if (SUCCEEDED(hr)) hr = filter.As(&m_sourceKs);
    DWORD inputs = 0, outputs = 0;
    if (SUCCEEDED(hr)) hr = m_source->GetStreamCount(&inputs, &outputs);
    std::vector<DWORD> inIds(inputs ? inputs : 1), outIds(outputs ? outputs : 1);
    if (SUCCEEDED(hr) && outputs) hr = m_source->GetStreamIDs(inputs, inIds.data(), outputs, outIds.data());
    if (FAILED(hr)) {
        Log(L"InitializeTransform: no camera transform (0x%08lX)", hr);
        return hr;
    }
    const Settings settings = LoadSettings();
    for (DWORD i = 0; i < outputs; ++i) {
        auto s = std::make_unique<Stream>();
        s->id = outIds[i];
        m_source->GetOutputStreamAttributes(s->id, &s->sourceAttributes);
        for (DWORD k = 0;; ++k) {
            ComPtr<IMFMediaType> t;
            if (m_source->GetOutputAvailableType(s->id, k, &t) != S_OK) break;
            s->inputTypes.push_back(t);
        }
        GUID category = GUID_NULL;
        if (s->sourceAttributes) s->sourceAttributes->GetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, &category);
        bool video = category == PINNAME_VIDEO_CAPTURE || category == PINNAME_VIDEO_PREVIEW;
        for (auto& t : s->inputTypes) {
            GUID sub = {};
            UINT32 w = 0, h = 0;
            uint32_t fps = 0;
            SensorMode mode;
            if (video && Describe(t.Get(), &sub, &w, &h, &fps) && sub == MFVideoFormat_YUY2 && SensorModeOf(w, h, fps, &mode))
                s->processed = true;
        }
        if (s->processed) {
            // Only what the camera can feed, with or without depth (an older firmware image still in
            // the camera's RAM lacks some modes; on USB 2.0 none of ours exist).
            for (const TypeSpec& spec : OutputTypes(settings.prefer60, settings.fullHdOnly)) {
                ComPtr<IMFMediaType> t;
                if (SUCCEEDED(MakeVideoType(spec, &t)) &&
                    (InputFor(*s, t.Get(), kHalfKey, true) || InputFor(*s, t.Get(), kMonoKey, true)))
                    s->outputTypes.push_back(t);
            }
            s->processed = !s->outputTypes.empty();
        }
        if (s->processed) s->processor = std::make_unique<FrameProcessor>();
        else s->outputTypes = s->inputTypes;
        hr = MakeAndInitialize<StreamAttributes>(&s->outputAttributes, s->sourceAttributes.Get());
        if (SUCCEEDED(hr)) hr = s->outputAttributes->SetUINT32(MF_DEVICESTREAM_STREAM_ID, s->id);
        if (FAILED(hr)) return hr;
        Log(L"stream %lu: %ls, %zu camera formats, %zu offered", s->id, s->processed ? L"processed" : L"passed through",
            s->inputTypes.size(), s->outputTypes.size());
        m_streams.push_back(std::move(s));
    }
    return S_OK;
}

STDMETHODIMP DeviceMft::GetStreamCount(DWORD* inputs, DWORD* outputs)
{
    if (!inputs || !outputs) return E_POINTER;
    std::lock_guard lock(m_lock);
    *inputs = *outputs = static_cast<DWORD>(m_streams.size());
    return CheckShutdown();
}

STDMETHODIMP DeviceMft::GetStreamIDs(DWORD inputSize, DWORD* inputs, DWORD outputSize, DWORD* outputs)
{
    // The pipeline may ask for one of the two lists only (the other with size 0), like Microsoft's
    // sample device MFT handles it.
    std::lock_guard lock(m_lock);
    const DWORD n = static_cast<DWORD>(m_streams.size());
    if (inputSize < n && outputSize < n) return MF_E_BUFFERTOOSMALL;
    if ((inputSize && !inputs) || (outputSize && !outputs)) return E_POINTER;
    for (DWORD i = 0; i < n; ++i) {
        if (i < inputSize) inputs[i] = m_streams[i]->id;
        if (i < outputSize) outputs[i] = m_streams[i]->id;
    }
    return CheckShutdown();
}

STDMETHODIMP DeviceMft::GetInputAvailableType(DWORD stream, DWORD index, IMFMediaType** type)
{
    if (!type) return E_POINTER;
    *type = nullptr;
    std::lock_guard lock(m_lock);
    Stream* s = Find(stream);
    if (!s) return MF_E_INVALIDSTREAMNUMBER;
    if (index >= s->inputTypes.size()) return MF_E_NO_MORE_TYPES;
    return s->inputTypes[index].CopyTo(type);
}

STDMETHODIMP DeviceMft::GetInputCurrentType(DWORD stream, IMFMediaType** type)
{
    if (!type) return E_POINTER;
    *type = nullptr;
    std::lock_guard lock(m_lock);
    Stream* s = Find(stream);
    if (!s) return MF_E_INVALIDSTREAMNUMBER;
    if (!s->inputType) return MF_E_TRANSFORM_TYPE_NOT_SET;
    return s->inputType.CopyTo(type);
}

STDMETHODIMP DeviceMft::GetInputStreamAttributes(DWORD stream, IMFAttributes** attributes)
{
    if (!attributes) return E_POINTER;
    *attributes = nullptr;
    std::lock_guard lock(m_lock);
    Stream* s = Find(stream);
    if (!s) return MF_E_INVALIDSTREAMNUMBER;
    return s->sourceAttributes ? s->sourceAttributes.CopyTo(attributes) : E_NOTIMPL;
}

STDMETHODIMP DeviceMft::GetOutputAvailableType(DWORD stream, DWORD index, IMFMediaType** type)
{
    if (!type) return E_POINTER;
    *type = nullptr;
    std::lock_guard lock(m_lock);
    Stream* s = Find(stream);
    if (!s) return MF_E_INVALIDSTREAMNUMBER;
    if (index >= s->outputTypes.size()) return MF_E_NO_MORE_TYPES;
    return s->outputTypes[index].CopyTo(type);
}

STDMETHODIMP DeviceMft::GetOutputCurrentType(DWORD stream, IMFMediaType** type)
{
    if (!type) return E_POINTER;
    *type = nullptr;
    std::lock_guard lock(m_lock);
    Stream* s = Find(stream);
    if (!s) return MF_E_INVALIDSTREAMNUMBER;
    if (!s->outputType) return MF_E_TRANSFORM_TYPE_NOT_SET;
    return s->outputType.CopyTo(type);
}

STDMETHODIMP DeviceMft::GetOutputStreamAttributes(DWORD stream, IMFAttributes** attributes)
{
    if (!attributes) return E_POINTER;
    *attributes = nullptr;
    std::lock_guard lock(m_lock);
    Stream* s = Find(stream);
    if (!s) return MF_E_INVALIDSTREAMNUMBER;
    return s->outputAttributes.CopyTo(attributes);
}

IMFMediaType* DeviceMft::InputFor(Stream& s, IMFMediaType* output, const wchar_t* wantedKey, bool quiet)
{
    if (!output) return nullptr;
    if (!s.processed) {
        for (auto& t : s.inputTypes)
            if (SameFormat(t.Get(), output)) return t.Get();
        return nullptr;
    }
    GUID sub = {};
    UINT32 w = 0, h = 0;
    uint32_t fps = 0;
    if (!Describe(output, &sub, &w, &h, &fps)) return nullptr;
    const ViewMode view = wcscmp(wantedKey, kMonoKey) == 0 ? ViewMode::Main : ViewMode::Bokeh;
    for (const SensorMode& m : SensorModes(fps, view)) {
        for (auto& t : s.inputTypes) {
            GUID tsub = {};
            UINT32 tw = 0, th = 0;
            uint32_t tfps = 0;
            if (Describe(t.Get(), &tsub, &tw, &th, &tfps) && tsub == MFVideoFormat_YUY2 && tw == m.packedW &&
                th == m.packedH && tfps == m.fps)
                return t.Get();
        }
        if (!quiet) Log(L"sensor mode %ux%u@%u not offered by the camera", m.packedW, m.packedH, m.fps);
    }
    return nullptr;
}

void DeviceMft::QueueStreamEvent(MediaEventType type, REFGUID ext, DWORD stream)
{
    // m_events never changes after construction and is thread safe; after Shutdown it refuses.
    ComPtr<IMFMediaEvent> ev;
    if (SUCCEEDED(MFCreateMediaEvent(type, ext, S_OK, nullptr, &ev)) &&
        SUCCEEDED(ev->SetUINT32(MF_EVENT_MFT_INPUT_STREAM_ID, stream)))
        m_events->QueueEvent(ev.Get());
}

HRESULT DeviceMft::RequestInput(std::unique_lock<std::mutex>& lock, Stream& s, IMFMediaType* type,
    DeviceStreamState state, bool wait)
{
    // DTM answers METransformInputStreamStateChanged with GetInputStreamPreferredState, sets the
    // camera's format and calls SetInputStreamState.
    s.preferredType = type;
    s.preferredState = state;
    const uint64_t before = s.inputChanges;
    const DWORD id = s.id;
    QueueStreamEvent(METransformInputStreamStateChanged, GUID_NULL, id);
    if (!wait) return S_OK;
    bool done = m_inputChanged.wait_for(lock, std::chrono::seconds(5), [&] {
        Stream* now = Find(id);
        return m_shutdown || !now || now->inputChanges != before;
    });
    if (m_shutdown) return MF_E_SHUTDOWN;
    if (!done) {
        Log(L"stream %lu: the camera format was not changed within 5 s", id);
        return MF_E_INVALIDREQUEST;
    }
    return S_OK;
}

STDMETHODIMP DeviceMft::SetOutputStreamState(DWORD stream, IMFMediaType* type, DeviceStreamState state, DWORD flags)
{
    std::unique_lock lock(m_lock);
    if (m_shutdown) return MF_E_SHUTDOWN;
    try {
        return SetOutputLocked(lock, stream, type, state, flags);
    } catch (...) {
        return E_OUTOFMEMORY;
    }
}

HRESULT DeviceMft::SetOutputLocked(std::unique_lock<std::mutex>& lock, DWORD stream, IMFMediaType* type,
    DeviceStreamState state, DWORD)
{
    Stream* s = Find(stream);
    if (!s) return MF_E_INVALIDSTREAMNUMBER;
    ComPtr<IMFMediaType> full = s->outputType;
    if (type) {
        full.Reset();
        for (auto& t : s->outputTypes)
            if (SameFormat(t.Get(), type)) full = t;
        if (!full) {
            Log(L"stream %lu: format %ls is not offered", stream, TypeText(type).c_str());
            return MF_E_INVALIDMEDIATYPE;
        }
    }
    const bool active = state == DeviceStreamState_Run || state == DeviceStreamState_Pause;
    ComPtr<IMFMediaType> input = s->inputType;
    if (active) {
        const wchar_t* key = s->processor ? s->processor->WantedKey() : kHalfKey;
        input = InputFor(*s, full.Get(), key);
        if (!input) {
            Log(L"stream %lu: no camera format for %ls", stream, TypeText(full.Get()).c_str());
            return MF_E_INVALIDMEDIATYPE;
        }
    }
    Log(L"stream %lu: output %ls %ls, camera %ls", stream, TypeText(full.Get()).c_str(), StateName(state),
        TypeText(input.Get()).c_str());
    const DeviceStreamState previous = s->outputState;
    const bool changed = !SameType(full.Get(), s->outputType.Get()) || previous != state;
    s->outputState = DeviceStreamState_Disabled;  // no frames while the formats change
    s->queue.clear();
    if (!SameType(input.Get(), s->inputType.Get()) || s->inputState != state) {
        HRESULT hr = RequestInput(lock, *s, input.Get(), state, true);
        s = Find(stream);
        if (!s) return MF_E_SHUTDOWN;
        if (FAILED(hr)) return hr;
    }
    s->inputChangePending = false;  // this change supersedes one we asked for ourselves
    if (s->processor) {
        if (active && !s->processor->Configure(s->inputType.Get(), full.Get())) {
            Log(L"stream %lu: cannot process %ls into %ls", stream, TypeText(s->inputType.Get()).c_str(),
                TypeText(full.Get()).c_str());
            return MF_E_INVALIDMEDIATYPE;  // the stream stays disabled
        }
        if (!active) s->processor->Reset();
    }
    s->outputType = full;
    s->outputState = state;
    s->discontinuity = true;
    if (changed) QueueStreamEvent(MEUnknown, MEDeviceStreamCreated, stream);
    return S_OK;
}

STDMETHODIMP DeviceMft::GetOutputStreamState(DWORD stream, DeviceStreamState* state)
{
    if (!state) return E_POINTER;
    std::lock_guard lock(m_lock);
    Stream* s = Find(stream);
    if (!s) return MF_E_INVALIDSTREAMNUMBER;
    *state = s->outputState;
    return S_OK;
}

STDMETHODIMP DeviceMft::SetInputStreamState(DWORD stream, IMFMediaType* type, DeviceStreamState state, DWORD)
{
    std::lock_guard lock(m_lock);
    if (m_shutdown) return MF_E_SHUTDOWN;
    Stream* s = Find(stream);
    if (!s) return MF_E_INVALIDSTREAMNUMBER;
    s->inputType = type;
    s->inputState = state;
    s->preferredType.Reset();
    ++s->inputChanges;
    Log(L"stream %lu: camera %ls %ls", stream, TypeText(type).c_str(), StateName(state));
    if (s->inputChangePending) {
        // Our own request after the effect changed while streaming (see ProcessInput).
        s->inputChangePending = false;
        s->discontinuity = true;
        if (s->processor && s->outputState == DeviceStreamState_Run && !s->processor->Configure(type, s->outputType.Get()))
            Log(L"stream %lu: cannot process %ls", stream, TypeText(type).c_str());
    }
    m_inputChanged.notify_all();
    return S_OK;
}

STDMETHODIMP DeviceMft::GetInputStreamState(DWORD stream, DeviceStreamState* state)
{
    if (!state) return E_POINTER;
    std::lock_guard lock(m_lock);
    Stream* s = Find(stream);
    if (!s) return MF_E_INVALIDSTREAMNUMBER;
    *state = s->inputState;
    return S_OK;
}

STDMETHODIMP DeviceMft::GetInputStreamPreferredState(DWORD stream, DeviceStreamState* state, IMFMediaType** type)
{
    std::lock_guard lock(m_lock);
    Stream* s = Find(stream);
    if (!s) return MF_E_INVALIDSTREAMNUMBER;
    if (state) *state = s->preferredState;
    if (type) {
        *type = nullptr;
        if (s->preferredType) s->preferredType.CopyTo(type);
    }
    return S_OK;
}

STDMETHODIMP DeviceMft::ProcessInput(DWORD stream, IMFSample* sample, DWORD)
{
    HRESULT hr = S_OK;
    bool haveOutput = false;
    int powerLine = -1;
    ComPtr<IKsControl> cameraKs;
    try {
        std::unique_lock lock(m_lock);
        Stream* s = m_shutdown ? nullptr : Find(stream);
        if (m_shutdown) hr = MF_E_SHUTDOWN;
        else if (!s) hr = MF_E_INVALIDSTREAMNUMBER;
        else if (sample && s->outputState == DeviceStreamState_Run) {
            ComPtr<IMFSample> out = s->processor ? s->processor->Process(sample) : ComPtr<IMFSample>(sample);
            if (s->processor && (powerLine = s->processor->TakePowerLineRequest()) >= 0) cameraKs = m_sourceKs;
            if (out) {
                if (s->discontinuity) {
                    out->SetUINT32(MFSampleExtension_Discontinuity, TRUE);
                    s->discontinuity = false;
                }
                s->queue.push_back(out);
                while (s->queue.size() > kMaxQueued) s->queue.pop_front();
                haveOutput = true;
            }
            if (s->inputChangePending && GetTickCount64() - s->pendingTick > kPendingTimeoutMs) {
                Log(L"stream %lu: the camera format change was not answered, giving up", stream);
                s->inputChangePending = false;
            }
            // The effect switched between needing depth and not while streaming: ask for the other
            // sensor mode (the answer arrives as SetInputStreamState). Compared by the mode the
            // camera would actually run, so an image without the wanted mode does not ask forever.
            if (s->processor && s->processor->Configured() && !s->inputChangePending) {
                IMFMediaType* next = InputFor(*s, s->outputType.Get(), s->processor->WantedKey(), true);
                if (next && wcscmp(ModeKey(next), s->processor->InputKey()) != 0) {
                    Log(L"stream %lu: the effect now needs another sensor mode, switching the camera to %ls", stream,
                        TypeText(next).c_str());
                    s->inputChangePending = true;
                    s->pendingTick = GetTickCount64();
                    RequestInput(lock, *s, next, DeviceStreamState_Run, false);
                }
            }
        }
    } catch (...) {
        hr = E_OUTOFMEMORY;
    }
    if (haveOutput) m_events->QueueEventParamVar(METransformHaveOutput, GUID_NULL, S_OK, nullptr);
    if (cameraKs) {
        const HRESULT plHr = SetPowerLineFrequency(cameraKs.Get(), powerLine);
        Log(L"anti-flicker %ls (0x%08lX)", powerLine == 0 ? L"off" : powerLine == 1 ? L"50 Hz" : L"60 Hz",
            static_cast<unsigned long>(plHr));
    }
    // The device transform manager hands the sample over with one reference too many (a known
    // pipeline quirk, see Microsoft's SampleDeviceMFT): release it, or the camera's sample pool
    // runs dry after a few frames. Seen here: 3 references before, 2 after. Should a later Windows
    // fix the quirk, the count would be lower and releasing would free a sample still in use.
    if (sample) {
        const ULONG before = sample->AddRef() - 1;
        sample->Release();
        const bool extra = before >= 3;
        if (extra) sample->Release();
        if (m_inputReleases < 3) {
            ++m_inputReleases;
            Log(L"input sample references %lu%ls", before, extra ? L", released the extra one" : L", left alone");
        }
    }
    return hr;
}

STDMETHODIMP DeviceMft::ProcessOutput(DWORD, DWORD count, MFT_OUTPUT_DATA_BUFFER* buffers, DWORD* status)
{
    if (!buffers || !status) return E_POINTER;
    std::lock_guard lock(m_lock);
    if (m_shutdown) return MF_E_SHUTDOWN;
    *status = 0;
    bool any = false;
    for (DWORD i = 0; i < count; ++i) {
        buffers[i].dwStatus = 0;
        Stream* s = Find(buffers[i].dwStreamID);
        if (!s || s->queue.empty() || buffers[i].pSample) continue;
        buffers[i].pSample = s->queue.front().Detach();
        s->queue.pop_front();
        any = true;
    }
    return any ? S_OK : MF_E_TRANSFORM_NEED_MORE_INPUT;
}

STDMETHODIMP DeviceMft::ProcessEvent(DWORD, IMFMediaEvent*)
{
    return S_OK;
}

STDMETHODIMP DeviceMft::ProcessMessage(MFT_MESSAGE_TYPE message, ULONG_PTR)
{
    std::lock_guard lock(m_lock);
    if (m_shutdown) return MF_E_SHUTDOWN;
    switch (message) {
    case MFT_MESSAGE_COMMAND_FLUSH:
        for (auto& s : m_streams) s->queue.clear();
        break;
    default:
        // Streaming starts and stops per stream through SetOutputStreamState; MFT_MESSAGE_SET_D3D_MANAGER
        // is not needed either: the effect brings its own D3D11 device and hands out system-memory samples.
        break;
    }
    return S_OK;
}

STDMETHODIMP DeviceMft::FlushInputStream(DWORD, DWORD)
{
    return S_OK;
}

STDMETHODIMP DeviceMft::FlushOutputStream(DWORD stream, DWORD)
{
    std::lock_guard lock(m_lock);
    Stream* s = Find(stream);
    if (!s) return MF_E_INVALIDSTREAMNUMBER;
    s->queue.clear();
    return S_OK;
}

STDMETHODIMP DeviceMft::Shutdown()
{
    std::vector<std::unique_ptr<Stream>> streams;
    {
        std::lock_guard lock(m_lock);
        if (m_shutdown) return S_OK;
        m_shutdown = true;
        streams.swap(m_streams);
        m_source.Reset();
        m_sourceKs.Reset();
    }
    m_events->Shutdown();  // event calls refuse with MF_E_SHUTDOWN from now on
    m_inputChanged.notify_all();
    for (auto& s : streams)
        if (s->processor) s->processor->Reset();
    Log(L"shut down");
    return S_OK;
}

STDMETHODIMP DeviceMft::GetShutdownStatus(MFSHUTDOWN_STATUS* status)
{
    if (!status) return E_POINTER;
    std::lock_guard lock(m_lock);
    if (!m_shutdown) return MF_E_INVALIDREQUEST;
    *status = MFSHUTDOWN_COMPLETED;
    return S_OK;
}

// The event queue is created with the object, never replaced, thread safe, and refuses calls after
// its Shutdown: no m_lock here, so a callback the queue might run on the calling thread cannot
// deadlock on it.
STDMETHODIMP DeviceMft::BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state)
{
    return m_events->BeginGetEvent(callback, state);
}

STDMETHODIMP DeviceMft::EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event)
{
    return m_events->EndGetEvent(result, event);
}

STDMETHODIMP DeviceMft::GetEvent(DWORD flags, IMFMediaEvent** event)
{
    return m_events->GetEvent(flags, event);
}

STDMETHODIMP DeviceMft::QueueEvent(MediaEventType type, REFGUID ext, HRESULT status, const PROPVARIANT* pv)
{
    return m_events->QueueEventParamVar(type, ext, status, pv);
}

STDMETHODIMP DeviceMft::RegisterThreadsEx(DWORD*, LPCWSTR, LONG)
{
    return S_OK;
}

STDMETHODIMP DeviceMft::UnregisterThreads()
{
    return S_OK;
}

STDMETHODIMP DeviceMft::SetWorkQueueEx(DWORD, LONG)
{
    return S_OK;  // frames are processed synchronously in ProcessInput
}

HRESULT DeviceMft::CameraKs(ComPtr<IKsControl>& ks)
{
    std::lock_guard lock(m_lock);
    if (m_shutdown) return MF_E_SHUTDOWN;
    ks = m_sourceKs;
    return ks ? S_OK : MF_E_NOT_INITIALIZED;
}

namespace {

constexpr ULONG kSegmentationSize = sizeof(KSCAMERA_EXTENDEDPROP_HEADER) + sizeof(KSCAMERA_EXTENDEDPROP_VALUE);
constexpr ULONGLONG kSegmentationCaps =
    KSCAMERA_EXTENDEDPROP_BACKGROUNDSEGMENTATION_BLUR | KSCAMERA_EXTENDEDPROP_BACKGROUNDSEGMENTATION_SHALLOWFOCUS;

}  // namespace

HRESULT DeviceMft::BackgroundSegmentation(PKSPROPERTY p, LPVOID d, ULONG dl, ULONG* r)
{
    *r = 0;
    if (p->Flags & KSPROPERTY_TYPE_GET) {
        if (dl < kSegmentationSize || !d) {
            *r = kSegmentationSize;
            return dl == 0 ? HRESULT_FROM_WIN32(ERROR_MORE_DATA) : HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
        }
        auto* h = static_cast<KSCAMERA_EXTENDEDPROP_HEADER*>(d);
        h->Version = 1;  // the only version of this control
        h->PinId = KSCAMERA_EXTENDEDPROP_FILTERSCOPE;
        h->Size = kSegmentationSize;
        h->Result = 0;
        h->Flags = BackgroundEffectFlags(LoadSettings());
        h->Capability = kSegmentationCaps;
        reinterpret_cast<KSCAMERA_EXTENDEDPROP_VALUE*>(h + 1)->Value.ull = 0;
        *r = kSegmentationSize;
        return S_OK;
    }
    if (p->Flags & KSPROPERTY_TYPE_SET) {
        if (dl < kSegmentationSize || !d) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
        const auto* h = static_cast<const KSCAMERA_EXTENDEDPROP_HEADER*>(d);
        const bool blur = (h->Flags & KSCAMERA_EXTENDEDPROP_BACKGROUNDSEGMENTATION_BLUR) != 0;
        const bool shallow = (h->Flags & KSCAMERA_EXTENDEDPROP_BACKGROUNDSEGMENTATION_SHALLOWFOCUS) != 0;
        // No mask metadata; shallow focus only together with blur (the control's contract).
        if (h->Version != 1 || h->PinId != KSCAMERA_EXTENDEDPROP_FILTERSCOPE || (h->Flags & ~kSegmentationCaps) ||
            (shallow && !blur))
            return E_INVALIDARG;
        // Settings are shared with the tray: a change here is a change of the camera's effect, and
        // the frames follow within half a second (FrameProcessor::RefreshSettings), sensor mode too.
        // Only the values concerned are written, so a tray change meanwhile is not undone.
        Settings s = LoadSettings();
        const ULONGLONG before = BackgroundEffectFlags(s);
        if (blur) {
            s.mode = 0;
            s.blurStyle = shallow ? kBlurPortrait : kBlurStandard;
        } else if (s.mode == 0) {
            s.mode = 1;  // off; diagnostic views stay as they are
        }
        if (BackgroundEffectFlags(s) != before) {
            if (!WriteSetting(L"Mode", s.mode) || !WriteSetting(L"BlurStyle", s.blurStyle)) return E_ACCESSDENIED;
            Log(L"background effects set to 0x%llX by Windows or an app", static_cast<unsigned long long>(h->Flags));
        }
        return S_OK;
    }
    return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
}

namespace {

// Windows' camera settings ("Basic settings": Brightness, Contrast, Saturation, Sharpness) and the
// video controls of apps: the driver's own picture settings, applied on the GPU to the finished
// picture (ToneControl), not the camera's processing unit, whose changes would reach both sensors
// and the stereo matching.
struct Tone {
    ULONG id;
    const wchar_t* name;  // registry value (settings.cpp)
    uint32_t PictureSettings::*value;
};
constexpr Tone kTones[] = {
    {KSPROPERTY_VIDEOPROCAMP_BRIGHTNESS, L"Brightness", &PictureSettings::brightness},
    {KSPROPERTY_VIDEOPROCAMP_CONTRAST, L"Contrast", &PictureSettings::contrast},
    {KSPROPERTY_VIDEOPROCAMP_SATURATION, L"Saturation", &PictureSettings::saturation},
    {KSPROPERTY_VIDEOPROCAMP_SHARPNESS, L"Sharpen", &PictureSettings::sharpen},
};
constexpr LONG kToneMin = 0, kToneMax = 100, kToneDefault = 50;

}  // namespace

HRESULT DeviceMft::ToneControl(ULONG index, PKSPROPERTY p, LPVOID d, ULONG dl, ULONG* r)
{
    const Tone& t = kTones[index];
    *r = 0;
    // Range and default as KS reports them: the description, then member lists (a stepped range,
    // the default value). BASICSUPPORT gives both, DEFAULTVALUES the default only; a caller may ask
    // for the access flags alone (a ULONG) or the description alone, to learn the size.
    const bool basic = (p->Flags & KSPROPERTY_TYPE_BASICSUPPORT) != 0;
    if (basic || (p->Flags & KSPROPERTY_TYPE_DEFAULTVALUES)) {
        const ULONG rangeBytes = sizeof(KSPROPERTY_MEMBERSHEADER) + sizeof(KSPROPERTY_STEPPING_LONG);
        const ULONG defaultBytes = sizeof(KSPROPERTY_MEMBERSHEADER) + sizeof(LONG);
        const ULONG full = sizeof(KSPROPERTY_DESCRIPTION) + (basic ? rangeBytes : 0) + defaultBytes;
        const ULONG access = KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT |
                             KSPROPERTY_TYPE_DEFAULTVALUES;
        if (!d || dl == 0) {
            *r = full;
            return HRESULT_FROM_WIN32(ERROR_MORE_DATA);
        }
        if (dl < sizeof(KSPROPERTY_DESCRIPTION)) {
            if (dl < sizeof(ULONG)) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
            *static_cast<ULONG*>(d) = access;
            *r = sizeof(ULONG);
            return S_OK;
        }
        auto* desc = static_cast<KSPROPERTY_DESCRIPTION*>(d);
        desc->AccessFlags = access;
        desc->DescriptionSize = full;
        desc->PropTypeSet.Set = KSPROPTYPESETID_General;
        desc->PropTypeSet.Id = VT_I4;
        desc->PropTypeSet.Flags = 0;
        desc->MembersListCount = basic ? 2 : 1;
        desc->Reserved = 0;
        // Then as many whole member lists as fit, the range first: a caller after the range alone
        // (IAMVideoProcAmp::GetRange) leaves room for that only.
        ULONG used = sizeof(KSPROPERTY_DESCRIPTION), lists = 0;
        auto* at = reinterpret_cast<BYTE*>(desc + 1);
        if (basic && dl >= used + rangeBytes) {
            auto* h = reinterpret_cast<KSPROPERTY_MEMBERSHEADER*>(at);
            h->MembersFlags = KSPROPERTY_MEMBER_STEPPEDRANGES;
            h->MembersSize = sizeof(KSPROPERTY_STEPPING_LONG);
            h->MembersCount = 1;
            h->Flags = 0;
            auto* range = reinterpret_cast<KSPROPERTY_STEPPING_LONG*>(h + 1);
            range->SteppingDelta = 1;
            range->Reserved = 0;
            range->Bounds.SignedMinimum = kToneMin;
            range->Bounds.SignedMaximum = kToneMax;
            at += rangeBytes;
            used += rangeBytes;
            ++lists;
        }
        if ((!basic || lists == 1) && dl >= used + defaultBytes) {
            auto* h = reinterpret_cast<KSPROPERTY_MEMBERSHEADER*>(at);
            h->MembersFlags = KSPROPERTY_MEMBER_VALUES;
            h->MembersSize = sizeof(LONG);
            h->MembersCount = 1;
            h->Flags = KSPROPERTY_MEMBER_FLAG_DEFAULT;
            *reinterpret_cast<LONG*>(h + 1) = kToneDefault;
            used += defaultBytes;
            ++lists;
        }
        if (lists) desc->MembersListCount = lists;  // the description alone keeps the total count
        *r = used;
        return S_OK;
    }
    // The value: after a KSPROPERTY, or after a KSP_NODE when the caller addresses the processing
    // unit's node (KSPROPERTY_TYPE_TOPOLOGY).
    const bool node = (p->Flags & KSPROPERTY_TYPE_TOPOLOGY) != 0;
    const ULONG size = node ? sizeof(KSPROPERTY_VIDEOPROCAMP_NODE_S) : sizeof(KSPROPERTY_VIDEOPROCAMP_S);
    LONG* current = nullptr;
    ULONG *flags = nullptr, *caps = nullptr;
    if (d && dl >= size) {
        if (node) {
            auto* v = static_cast<KSPROPERTY_VIDEOPROCAMP_NODE_S*>(d);
            current = &v->Value, flags = &v->Flags, caps = &v->Capabilities;
        } else {
            auto* v = static_cast<KSPROPERTY_VIDEOPROCAMP_S*>(d);
            current = &v->Value, flags = &v->Flags, caps = &v->Capabilities;
        }
    }
    if (p->Flags & KSPROPERTY_TYPE_GET) {
        if (!current) {
            *r = size;
            return dl == 0 ? HRESULT_FROM_WIN32(ERROR_MORE_DATA) : HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
        }
        const Settings s = LoadSettings();
        *current = static_cast<LONG>(s.*t.value);
        *flags = KSPROPERTY_VIDEOPROCAMP_FLAGS_MANUAL;
        *caps = KSPROPERTY_VIDEOPROCAMP_FLAGS_MANUAL;
        *r = size;
        return S_OK;
    }
    if (p->Flags & KSPROPERTY_TYPE_SET) {
        if (!current) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
        const LONG wanted = *current;
        if (wanted < kToneMin || wanted > kToneMax) return E_INVALIDARG;
        // Like the background effects: a setting of the camera, shared with ps5cam-ctl; the frames
        // follow within half a second (FrameProcessor::RefreshSettings).
        if (LoadSettings().*t.value != static_cast<uint32_t>(wanted)) {
            if (!WriteSetting(t.name, static_cast<uint32_t>(wanted))) return E_ACCESSDENIED;
            Log(L"%ls set to %ld by Windows or an app", t.name, wanted);
        }
        return S_OK;
    }
    return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
}

STDMETHODIMP DeviceMft::KsProperty(PKSPROPERTY p, ULONG pl, LPVOID d, ULONG dl, ULONG* r)
{
    if (p && pl >= sizeof(KSPROPERTY) && IsEqualGUID(p->Set, KSPROPERTYSETID_ExtendedCameraControl) &&
        p->Id == KSPROPERTY_CAMERACONTROL_EXTENDED_BACKGROUNDSEGMENTATION)
        return r ? BackgroundSegmentation(p, d, dl, r) : E_POINTER;
    // (Other requests about them, e.g. KSPROPERTY_TYPE_SETSUPPORT, go to the camera as before.)
    if (p && pl >= sizeof(KSPROPERTY) && IsEqualGUID(p->Set, PROPSETID_VIDCAP_VIDEOPROCAMP) &&
        (p->Flags & (KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT |
                        KSPROPERTY_TYPE_DEFAULTVALUES)))
        for (ULONG i = 0; i < ARRAYSIZE(kTones); ++i)
            if (p->Id == kTones[i].id) return r ? ToneControl(i, p, d, dl, r) : E_POINTER;
    if (p && pl >= sizeof(KSPROPERTY) && IsEqualGUID(p->Set, PROPSETID_VIDCAP_VIDEOPROCAMP) &&
        p->Id == KSPROPERTY_VIDEOPROCAMP_POWERLINE_FREQUENCY && (p->Flags & KSPROPERTY_TYPE_SET)) {
        // An app (or Windows' saved default) chooses the anti-flicker itself: its choice stands for
        // this stream, over Auto and over a fixed setting alike.
        std::lock_guard lock(m_lock);
        for (auto& s : m_streams)
            if (s->processor) s->processor->HoldPowerLine();
    }
    ComPtr<IKsControl> ks;
    HRESULT hr = CameraKs(ks);
    return SUCCEEDED(hr) ? ks->KsProperty(p, pl, d, dl, r) : hr;
}

STDMETHODIMP DeviceMft::KsMethod(PKSMETHOD m, ULONG ml, LPVOID d, ULONG dl, ULONG* r)
{
    ComPtr<IKsControl> ks;
    HRESULT hr = CameraKs(ks);
    return SUCCEEDED(hr) ? ks->KsMethod(m, ml, d, dl, r) : hr;
}

STDMETHODIMP DeviceMft::KsEvent(PKSEVENT e, ULONG el, LPVOID d, ULONG dl, ULONG* r)
{
    ComPtr<IKsControl> ks;
    HRESULT hr = CameraKs(ks);
    return SUCCEEDED(hr) ? ks->KsEvent(e, el, d, dl, r) : hr;
}

}  // namespace ps5cam
