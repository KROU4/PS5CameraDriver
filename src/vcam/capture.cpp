#include "capture.h"

#include <initguid.h>
#include <cfgmgr32.h>
#include <ks.h>
#include <ksmedia.h>
#include <mfapi.h>
#include <mferror.h>
#include <knownfolders.h>
#include <sddl.h>
#include <shlobj.h>
#include <wrl/implements.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "../common/camctl.h"
#include "../common/effect.h"
#include "../common/log.h"
#include "cameramodes.h"

using Microsoft::WRL::ComPtr;

namespace ps5cam {

constexpr DWORD kVideoStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);

// Shared between the engine and the source-reader callback. The callback may fire after the engine
// has moved on (or is gone), so it only touches the engine while `engine` is set, under `m`.
struct ReaderLink {
    std::mutex m;
    CaptureEngine* engine = nullptr;
    ComPtr<IMFSourceReader> reader;
    HANDLE flushed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ~ReaderLink() { CloseHandle(flushed); }
};

namespace {

class ReaderCallback
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          IMFSourceReaderCallback> {
public:
    explicit ReaderCallback(std::shared_ptr<ReaderLink> link) : m_link(std::move(link)) {}
    STDMETHODIMP OnReadSample(HRESULT hr, DWORD, DWORD flags, LONGLONG, IMFSample* sample) override
    {
        std::lock_guard lock(m_link->m);
        if (m_link->engine && m_link->reader && m_link->engine->OnReadSample(hr, flags, sample))
            m_link->reader->ReadSample(kVideoStream, 0, nullptr, nullptr, nullptr, nullptr);
        return S_OK;
    }
    STDMETHODIMP OnFlush(DWORD) override
    {
        SetEvent(m_link->flushed);
        return S_OK;
    }
    STDMETHODIMP OnEvent(DWORD, IMFMediaEvent*) override { return S_OK; }

private:
    std::shared_ptr<ReaderLink> m_link;
};

std::wstring FindCameraLink()
{
    // Hidden cameras (SensorCameraMode) are only listed under the sensor-camera category.
    const GUID categories[] = {KSCATEGORY_VIDEO_CAMERA, KSCATEGORY_SENSOR_CAMERA, KSCATEGORY_VIDEO};
    for (const GUID& cat : categories) {
        ULONG len = 0;
        if (CM_Get_Device_Interface_List_SizeW(&len, const_cast<GUID*>(&cat), nullptr,
                CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS || len < 2)
            continue;
        std::vector<wchar_t> list(len);
        if (CM_Get_Device_Interface_ListW(const_cast<GUID*>(&cat), nullptr, list.data(), len,
                CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS)
            continue;
        for (const wchar_t* p = list.data(); *p; p += wcslen(p) + 1) {
            std::wstring s = p;
            std::wstring lower = s;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
            if (lower.find(L"vid_05a9&pid_058c&mi_00") != std::wstring::npos) return s;
        }
    }
    return {};
}

// OpenCamera: the camera is on a USB 2.0 port and offers none of our modes (an own code, so no
// Media Foundation error can be mistaken for it).
constexpr HRESULT kUsb2Port = MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x0201);

bool IsDeviceLost(HRESULT hr)
{
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG ||
           hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

}  // namespace

CaptureEngine::CaptureEngine()
{
    m_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

CaptureEngine::~CaptureEngine()
{
    Stop();
    CloseHandle(m_wake);
}

ComPtr<IUnknown> CaptureEngine::PhysicalSource()
{
    std::lock_guard lock(m_lock);
    return m_source;
}

void CaptureEngine::HoldPowerLine()
{
    std::lock_guard lock(m_lock);
    if (!m_powerLineHeld) Log(L"an app set the anti-flicker itself: it stands for this stream");
    m_powerLineHeld = true;
    m_flicker.Hold();
    m_powerLineRequest = -1;
}

void CaptureEngine::SetError(const wchar_t* text)
{
    std::lock_guard lock(m_errorLock);
    m_lastError = text;
}

std::wstring CaptureEngine::Error()
{
    std::lock_guard lock(m_errorLock);
    return m_lastError;
}

HRESULT CaptureEngine::Start(const OutputRequest& req, FrameSink sink)
{
    std::lock_guard life(m_lifecycle);
    StopLocked();
    m_req = req;
    m_req.fps = std::clamp<uint32_t>(m_req.fps, 1, 240);
    m_sink = std::move(sink);
    m_frameCount = 0;
    m_framesInWindow = 0;
    m_flickerStarted = false;
    m_powerLineHeld = false;
    m_statusTick = GetTickCount64();
    SetError(L"");
    wchar_t fmt[64];
    swprintf_s(fmt, L"%ux%u %ls @%u", m_req.width, m_req.height, m_req.format == PixelFormat::YUY2 ? L"YUY2" : L"NV12",
        m_req.fps);
    m_formatText = fmt;
    RefreshSettings(true);
    m_running = true;
    m_lastFrameTick = GetTickCount64();
    try {
        m_supervisor = std::thread([this] { Supervisor(); });
    } catch (...) {
        m_running = false;
        return E_OUTOFMEMORY;
    }
    Log(L"capture start: %ls", m_formatText.c_str());
    return S_OK;
}

void CaptureEngine::Stop()
{
    std::lock_guard life(m_lifecycle);
    StopLocked();
}

void CaptureEngine::StopLocked()
{
    if (!m_running.exchange(false)) return;
    SetEvent(m_wake);
    if (m_supervisor.joinable()) m_supervisor.join();
    CloseCamera();
    {
        // Release the GPU device and its ~65 MB of resources while nobody streams.
        std::lock_guard lock(m_lock);
        m_pipeline.reset();
        EndRecording(L"stopped with the stream");
    }
    Status st;
    st.streaming = false;
    SaveStatus(st);
    Log(L"capture stop");
}

void CaptureEngine::ApplyPendingPowerLine()
{
    // A USB control request: sent without m_lock, which the reader thread and app control requests
    // forwarded through PhysicalSource() wait for.
    int value = -1;
    bool flickerSeen = false;
    ComPtr<IMFMediaSource> source;
    {
        std::lock_guard lock(m_lock);
        std::swap(value, m_powerLineRequest);
        flickerSeen = m_flicker.FlickerSeen();
        source = m_source;
    }
    if (value < 0 || !source) return;
    const HRESULT hr = SetPowerLineFrequency(source.Get(), value);
    Log(L"anti-flicker %ls%ls (0x%08lX)", value == kPowerLineOff ? L"off" : value == kPowerLine50 ? L"50 Hz" : L"60 Hz",
        flickerSeen && value == kPowerLine50 ? L", lamp flicker seen" : L"", static_cast<unsigned long>(hr));
}

void CaptureEngine::RefreshSettings(bool force)
{
    ULONGLONG now = GetTickCount64();
    if (!force && now - m_settingsTick < 500) return;
    m_settingsTick = now;
    Settings s = LoadSettings();
    ApplySettings(s, m_effect);
    if (s.antiFlicker != m_antiFlicker || Mains60(s) != m_mains60) {
        m_antiFlicker = s.antiFlicker;
        m_mains60 = Mains60(s);
        if (m_flickerStarted) {  // a change by the user while streaming wins over an app's choice
            m_powerLineHeld = false;
            m_powerLineRequest = m_flicker.Start(static_cast<AntiFlicker>(m_antiFlicker), m_mains60);
        }
    }
    // Taken only while frames flow, so the file is named after the sensor mode actually streaming.
    if (m_readerAlive && !m_recordLeft)
        if (uint32_t frames = TakeRecordRequest()) StartRecording(frames);
    // The tray bumps Request; Handled records the last one we served, so a request made while the
    // camera was idle is honoured at the next stream start.
    if (LoadCalibrationRequest() != LoadCalibrationHandled()) m_calib.Request(m_frameCount);
}

void CaptureEngine::StartRecording(uint32_t frames)
{
    PWSTR programData = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &programData))) {
        Log(L"recording: no ProgramData folder");
        return;
    }
    std::wstring path = std::wstring(programData) + L"\\PS5Camera\\record-" + m_sensorKey + L".raw";
    CoTaskMemFree(programData);
    // The frames show whoever sits at the camera: SYSTEM, admins and this service only, not the
    // users who may read the log folder. A new file, since an existing one would keep its ACL.
    DeleteFileW(path.c_str());
    SECURITY_ATTRIBUTES sa = {sizeof(sa)};
    DWORD error = 0;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;LS)", SDDL_REVISION_1,
            &sa.lpSecurityDescriptor, nullptr)) {
        m_record = CreateFileW(path.c_str(), GENERIC_WRITE, 0, &sa, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        error = GetLastError();
        LocalFree(sa.lpSecurityDescriptor);
    }
    m_recordLeft = m_record != INVALID_HANDLE_VALUE ? frames : 0;
    m_recordFrameBytes = 0;
    if (m_recordLeft) Log(L"recording %u raw frames to %ls", frames, path.c_str());
    else Log(L"recording to %ls failed (error %lu; the old file open elsewhere?)", path.c_str(), error);
}

void CaptureEngine::EndRecording(const wchar_t* why)
{
    if (m_record != INVALID_HANDLE_VALUE) {
        CloseHandle(m_record);
        m_record = INVALID_HANDLE_VALUE;
        Log(L"recording %ls", why);
    }
    m_recordLeft = 0;
}

bool CaptureEngine::EnsurePipeline()
{
    // Called with m_lock held.
    if (m_pipeline) return true;
    ULONGLONG now = GetTickCount64();
    if (now < m_pipelineRetryTick) return false;
    auto p = std::unique_ptr<StereoPipeline>(new (std::nothrow) StereoPipeline());
    HRESULT hr = p ? p->Initialize(m_stereo, m_output) : E_OUTOFMEMORY;
    if (FAILED(hr)) {
        Log(L"GPU pipeline init failed 0x%08lX, retrying", hr);
        SetError(L"GPU unavailable");
        m_pipelineRetryTick = now + 2000;
        return false;
    }
    StoredCalibration cal = m_stereo.mono ? StoredCalibration{} : LoadCalibration(m_sensorKey);
    if (cal.valid) {
        p->SetRectification({cal.dy, cal.rotation, 0});
    } else if (m_stereo.halfSecond) {
        // Same sensors as the full 1080 stereo mode with the roles swapped: its alignment, inverted,
        // is a good start until this mode's own calibration succeeds.
        StoredCalibration full = LoadCalibration(L"1080");
        if (full.valid) {
            p->SetRectification({-full.dy, -full.rotation, 0});
            Log(L"%ls not calibrated yet, starting from the inverted 1080 one: dy %.2f roll %.2f", m_sensorKey,
                -full.dy, -full.rotation);
        }
    }
    // A single sensor has nothing to align; a pending request waits for the next stereo stream.
    m_calib.Start(!m_stereo.mono && (!cal.valid || LoadCalibrationRequest() != LoadCalibrationHandled()), m_frameCount);
    m_pipeline = std::move(p);
    return true;
}

HRESULT CaptureEngine::OpenCamera()
{
    ViewMode view;
    {
        // Settings are only refreshed per delivered frame; while the camera is closed (or failing)
        // the effect may have changed since, and it decides the sensor mode.
        std::lock_guard lock(m_lock);
        RefreshSettings(true);
        // The depth camera needs the second sensor even when the picture does not.
        view = m_depth.Wanted() && m_effect.mode == ViewMode::Main ? ViewMode::Bokeh : m_effect.mode;
    }
    const std::vector<SensorMode> modes = SensorModes(m_req.fps, view);
    const wchar_t* wantedKey = WantedKey(view);
    m_modeChange = false;
    std::wstring link = FindCameraLink();
    if (link.empty()) return HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED);
    ComPtr<IMFAttributes> attr;
    HRESULT hr = MFCreateAttributes(&attr, 2);
    if (FAILED(hr)) return hr;
    attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    attr->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, link.c_str());
    ComPtr<IMFMediaSource> source;
    hr = MFCreateDeviceSource(attr.Get(), &source);
    if (FAILED(hr)) {
        Log(L"MFCreateDeviceSource(%ls) failed 0x%08lX", link.c_str(), hr);
        return hr;
    }

    auto readerLink = std::make_shared<ReaderLink>();
    auto callback = Microsoft::WRL::Make<ReaderCallback>(readerLink);
    ComPtr<IMFAttributes> ra;
    hr = callback ? MFCreateAttributes(&ra, 3) : E_OUTOFMEMORY;
    if (FAILED(hr)) {
        source->Shutdown();
        return hr;
    }
    ra->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, callback.Get());
    ra->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, TRUE);
    ra->SetUINT32(MF_SOURCE_READER_DISCONNECT_MEDIASOURCE_ON_SHUTDOWN, TRUE);
    ComPtr<IMFSourceReader> reader;
    hr = MFCreateSourceReaderFromMediaSource(source.Get(), ra.Get(), &reader);
    if (FAILED(hr)) {
        source->Shutdown();
        return hr;
    }
    auto findType = [&](const SensorMode& m) {
        for (DWORD i = 0;; ++i) {
            ComPtr<IMFMediaType> mt;
            if (FAILED(reader->GetNativeMediaType(kVideoStream, i, &mt))) return ComPtr<IMFMediaType>();
            GUID sub = {};
            UINT32 w = 0, h = 0, num = 0, den = 0;
            mt->GetGUID(MF_MT_SUBTYPE, &sub);
            MFGetAttributeSize(mt.Get(), MF_MT_FRAME_SIZE, &w, &h);
            MFGetAttributeRatio(mt.Get(), MF_MT_FRAME_RATE, &num, &den);
            if (sub == MFVideoFormat_YUY2 && w == m.packedW && h == m.packedH && den &&
                std::lround(double(num) / den) == static_cast<long>(m.fps))
                return mt;
        }
    };
    ComPtr<IMFMediaType> chosen;
    SensorMode mode = modes.front();
    for (const SensorMode& m : modes) {
        chosen = findType(m);
        if (chosen) {
            mode = m;
            break;
        }
        // The camera still runs an older image (firmware lives in RAM until the next replug).
        Log(L"sensor mode %ux%u@%u not offered by the camera", m.packedW, m.packedH, m.fps);
    }
    if (!chosen) {
        // On a USB 2.0 port the firmware only offers its high-speed configuration (640x400@30).
        UINT32 widest = 0;
        for (DWORD i = 0;; ++i) {
            ComPtr<IMFMediaType> mt;
            UINT32 w = 0, h = 0;
            if (FAILED(reader->GetNativeMediaType(kVideoStream, i, &mt))) break;
            MFGetAttributeSize(mt.Get(), MF_MT_FRAME_SIZE, &w, &h);
            widest = std::max(widest, w);
        }
        source->Shutdown();
        if (widest && widest <= 640) {
            Log(L"the camera offers %u pixels at most: it is on a USB 2.0 port, plug it into USB 3", widest);
            return kUsb2Port;
        }
        return MF_E_INVALIDMEDIATYPE;
    }
    hr = reader->SetCurrentMediaType(kVideoStream, nullptr, chosen.Get());
    if (FAILED(hr)) {
        source->Shutdown();
        return hr;
    }

    const StereoFormat sf = FormatOf(mode);
    const OutputFormat of = OutputFor(sf, m_req.width, m_req.height, m_req.format);
    {
        std::lock_guard lock(m_lock);
        bool same = m_pipeline && m_stereo.eyeWidth == sf.eyeWidth && m_stereo.eyeHeight == sf.eyeHeight &&
                    m_stereo.mono == sf.mono && m_stereo.halfSecond == sf.halfSecond &&
                    m_output.width == of.width && m_output.height == of.height && m_output.format == of.format;
        m_stereo = sf;
        m_output = of;
        m_sensorKey = mode.key;
        m_wantedKey = wantedKey;
        if (!same) {
            m_pipeline.reset();
            m_pipelineRetryTick = 0;
        }
        if (m_pipeline) m_pipeline->Reset();
        EnsurePipeline();
        m_source = source;
        // Once per stream; a reopen for another sensor mode keeps what the guard found, on the new
        // source. An app's own choice stands.
        if (m_powerLineHeld) {
            m_flicker.Hold();
        } else if (!m_flickerStarted) {
            m_flickerStarted = true;
            m_powerLineRequest = m_flicker.Start(static_cast<AntiFlicker>(m_antiFlicker), m_mains60);
        } else {
            m_powerLineRequest = m_flicker.Value();
        }
    }
    ApplyPendingPowerLine();
    {
        std::lock_guard lock(readerLink->m);
        readerLink->engine = this;
        readerLink->reader = reader;
    }
    m_link = readerLink;
    m_readerAlive = true;
    m_lastFrameTick = GetTickCount64();
    hr = reader->ReadSample(kVideoStream, 0, nullptr, nullptr, nullptr, nullptr);
    Log(L"camera opened in %ux%u@%u for %ls (0x%08lX)", mode.packedW, mode.packedH, mode.fps, m_formatText.c_str(), hr);
    return hr;
}

void CaptureEngine::CloseCamera()
{
    std::shared_ptr<ReaderLink> link = std::move(m_link);
    m_readerAlive = false;
    ComPtr<IMFSourceReader> reader;
    if (link) {
        // Detach first: waits for a callback that is processing a frame and stops it re-arming.
        std::lock_guard lock(link->m);
        link->engine = nullptr;
        reader = std::move(link->reader);
    }
    if (reader) {
        ResetEvent(link->flushed);
        if (SUCCEEDED(reader->Flush(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS))))
            WaitForSingleObject(link->flushed, 2000);
        reader.Reset();
    }
    ComPtr<IMFMediaSource> source;
    {
        std::lock_guard lock(m_lock);
        source = std::move(m_source);
    }
    if (source) source->Shutdown();
}

bool CaptureEngine::OnReadSample(HRESULT hr, DWORD flags, IMFSample* sample)
{
    if (!m_running) return false;
    if (FAILED(hr) || (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM))) {
        Log(L"camera stream ended (hr 0x%08lX flags 0x%lX)", hr, flags);
        SetError(L"camera stream ended");
        m_readerAlive = false;
        SetEvent(m_wake);
        return false;
    }
    if (sample) {
        ProcessFrame(sample);
        m_lastFrameTick = GetTickCount64();
    }
    return m_running.load();
}

void CaptureEngine::ProcessFrame(IMFSample* sample)
{
    HandleFrame(sample);
    ApplyPendingPowerLine();
}

void CaptureEngine::HandleFrame(IMFSample* sample)
{
    ComPtr<IMFMediaBuffer> buf;
    if (FAILED(sample->GetBufferByIndex(0, &buf))) return;
    std::lock_guard lock(m_lock);
    if (!EnsurePipeline()) {
        DeliverPlaceholder();
        return;
    }
    const uint32_t rowBytes = m_stereo.PackedWidth() * 2;  // YUY2: 2 bytes per pixel
    const uint32_t rows = m_stereo.PackedHeight();
    const size_t need = size_t(rowBytes) * (rows - 1) + rowBytes;
    ComPtr<IMF2DBuffer2> b2;
    BYTE* scan0 = nullptr;
    BYTE* start = nullptr;
    LONG pitch = 0;
    DWORD len = 0;
    bool twoD = SUCCEEDED(buf.As(&b2)) && SUCCEEDED(b2->Lock2DSize(MF2DBuffer_LockFlags_Read, &scan0, &pitch, &start, &len));
    if (!twoD) {
        if (FAILED(buf->Lock(&scan0, nullptr, &len))) return;
        start = scan0;
        pitch = static_cast<LONG>(rowBytes);
    }
    // Reject bottom-up or truncated buffers instead of reading past their end.
    size_t available = scan0 && start ? size_t(start + len - scan0) : 0;
    bool ok = pitch >= static_cast<LONG>(rowBytes) && available >= size_t(pitch) * (rows - 1) + rowBytes &&
              available >= need;
    if (ok && m_recordLeft) {
        // Debug aid: ~5 MB per frame written synchronously, so a few frames get dropped meanwhile.
        const uint32_t frameBytes = rowBytes * rows;
        if (!m_recordFrameBytes) m_recordFrameBytes = frameBytes;
        bool written = frameBytes == m_recordFrameBytes;
        const bool packed = pitch == static_cast<LONG>(rowBytes);  // usually: the whole frame in one write
        for (uint32_t y = 0; written && y < (packed ? 1 : rows); ++y) {
            const DWORD bytes = packed ? frameBytes : rowBytes;
            DWORD done = 0;
            written = WriteFile(m_record, scan0 + size_t(pitch) * y, bytes, &done, nullptr) && done == bytes;
        }
        if (!written) EndRecording(L"stopped (sensor mode changed or disk error)");
        else if (--m_recordLeft == 0) EndRecording(L"finished");
    }
    if (ok) {
        Deliver(scan0, static_cast<uint32_t>(pitch));
    } else if (m_badFrames++ % 300 == 0) {
        Log(L"unexpected camera buffer (pitch %ld, %lu bytes), frame skipped", pitch, len);
    }
    if (twoD) b2->Unlock2D();
    else buf->Unlock();
}

IMFSample* CaptureEngine::NewOutputSample(BYTE** scan0, LONG* pitch, ComPtr<IMF2DBuffer2>& lockOut)
{
    DWORD fourcc = m_req.format == PixelFormat::YUY2 ? MFVideoFormat_YUY2.Data1 : MFVideoFormat_NV12.Data1;
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(MFCreate2DMediaBuffer(m_req.width, m_req.height, fourcc, FALSE, &buffer))) return nullptr;
    if (FAILED(buffer.As(&lockOut))) return nullptr;
    BYTE* start = nullptr;
    DWORD len = 0;
    if (FAILED(lockOut->Lock2DSize(MF2DBuffer_LockFlags_Write, scan0, pitch, &start, &len))) return nullptr;
    // NV12 needs height rows of Y plus height/2 rows of UV with the same pitch, laid out contiguously.
    size_t rows = m_req.format == PixelFormat::YUY2 ? m_req.height : m_req.height + m_req.height / 2;
    if (*pitch <= 0 || size_t(start + len - *scan0) < size_t(*pitch) * rows) {
        lockOut->Unlock2D();
        return nullptr;
    }
    ComPtr<IMFSample> sample;
    if (FAILED(MFCreateSample(&sample)) || FAILED(sample->AddBuffer(buffer.Get()))) {
        lockOut->Unlock2D();
        return nullptr;
    }
    return sample.Detach();
}

void CaptureEngine::Deliver(const uint8_t* yuy2, uint32_t pitch)
{
    // Called with m_lock held and m_pipeline present.
    RefreshSettings(false);
    const wchar_t* wanted = m_depth.Wanted() ? kHalfKey : WantedKey(m_effect.mode);
    if (wcscmp(wanted, m_wantedKey) != 0 && !m_modeChange.exchange(true))
        SetEvent(m_wake);  // e.g. blur switched on while streaming one sensor: the supervisor reopens
    ++m_frameCount;
    if (!m_stereo.mono && m_calib.Due(m_frameCount)) {
        Rectification r;
        uint32_t request = LoadCalibrationRequest();
        if (SUCCEEDED(m_pipeline->Calibrate(yuy2, pitch, &r))) {
            m_calib.Succeeded();
            SaveCalibration(m_sensorKey, {true, r.dy, r.rotation});
            SaveCalibrationHandled(request);
            Log(L"calibrated %ls: dy %.2f roll %.2f (score %.2f)", m_sensorKey, r.dy, r.rotation, r.quality);
        } else {
            m_calib.Failed(m_frameCount, m_req.fps);  // dark or featureless scene
        }
    }
    MainRowMeans(yuy2, pitch, m_stereo, m_rowMeans);
    if (const int pl = m_flicker.Update(m_rowMeans); pl >= 0) m_powerLineRequest = pl;
    BYTE* scan0 = nullptr;
    LONG outPitch = 0;
    ComPtr<IMF2DBuffer2> lock;
    ComPtr<IMFSample> out;
    out.Attach(NewOutputSample(&scan0, &outPitch, lock));
    if (!out) return;
    FrameStats st;
    DepthPlane* plane = m_depth.Plane(m_req.width, m_req.height);
    EffectSettings effect = m_effect;
    effect.depthPlane = plane != nullptr;
    HRESULT hr = m_pipeline->Process(yuy2, pitch, effect, scan0, scan0 + size_t(outPitch) * m_req.height,
        static_cast<uint32_t>(outPitch), &st, plane);
    lock->Unlock2D();
    if (FAILED(hr)) {
        // A lost device (driver update, TDR) never recovers by itself: rebuild the pipeline.
        if (m_pipelineFailures++ % 300 == 0)
            Log(L"pipeline failed 0x%08lX%ls, rebuilding", hr, IsDeviceLost(hr) ? L" (GPU device lost)" : L"");
        m_pipeline.reset();
        m_pipelineRetryTick = GetTickCount64() + 500;
        SetError(L"GPU restarting");
        DeliverPlaceholder();
        return;
    }
    m_pipelineFailures = 0;
    if (hr == S_FALSE) return;  // first frame primes the GPU pipeline
    m_lastGpuMs = st.gpuMs;
    m_lastFocus = st.focusDisparity;
    const LONGLONG now = MFGetSystemTime();
    m_depth.Publish(now);
    out->SetSampleTime(now);
    out->SetSampleDuration(10'000'000LL / m_req.fps);
    ++m_framesInWindow;
    PublishStatus();
    if (m_sink) m_sink(out.Get());
}

void CaptureEngine::DeliverPlaceholder()
{
    // Dark slate with a slowly moving bar, so apps keep running and the user sees "no camera".
    BYTE* scan0 = nullptr;
    LONG pitch = 0;
    ComPtr<IMF2DBuffer2> lock;
    ComPtr<IMFSample> out;
    out.Attach(NewOutputSample(&scan0, &pitch, lock));
    if (!out) return;
    const uint32_t w = m_req.width, h = m_req.height;
    uint32_t bar = static_cast<uint32_t>((GetTickCount64() / 20) % h);
    if (m_req.format == PixelFormat::YUY2) {
        for (uint32_t y = 0; y < h; ++y) {
            uint8_t luma = (y >= bar && y < bar + 6) ? 60 : 24;
            uint8_t* row = scan0 + size_t(y) * pitch;
            for (uint32_t x = 0; x < w; x += 2) {
                row[x * 2 + 0] = luma;
                row[x * 2 + 1] = 128;
                row[x * 2 + 2] = luma;
                row[x * 2 + 3] = 128;
            }
        }
    } else {
        for (uint32_t y = 0; y < h; ++y) memset(scan0 + size_t(y) * pitch, (y >= bar && y < bar + 6) ? 60 : 24, w);
        uint8_t* uv = scan0 + size_t(pitch) * h;
        for (uint32_t y = 0; y < h / 2; ++y) memset(uv + size_t(y) * pitch, 128, w);
    }
    lock->Unlock2D();
    out->SetSampleTime(MFGetSystemTime());
    out->SetSampleDuration(10'000'000LL / m_req.fps);
    if (m_sink) m_sink(out.Get());
}

void CaptureEngine::PublishStatus()
{
    ULONGLONG now = GetTickCount64();
    ULONGLONG since = m_statusTick;
    if (now - since < 1000) return;
    Status st;
    st.streaming = true;
    st.fpsX100 = static_cast<uint32_t>(m_framesInWindow * 100000ULL / (now - since));
    st.gpuUs = static_cast<uint32_t>(m_lastGpuMs * 1000);
    st.focusX100 = static_cast<int32_t>(m_lastFocus * 100);
    st.format = m_formatText;
    st.error = m_readerAlive ? std::wstring() : Error();
    SaveStatus(st);
    m_statusTick = now;
    m_framesInWindow = 0;
}

void CaptureEngine::Supervisor()
{
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    ULONGLONG nextOpen = 0;
    const DWORD frameMs = 1000 / m_req.fps;
    while (m_running) {
        ULONGLONG now = GetTickCount64();
        bool alive = m_readerAlive;
        if (alive && now - m_lastFrameTick > 3000) {
            Log(L"no frames for 3 s, reopening camera");
            SetError(L"camera stalled");
            alive = false;
        }
        if (alive && m_modeChange.exchange(false)) {
            Log(L"effect needs another sensor mode, reopening camera");
            alive = false;
        }
        if (!alive) {
            if (m_link) CloseCamera();
            if (now >= nextOpen) {
                HRESULT hr = OpenCamera();
                if (FAILED(hr)) {
                    SetError(hr == HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED) ? L"camera not connected"
                             : hr == kUsb2Port                                     ? L"camera on USB 2.0: use a USB 3 port"
                                                                                   : L"camera open failed");
                    nextOpen = now + 2000;
                }
                continue;
            }
            {
                std::lock_guard lock(m_lock);
                DeliverPlaceholder();
            }
            if (now - m_statusTick >= 1000) {
                Status st;
                st.streaming = true;
                st.format = m_formatText;
                st.error = Error();
                SaveStatus(st);
                m_statusTick = now;
            }
            WaitForSingleObject(m_wake, frameMs);
            continue;
        }
        WaitForSingleObject(m_wake, 250);
    }
    MFShutdown();
    if (SUCCEEDED(co)) CoUninitialize();
}

}  // namespace ps5cam
