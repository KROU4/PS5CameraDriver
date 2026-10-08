#include "processor.h"

#include <mfapi.h>
#include <mferror.h>

#include <algorithm>
#include <cmath>

#include "../common/effect.h"
#include "../common/log.h"
#include "../common/settings.h"

using Microsoft::WRL::ComPtr;

namespace ps5cam {

namespace {

bool SizeAndRate(IMFMediaType* t, GUID* sub, UINT32* w, UINT32* h, uint32_t* fps)
{
    UINT32 num = 0, den = 0;
    if (FAILED(t->GetGUID(MF_MT_SUBTYPE, sub)) || FAILED(MFGetAttributeSize(t, MF_MT_FRAME_SIZE, w, h)) ||
        FAILED(MFGetAttributeRatio(t, MF_MT_FRAME_RATE, &num, &den)) || !den)
        return false;
    *fps = static_cast<uint32_t>(std::lround(double(num) / den));
    return true;
}

bool IsDeviceLost(HRESULT hr)
{
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG ||
           hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}

}  // namespace

bool FrameProcessor::Configure(IMFMediaType* input, IMFMediaType* output)
{
    GUID inSub = {}, outSub = {};
    UINT32 inW = 0, inH = 0, outW = 0, outH = 0;
    uint32_t inFps = 0, outFps = 0;
    SensorMode mode;
    if (!input || !output || !SizeAndRate(input, &inSub, &inW, &inH, &inFps) ||
        !SizeAndRate(output, &outSub, &outW, &outH, &outFps) || inSub != MFVideoFormat_YUY2 ||
        (outSub != MFVideoFormat_NV12 && outSub != MFVideoFormat_YUY2) || !SensorModeOf(inW, inH, inFps, &mode)) {
        m_configured = false;  // no frames from a half-known format
        m_pipeline.reset();
        return false;
    }
    m_configured = true;
    const StereoFormat sf = FormatOf(mode);
    const PixelFormat pf = outSub == MFVideoFormat_YUY2 ? PixelFormat::YUY2 : PixelFormat::NV12;
    const OutputFormat of = OutputFor(sf, outW, outH, pf);
    bool same = m_pipeline && m_mode.packedW == mode.packedW && m_mode.packedH == mode.packedH &&
                m_output.width == of.width && m_output.height == of.height && m_output.format == of.format;
    m_mode = mode;
    m_stereo = sf;
    m_output = of;
    m_fps = outFps;
    if (!same) m_pipeline.reset();
    else m_pipeline->Reset();
    m_retryTick = 0;
    m_lastTime = -1;
    wchar_t text[64];
    swprintf_s(text, L"%ux%u %ls @%u", outW, outH, pf == PixelFormat::YUY2 ? L"YUY2" : L"NV12", outFps);
    m_formatText = text;
    RefreshSettings(true);
    Log(L"processing %ux%u@%u (%ls) into %ls", inW, inH, inFps, mode.key, text);
    if (!m_flickerStarted) {  // not again when the effect only switches the sensor mode
        m_flickerStarted = true;
        if (m_powerLineHeld) m_flicker.Hold();  // the app chose before starting
        else m_powerLineRequest = m_flicker.Start(static_cast<AntiFlicker>(m_antiFlicker), m_mains60);
    }
    return true;
}

void FrameProcessor::HoldPowerLine()
{
    if (!m_powerLineHeld) Log(L"an app set the anti-flicker itself: it stands for this stream");
    m_powerLineHeld = true;
    m_flicker.Hold();
    m_powerLineRequest = -1;
}

int FrameProcessor::TakePowerLineRequest()
{
    const int v = m_powerLineRequest;
    m_powerLineRequest = -1;
    return v;
}

void FrameProcessor::Reset()
{
    m_pipeline.reset();  // releases the GPU device and its ~65 MB while nobody streams
    m_lastTime = -1;
    m_flickerStarted = false;
    m_powerLineHeld = false;
    if (!m_configured) return;  // a transform that never streamed: another one may be streaming now
    m_configured = false;
    Status st;
    st.streaming = false;
    SaveStatus(st);
}

const wchar_t* FrameProcessor::WantedKey()
{
    RefreshSettings(false);
    // The depth camera needs the second sensor even when the picture does not.
    return m_depth.Wanted() ? kHalfKey : ps5cam::WantedKey(m_effect.mode);
}

void FrameProcessor::RefreshSettings(bool force)
{
    ULONGLONG now = GetTickCount64();
    if (!force && now - m_settingsTick < 500) return;
    m_settingsTick = now;
    Settings s = LoadSettings();
    ApplySettings(s, m_effect);
    const bool mains60 = Mains60(s);
    if (!m_settingsLoaded) {
        m_settingsLoaded = true;  // the first reading is no change of the setting
        m_antiFlicker = s.antiFlicker;
        m_mains60 = mains60;
    } else if (s.antiFlicker != m_antiFlicker || mains60 != m_mains60) {
        m_antiFlicker = s.antiFlicker;
        m_mains60 = mains60;
        m_powerLineHeld = false;  // the user's choice now
        if (m_flickerStarted) m_powerLineRequest = m_flicker.Start(static_cast<AntiFlicker>(m_antiFlicker), m_mains60);
    }
    // The tray bumps Request; Handled records the last one served.
    if (LoadCalibrationRequest() != LoadCalibrationHandled() && !m_calibPending) {
        m_calibPending = true;
        m_calibFailures = 0;
        m_nextCalibFrame = m_frameCount + 10;
    }
}

bool FrameProcessor::EnsurePipeline()
{
    if (m_pipeline) return true;
    ULONGLONG now = GetTickCount64();
    if (now < m_retryTick) return false;
    auto p = std::unique_ptr<StereoPipeline>(new (std::nothrow) StereoPipeline());
    HRESULT hr = p ? p->Initialize(m_stereo, m_output) : E_OUTOFMEMORY;
    if (FAILED(hr)) {
        Log(L"GPU pipeline init failed 0x%08lX, retrying", hr);
        m_retryTick = now + 2000;
        return false;
    }
    StoredCalibration cal = m_stereo.mono ? StoredCalibration{} : LoadCalibration(m_mode.key);
    if (cal.valid) {
        p->SetRectification({cal.dy, cal.rotation, 0});
    } else if (m_stereo.halfSecond) {
        // Same sensors as the full 1080 stereo mode with the roles swapped: its alignment, inverted,
        // is a good start until this mode's own calibration succeeds.
        StoredCalibration full = LoadCalibration(L"1080");
        if (full.valid) p->SetRectification({-full.dy, -full.rotation, 0});
    }
    m_calibPending = !m_stereo.mono && (!cal.valid || LoadCalibrationRequest() != LoadCalibrationHandled());
    m_calibFailures = 0;
    m_nextCalibFrame = m_frameCount + 10;
    m_pipeline = std::move(p);
    return true;
}

void FrameProcessor::Calibrate(const uint8_t* yuy2, uint32_t pitch)
{
    if (!m_calibPending || m_stereo.mono || m_frameCount < m_nextCalibFrame) return;
    Rectification r;
    uint32_t request = LoadCalibrationRequest();
    if (SUCCEEDED(m_pipeline->Calibrate(yuy2, pitch, &r))) {
        m_calibPending = false;
        m_calibFailures = 0;
        SaveCalibration(m_mode.key, {true, r.dy, r.rotation});
        SaveCalibrationHandled(request);
        Log(L"calibrated %ls: dy %.2f roll %.2f (score %.2f)", m_mode.key, r.dy, r.rotation, r.quality);
    } else {
        // Dark or featureless scene: back off (1 s, 2 s, 4 s ... up to 16 s at 30 fps).
        m_calibFailures = std::min<uint32_t>(m_calibFailures + 1, 5);
        m_nextCalibFrame = m_frameCount + (30u << (m_calibFailures - 1)) * std::max<uint32_t>(m_fps / 30, 1);
    }
}

IMFSample* FrameProcessor::NewOutputSample(BYTE** scan0, LONG* pitch, ComPtr<IMF2DBuffer2>& lockOut)
{
    const bool yuy2 = m_output.format == PixelFormat::YUY2;
    DWORD fourcc = yuy2 ? MFVideoFormat_YUY2.Data1 : MFVideoFormat_NV12.Data1;
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(MFCreate2DMediaBuffer(m_output.width, m_output.height, fourcc, FALSE, &buffer))) return nullptr;
    if (FAILED(buffer.As(&lockOut))) return nullptr;
    BYTE* start = nullptr;
    DWORD len = 0;
    if (FAILED(lockOut->Lock2DSize(MF2DBuffer_LockFlags_Write, scan0, pitch, &start, &len))) return nullptr;
    // NV12 needs height rows of Y plus height/2 rows of UV with the same pitch, laid out contiguously.
    size_t rows = yuy2 ? m_output.height : m_output.height + m_output.height / 2;
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

ComPtr<IMFSample> FrameProcessor::Process(IMFSample* input)
{
    ComPtr<IMFMediaBuffer> buf;
    if (!m_configured || !input || FAILED(input->GetBufferByIndex(0, &buf))) return nullptr;
    RefreshSettings(false);
    if (!EnsurePipeline()) return nullptr;
    const uint32_t rowBytes = m_stereo.PackedWidth() * 2;  // YUY2: 2 bytes per pixel
    const uint32_t rows = m_stereo.PackedHeight();
    ComPtr<IMF2DBuffer2> b2;
    BYTE* scan0 = nullptr;
    BYTE* start = nullptr;
    LONG pitch = 0;
    DWORD len = 0;
    bool twoD = SUCCEEDED(buf.As(&b2)) && SUCCEEDED(b2->Lock2DSize(MF2DBuffer_LockFlags_Read, &scan0, &pitch, &start, &len));
    if (!twoD) {
        if (FAILED(buf->Lock(&scan0, nullptr, &len))) return nullptr;
        start = scan0;
        pitch = static_cast<LONG>(rowBytes);
    }
    // Reject bottom-up or truncated buffers instead of reading past their end, and frames of another
    // sensor mode (the camera switches a moment before SetInputStreamState tells us).
    size_t available = scan0 && start ? size_t(start + len - scan0) : 0;
    const bool otherMode = twoD ? pitch > static_cast<LONG>(rowBytes) + 256
                                : available > size_t(rowBytes) * rows + rowBytes;
    ComPtr<IMFSample> out;
    if (!otherMode && pitch >= static_cast<LONG>(rowBytes) && available >= size_t(pitch) * (rows - 1) + rowBytes) {
        ++m_frameCount;
        Calibrate(scan0, static_cast<uint32_t>(pitch));
        MainRowMeans(scan0, static_cast<uint32_t>(pitch), m_stereo, m_rowMeans);
        if (const int pl = m_flicker.Update(m_rowMeans); pl >= 0) {
            m_powerLineRequest = pl;
            Log(pl == kPowerLineOff ? L"dim scene: anti-flicker off for a longer exposure"
                                    : L"lamp flicker seen (score %.4f): anti-flicker back to 50 Hz",
                m_flicker.Score());
        }
        BYTE* dst = nullptr;
        LONG dstPitch = 0;
        ComPtr<IMF2DBuffer2> lock;
        out.Attach(NewOutputSample(&dst, &dstPitch, lock));
        if (out) {
            FrameStats st;
            DepthPlane* plane = m_depth.Plane(m_output.width, m_output.height);
            EffectSettings effect = m_effect;
            effect.depthPlane = plane != nullptr;
            HRESULT hr = m_pipeline->Process(scan0, static_cast<uint32_t>(pitch), effect, dst,
                dst + size_t(dstPitch) * m_output.height, static_cast<uint32_t>(dstPitch), &st, plane);
            lock->Unlock2D();
            LONGLONG time = 0, previous = m_lastTime;
            m_lastTime = SUCCEEDED(input->GetSampleTime(&time)) ? time : MFGetSystemTime();
            if (FAILED(hr)) {
                // A lost device (driver update, TDR) never recovers by itself: rebuild the pipeline.
                if (m_failures++ % 300 == 0)
                    Log(L"pipeline failed 0x%08lX%ls, rebuilding", hr, IsDeviceLost(hr) ? L" (GPU device lost)" : L"");
                m_pipeline.reset();
                m_retryTick = GetTickCount64() + 500;
                out.Reset();
            } else if (hr == S_FALSE || previous < 0) {
                out.Reset();  // the first frame only primes the GPU pipeline
            } else {
                m_failures = 0;
                m_lastGpuMs = st.gpuMs;
                m_lastFocus = st.focusDisparity;
                out->SetSampleTime(previous);
                m_depth.Publish(previous);
                out->SetSampleDuration(10'000'000LL / std::max<uint32_t>(m_fps, 1));
                ++m_framesInWindow;
                PublishStatus();
            }
        }
    }
    if (twoD) b2->Unlock2D();
    else buf->Unlock();
    return out;
}

void FrameProcessor::PublishStatus()
{
    ULONGLONG now = GetTickCount64();
    if (now - m_statusTick < 1000) return;
    Status st;
    st.streaming = true;
    st.fpsX100 = m_statusTick ? static_cast<uint32_t>(m_framesInWindow * 100000ULL / (now - m_statusTick)) : 0;
    st.gpuUs = static_cast<uint32_t>(m_lastGpuMs * 1000);
    st.focusX100 = static_cast<int32_t>(m_lastFocus * 100);
    st.format = m_formatText;
    SaveStatus(st);
    m_statusTick = now;
    m_framesInWindow = 0;
}

}  // namespace ps5cam
