#include "source.h"

#include <initguid.h>
#include <ksmedia.h>
#include <mferror.h>
#include <mfvirtualcamera.h>

#include <cmath>

#include "../common/log.h"
#include "../common/settings.h"

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::MakeAndInitialize;

namespace ps5cam {

namespace {

struct TypeSpec {
    uint32_t w, h, fps;
    PixelFormat fmt;
};

HRESULT MakeVideoType(const TypeSpec& t, IMFMediaType** out)
{
    ComPtr<IMFMediaType> mt;
    HRESULT hr = MFCreateMediaType(&mt);
    if (FAILED(hr)) return hr;
    const bool yuy2 = t.fmt == PixelFormat::YUY2;
    mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    mt->SetGUID(MF_MT_SUBTYPE, yuy2 ? MFVideoFormat_YUY2 : MFVideoFormat_NV12);
    MFSetAttributeSize(mt.Get(), MF_MT_FRAME_SIZE, t.w, t.h);
    // Express the rate as a whole 100 ns frame interval (60 fps -> 166666, like the camera's own UVC
    // descriptors): the DirectShow bridge truncates intervals, and 60/1 would otherwise surface as
    // 59.9999 fps and reject a request for exactly 60.
    UINT32 interval = 10'000'000 / t.fps;
    MFSetAttributeRatio(mt.Get(), MF_MT_FRAME_RATE, 10'000'000, interval);
    MFSetAttributeRatio(mt.Get(), MF_MT_FRAME_RATE_RANGE_MAX, 10'000'000, interval);
    MFSetAttributeRatio(mt.Get(), MF_MT_FRAME_RATE_RANGE_MIN, 10'000'000, interval);
    MFSetAttributeRatio(mt.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    mt->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    mt->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    mt->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
    mt->SetUINT32(MF_MT_DEFAULT_STRIDE, yuy2 ? t.w * 2 : t.w);  // positive: top-down
    UINT32 sampleSize = yuy2 ? t.w * t.h * 2 : t.w * t.h * 3 / 2;
    mt->SetUINT32(MF_MT_SAMPLE_SIZE, sampleSize);
    mt->SetUINT32(MF_MT_AVG_BITRATE, sampleSize * 8 * t.fps);
    mt->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT601);
    mt->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    mt->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
    mt->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
    *out = mt.Detach();
    return S_OK;
}

HRESULT SetStreamAttributes(IMFAttributes* a)
{
    HRESULT hr = a->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE);
    if (SUCCEEDED(hr)) hr = a->SetUINT32(MF_DEVICESTREAM_STREAM_ID, 0);
    if (SUCCEEDED(hr)) hr = a->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1);
    if (SUCCEEDED(hr)) hr = a->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes_Color);
    return hr;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Ps5Stream

HRESULT Ps5Stream::RuntimeClassInitialize(Ps5Source* parent, bool prefer60)
{
    m_parent = parent;
    HRESULT hr = MFCreateEventQueue(&m_queue);
    if (FAILED(hr)) return hr;
    hr = MFCreateAttributes(&m_attributes, 8);
    if (FAILED(hr)) return hr;
    hr = SetStreamAttributes(m_attributes.Get());
    if (FAILED(hr)) return hr;

    // Both rates show one full 1920x1080 sensor; with depth (firmware e9) the second sensor comes
    // along at half size. Older images fall back to the 1280x800 stereo crop at 60 fps and both full
    // sensors at 30 (see SensorModes in capture.cpp).
    const uint32_t sizes[][2] = {{1920, 1080}, {1280, 720}};  // Full HD and HD; apps scale anything smaller
    const uint32_t rates[2] = {prefer60 ? 60u : 30u, prefer60 ? 30u : 60u};
    std::vector<ComPtr<IMFMediaType>> types;
    for (PixelFormat fmt : {PixelFormat::NV12, PixelFormat::YUY2})
        for (const auto& s : sizes)
            for (uint32_t fps : rates) {
                ComPtr<IMFMediaType> mt;
                hr = MakeVideoType({s[0], s[1], fps, fmt}, &mt);
                if (FAILED(hr)) return hr;
                types.push_back(mt);
            }
    std::vector<IMFMediaType*> raw;
    for (auto& t : types) raw.push_back(t.Get());
    hr = MFCreateStreamDescriptor(0, static_cast<DWORD>(raw.size()), raw.data(), &m_descriptor);
    if (FAILED(hr)) return hr;
    ComPtr<IMFMediaTypeHandler> handler;
    hr = m_descriptor->GetMediaTypeHandler(&handler);
    if (SUCCEEDED(hr)) hr = handler->SetCurrentMediaType(raw[0]);
    if (SUCCEEDED(hr)) hr = SetStreamAttributes(m_descriptor.Get());
    return hr;
}

HRESULT Ps5Stream::Start()
{
    std::lock_guard control(m_control);
    HRESULT hr = StartEngine();
    if (FAILED(hr)) return hr;
    std::lock_guard lock(m_lock);
    PROPVARIANT var;
    PropVariantInit(&var);
    var.vt = VT_I8;
    var.hVal.QuadPart = MFGetSystemTime();
    return m_queue->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, &var);
}

HRESULT Ps5Stream::StartEngine()
{
    // Called with m_control held.
    OutputRequest req;
    {
        std::lock_guard lock(m_lock);
        HRESULT hr = CheckShutdown();
        if (FAILED(hr)) return hr;
        ComPtr<IMFMediaTypeHandler> handler;
        ComPtr<IMFMediaType> mt;
        hr = m_descriptor->GetMediaTypeHandler(&handler);
        if (SUCCEEDED(hr)) hr = handler->GetCurrentMediaType(&mt);
        if (FAILED(hr)) return hr;
        GUID sub = {};
        UINT32 num = 30, den = 1;
        mt->GetGUID(MF_MT_SUBTYPE, &sub);
        MFGetAttributeSize(mt.Get(), MF_MT_FRAME_SIZE, &req.width, &req.height);
        MFGetAttributeRatio(mt.Get(), MF_MT_FRAME_RATE, &num, &den);
        req.fps = den && num ? static_cast<uint32_t>(std::lround(double(num) / den)) : 30;
        req.format = sub == MFVideoFormat_YUY2 ? PixelFormat::YUY2 : PixelFormat::NV12;
    }
    // Stop the previous session first (its frames may be in flight), then open a new epoch so that
    // anything the old session still delivers is recognised and dropped. The engine thread calls
    // back into OnFrame (which takes m_lock), so it is (re)started unlocked.
    m_engine.Stop();
    uint64_t epoch;
    {
        std::lock_guard lock(m_lock);
        m_tokens.clear();
        m_pending.Reset();
        m_state = MF_STREAM_STATE_RUNNING;
        epoch = ++m_epoch;
    }
    HRESULT hr = m_engine.Start(req, [this, epoch](IMFSample* s) { OnFrame(s, epoch); });
    if (FAILED(hr)) {
        std::lock_guard lock(m_lock);
        m_state = MF_STREAM_STATE_STOPPED;  // no engine: do not report a running stream
        ++m_epoch;
    }
    return hr;
}

// Moves the stream to stopped and stops the engine. Called with m_control held; the engine is
// stopped outside m_lock because its thread may be inside OnFrame.
void Ps5Stream::StopEngine()
{
    {
        std::lock_guard lock(m_lock);
        m_state = MF_STREAM_STATE_STOPPED;
        ++m_epoch;
        m_tokens.clear();
        m_pending.Reset();
    }
    m_engine.Stop();
}

HRESULT Ps5Stream::Stop()
{
    std::lock_guard control(m_control);
    {
        std::lock_guard lock(m_lock);
        HRESULT hr = CheckShutdown();
        if (FAILED(hr)) return hr;
    }
    StopEngine();
    std::lock_guard lock(m_lock);
    return m_queue->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr);
}

void Ps5Stream::Shutdown()
{
    std::lock_guard control(m_control);
    {
        std::lock_guard lock(m_lock);
        if (m_shutdown) return;
    }
    StopEngine();
    std::lock_guard lock(m_lock);
    m_shutdown = true;
    if (m_queue) m_queue->Shutdown();
}

void Ps5Stream::OnFrame(IMFSample* sample, uint64_t epoch)
{
    std::lock_guard lock(m_lock);
    if (m_shutdown || m_state != MF_STREAM_STATE_RUNNING || epoch != m_epoch) return;
    m_pending = sample;  // drop older undelivered frames: always serve the newest
    DeliverLocked();
}
void Ps5Stream::DeliverLocked()
{
    if (!m_pending || m_tokens.empty()) return;
    ComPtr<IUnknown> token = m_tokens.front();
    m_tokens.pop_front();
    if (token) m_pending->SetUnknown(MFSampleExtension_Token, token.Get());
    m_queue->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, m_pending.Get());
    m_pending.Reset();
}

STDMETHODIMP Ps5Stream::RequestSample(IUnknown* token)
{
    std::lock_guard lock(m_lock);
    HRESULT hr = CheckShutdown();
    if (FAILED(hr)) return hr;
    if (m_state != MF_STREAM_STATE_RUNNING) return MF_E_MEDIA_SOURCE_WRONGSTATE;
    if (m_tokens.size() > 16) m_tokens.pop_front();  // client asking far ahead of the camera
    m_tokens.emplace_back(token);
    DeliverLocked();
    return S_OK;
}

// Frame Server stops a stream this way when its last client goes, and may keep the source alive
// (no Stop, no Shutdown) for a long time: the camera and the GPU must stop here too, and start
// again when the stream is set running.
STDMETHODIMP Ps5Stream::SetStreamState(MF_STREAM_STATE state)
{
    std::lock_guard control(m_control);
    MF_STREAM_STATE current;
    {
        std::lock_guard lock(m_lock);
        HRESULT hr = CheckShutdown();
        if (FAILED(hr)) return hr;
        current = m_state;
        if (state == current) return S_OK;
        switch (state) {
        case MF_STREAM_STATE_PAUSED:  // the engine keeps running; frames are dropped meanwhile
            if (current != MF_STREAM_STATE_RUNNING) return MF_E_INVALID_STATE_TRANSITION;
            Log(L"stream paused");
            m_state = state;
            m_tokens.clear();
            m_pending.Reset();
            return S_OK;
        case MF_STREAM_STATE_RUNNING:
            if (current == MF_STREAM_STATE_PAUSED) {
                m_state = state;
                return S_OK;
            }
            break;  // stopped -> running: start the engine below
        case MF_STREAM_STATE_STOPPED:
            break;
        default:
            return MF_E_INVALID_STATE_TRANSITION;
        }
    }
    if (state == MF_STREAM_STATE_STOPPED) {
        StopEngine();
        return S_OK;
    }
    return StartEngine();
}

STDMETHODIMP Ps5Stream::GetStreamState(MF_STREAM_STATE* state)
{
    if (!state) return E_POINTER;
    std::lock_guard lock(m_lock);
    *state = m_state;
    return S_OK;
}

STDMETHODIMP Ps5Stream::GetMediaSource(IMFMediaSource** source)
{
    if (!source) return E_POINTER;
    std::lock_guard lock(m_lock);
    HRESULT hr = CheckShutdown();
    if (FAILED(hr)) return hr;
    return m_parent->QueryInterface(IID_PPV_ARGS(source));
}

STDMETHODIMP Ps5Stream::GetStreamDescriptor(IMFStreamDescriptor** sd)
{
    if (!sd) return E_POINTER;
    std::lock_guard lock(m_lock);
    HRESULT hr = CheckShutdown();
    if (FAILED(hr)) return hr;
    return m_descriptor.CopyTo(sd);
}

STDMETHODIMP Ps5Stream::BeginGetEvent(IMFAsyncCallback* cb, IUnknown* state)
{
    std::lock_guard lock(m_lock);
    HRESULT hr = CheckShutdown();
    return FAILED(hr) ? hr : m_queue->BeginGetEvent(cb, state);
}

STDMETHODIMP Ps5Stream::EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** ev)
{
    std::lock_guard lock(m_lock);
    HRESULT hr = CheckShutdown();
    return FAILED(hr) ? hr : m_queue->EndGetEvent(result, ev);
}

STDMETHODIMP Ps5Stream::GetEvent(DWORD flags, IMFMediaEvent** ev)
{
    ComPtr<IMFMediaEventQueue> queue;
    {
        std::lock_guard lock(m_lock);
        HRESULT hr = CheckShutdown();
        if (FAILED(hr)) return hr;
        queue = m_queue;
    }
    return queue->GetEvent(flags, ev);  // may block: never hold the lock here
}

STDMETHODIMP Ps5Stream::QueueEvent(MediaEventType type, REFGUID ext, HRESULT status, const PROPVARIANT* pv)
{
    std::lock_guard lock(m_lock);
    HRESULT hr = CheckShutdown();
    return FAILED(hr) ? hr : m_queue->QueueEventParamVar(type, ext, status, pv);
}

// ---------------------------------------------------------------------------------------------
// Ps5Source

HRESULT Ps5Source::RuntimeClassInitialize(IMFAttributes* activateAttributes)
{
    HRESULT hr = MFCreateEventQueue(&m_queue);
    if (FAILED(hr)) return hr;
    hr = MFCreateAttributes(&m_attributes, 4);
    if (FAILED(hr)) return hr;
    Settings settings = LoadSettings();
    hr = MakeAndInitialize<Ps5Stream>(&m_stream, this, settings.prefer60);
    if (FAILED(hr)) return hr;
    IMFStreamDescriptor* sd = m_stream->Descriptor();
    hr = MFCreatePresentationDescriptor(1, &sd, &m_pd);
    if (FAILED(hr)) return hr;
    hr = m_pd->SelectStream(0);
    if (FAILED(hr)) return hr;

    // The physical camera is opened by symbolic link when streaming starts. A source reader on top of the
    // Frame Server-provided associated source fails with MF_E_INVALIDREQUEST, so that path is not used;
    // AddDeviceSourceInfo at registration still reserves the camera for this virtual camera.
    UNREFERENCED_PARAMETER(activateAttributes);
    Log(L"source created");
    return S_OK;
}

STDMETHODIMP Ps5Source::BeginGetEvent(IMFAsyncCallback* cb, IUnknown* state)
{
    std::lock_guard lock(m_lock);
    HRESULT hr = CheckShutdown();
    return FAILED(hr) ? hr : m_queue->BeginGetEvent(cb, state);
}

STDMETHODIMP Ps5Source::EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** ev)
{
    std::lock_guard lock(m_lock);
    HRESULT hr = CheckShutdown();
    return FAILED(hr) ? hr : m_queue->EndGetEvent(result, ev);
}

STDMETHODIMP Ps5Source::GetEvent(DWORD flags, IMFMediaEvent** ev)
{
    ComPtr<IMFMediaEventQueue> queue;
    {
        std::lock_guard lock(m_lock);
        HRESULT hr = CheckShutdown();
        if (FAILED(hr)) return hr;
        queue = m_queue;
    }
    return queue->GetEvent(flags, ev);
}

STDMETHODIMP Ps5Source::QueueEvent(MediaEventType type, REFGUID ext, HRESULT status, const PROPVARIANT* pv)
{
    std::lock_guard lock(m_lock);
    HRESULT hr = CheckShutdown();
    return FAILED(hr) ? hr : m_queue->QueueEventParamVar(type, ext, status, pv);
}

STDMETHODIMP Ps5Source::CreatePresentationDescriptor(IMFPresentationDescriptor** pd)
{
    if (!pd) return E_POINTER;
    std::lock_guard lock(m_lock);
    HRESULT hr = CheckShutdown();
    return FAILED(hr) ? hr : m_pd->Clone(pd);
}

STDMETHODIMP Ps5Source::GetCharacteristics(DWORD* characteristics)
{
    if (!characteristics) return E_POINTER;
    std::lock_guard lock(m_lock);
    HRESULT hr = CheckShutdown();
    if (FAILED(hr)) return hr;
    *characteristics = MFMEDIASOURCE_IS_LIVE;
    return S_OK;
}

STDMETHODIMP Ps5Source::Pause()
{
    std::lock_guard lock(m_lock);
    HRESULT hr = CheckShutdown();
    return FAILED(hr) ? hr : MF_E_INVALID_STATE_TRANSITION;  // live source
}

STDMETHODIMP Ps5Source::Start(IMFPresentationDescriptor* pd, const GUID* timeFormat, const PROPVARIANT*)
{
    if (!pd) return E_INVALIDARG;
    if (timeFormat && *timeFormat != GUID_NULL) return MF_E_UNSUPPORTED_TIME_FORMAT;
    ComPtr<Ps5Stream> stream;
    bool selected = false;
    {
        std::lock_guard lock(m_lock);
        HRESULT hr = CheckShutdown();
        if (FAILED(hr)) return hr;
        BOOL sel = FALSE;
        ComPtr<IMFStreamDescriptor> sd;
        hr = pd->GetStreamDescriptorByIndex(0, &sel, &sd);
        if (FAILED(hr)) return hr;
        selected = sel != FALSE;
        if (selected) {
            // The caller may have chosen the type on its copy of the descriptor; mirror it onto ours.
            ComPtr<IMFMediaTypeHandler> theirs, ours;
            ComPtr<IMFMediaType> mt;
            if (SUCCEEDED(sd->GetMediaTypeHandler(&theirs)) && SUCCEEDED(theirs->GetCurrentMediaType(&mt)) &&
                SUCCEEDED(m_stream->Descriptor()->GetMediaTypeHandler(&ours)))
                ours->SetCurrentMediaType(mt.Get());
            hr = m_queue->QueueEventParamUnk(m_streamActive ? MEUpdatedStream : MENewStream, GUID_NULL, S_OK,
                static_cast<IMFMediaStream*>(m_stream.Get()));
            if (FAILED(hr)) return hr;
            m_streamActive = true;
            stream = m_stream;
        }
    }
    // Restarting the engine can take a while (camera + GPU setup); keep the source's event
    // queue available meanwhile.
    if (stream) {
        HRESULT hr = stream->Start();
        if (FAILED(hr)) return hr;
    }
    std::lock_guard lock(m_lock);
    HRESULT hr = CheckShutdown();
    if (FAILED(hr)) return hr;
    PROPVARIANT var;
    PropVariantInit(&var);
    var.vt = VT_I8;
    var.hVal.QuadPart = MFGetSystemTime();
    return m_queue->QueueEventParamVar(MESourceStarted, GUID_NULL, S_OK, &var);
}
STDMETHODIMP Ps5Source::Stop()
{
    ComPtr<Ps5Stream> stream;
    {
        std::lock_guard lock(m_lock);
        HRESULT hr = CheckShutdown();
        if (FAILED(hr)) return hr;
        stream = m_stream;
    }
    if (stream) stream->Stop();
    std::lock_guard lock(m_lock);
    return m_queue->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK, nullptr);
}

STDMETHODIMP Ps5Source::Shutdown()
{
    ComPtr<Ps5Stream> stream;
    {
        std::lock_guard lock(m_lock);
        if (m_shutdown) return MF_E_SHUTDOWN;
        m_shutdown = true;
        stream = m_stream;
    }
    if (stream) stream->Shutdown();
    std::lock_guard lock(m_lock);
    if (m_queue) m_queue->Shutdown();
    Log(L"source shut down");
    return S_OK;
}

STDMETHODIMP Ps5Source::GetSourceAttributes(IMFAttributes** attributes)
{
    if (!attributes) return E_POINTER;
    std::lock_guard lock(m_lock);
    HRESULT hr = CheckShutdown();
    return FAILED(hr) ? hr : m_attributes.CopyTo(attributes);  // the same object, not a copy
}

STDMETHODIMP Ps5Source::GetStreamAttributes(DWORD streamId, IMFAttributes** attributes)
{
    if (!attributes) return E_POINTER;
    std::lock_guard lock(m_lock);
    HRESULT hr = CheckShutdown();
    if (FAILED(hr)) return hr;
    if (streamId != 0) return MF_E_INVALIDSTREAMNUMBER;
    *attributes = m_stream->Attributes();
    (*attributes)->AddRef();
    return S_OK;
}

STDMETHODIMP Ps5Source::SetD3DManager(IUnknown*)
{
    return S_OK;  // frames are delivered in system memory
}

STDMETHODIMP Ps5Source::GetService(REFGUID, REFIID, LPVOID* object)
{
    if (!object) return E_POINTER;
    *object = nullptr;
    std::lock_guard lock(m_lock);
    HRESULT hr = CheckShutdown();
    return FAILED(hr) ? hr : MF_E_UNSUPPORTED_SERVICE;
}

STDMETHODIMP Ps5Source::KsProperty(PKSPROPERTY property, ULONG propertyLength, LPVOID data, ULONG dataLength,
    ULONG* bytesReturned)
{
    if (!property || propertyLength < sizeof(KSPROPERTY)) return E_INVALIDARG;
    // Image controls (brightness, contrast, exposure, ...) go to the real camera while it is open.
    if (property->Set == PROPSETID_VIDCAP_VIDEOPROCAMP || property->Set == PROPSETID_VIDCAP_CAMERACONTROL) {
        ComPtr<Ps5Stream> stream;
        {
            std::lock_guard lock(m_lock);
            HRESULT hr = CheckShutdown();
            if (FAILED(hr)) return hr;
            stream = m_stream;
        }
        ComPtr<IUnknown> physical = stream ? stream->PhysicalSource() : nullptr;
        ComPtr<IKsControl> ks;
        if (physical && SUCCEEDED(physical.As(&ks)))
            return ks->KsProperty(property, propertyLength, data, dataLength, bytesReturned);
    }
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP Ps5Source::KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*)
{
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP Ps5Source::KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*)
{
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

// ---------------------------------------------------------------------------------------------
// Ps5Activate

HRESULT Ps5Activate::RuntimeClassInitialize()
{
    return MFCreateAttributes(&m_attr, 4);
}

STDMETHODIMP Ps5Activate::ActivateObject(REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (m_source && m_source->IsShutdown()) m_source.Reset();  // re-activation after Shutdown
    if (!m_source) {
        HRESULT hr = MakeAndInitialize<Ps5Source>(&m_source, m_attr.Get());
        if (FAILED(hr)) {
            Log(L"source creation failed 0x%08lX", hr);
            return hr;
        }
    }
    return m_source->QueryInterface(riid, ppv);
}

STDMETHODIMP Ps5Activate::ShutdownObject()
{
    if (m_source) {
        m_source->Shutdown();
        m_source.Reset();
    }
    return S_OK;
}

STDMETHODIMP Ps5Activate::DetachObject()
{
    m_source.Reset();
    return S_OK;
}

}  // namespace ps5cam
