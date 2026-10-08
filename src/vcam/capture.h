#pragma once
// Owns the physical PS5 camera while the virtual camera streams: opens it in the stereo mode that
// matches the requested output (or the single-sensor 1920x1080 mode when no depth effect is on),
// runs the GPU pipeline on every frame and hands finished NV12/YUY2
// samples to the stream. Falls back to a placeholder picture while the camera (or the GPU) is
// unavailable. Everything here runs inside the Frame Server service, so failures must degrade, not
// crash.
#include <windows.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "pipeline.h"
#include "../common/settings.h"

namespace ps5cam {

struct OutputRequest {
    uint32_t width = 1920;
    uint32_t height = 1080;
    uint32_t fps = 30;
    PixelFormat format = PixelFormat::NV12;
};

struct ReaderLink;  // shared with the source-reader callback, outlives the engine if needed

class CaptureEngine {
public:
    using FrameSink = std::function<void(IMFSample*)>;

    CaptureEngine();
    ~CaptureEngine();

    HRESULT Start(const OutputRequest& req, FrameSink sink);
    void Stop();

    // For forwarding UVC controls (brightness etc.) to the physical camera.
    Microsoft::WRL::ComPtr<IUnknown> PhysicalSource();

    // Called by the reader callback while the engine is attached; returns true to read the next frame.
    bool OnReadSample(HRESULT hr, DWORD flags, IMFSample* sample);

private:
    void StopLocked();
    HRESULT OpenCamera();
    void CloseCamera();
    void Supervisor();
    void ProcessFrame(IMFSample* sample);
    void Deliver(const uint8_t* yuy2, uint32_t pitch);
    void DeliverPlaceholder();
    bool EnsurePipeline();
    IMFSample* NewOutputSample(BYTE** scan0, LONG* pitch, Microsoft::WRL::ComPtr<IMF2DBuffer2>& lockOut);
    void RefreshSettings(bool force);
    void StartRecording(uint32_t frames);
    void EndRecording(const wchar_t* why);
    void PublishStatus();
    void SetError(const wchar_t* text);
    std::wstring Error();

    std::mutex m_lifecycle;  // serialises Start/Stop
    OutputRequest m_req;
    FrameSink m_sink;

    std::mutex m_lock;  // guards source/pipeline and per-frame state
    Microsoft::WRL::ComPtr<IMFMediaSource> m_source;
    std::shared_ptr<ReaderLink> m_link;  // touched only by the supervisor thread and Stop
    std::unique_ptr<StereoPipeline> m_pipeline;
    StereoFormat m_stereo;
    OutputFormat m_output;
    ULONGLONG m_pipelineRetryTick = 0;
    uint32_t m_pipelineFailures = 0;
    const wchar_t* m_sensorKey = L"1080";  // sensor mode actually streaming (calibration key)
    const wchar_t* m_wantedKey = L"1080";  // mode the effect asked for (differs after a fallback)

    std::atomic<bool> m_running = false;
    std::atomic<bool> m_readerAlive = false;
    std::atomic<bool> m_modeChange = false;  // the effect now needs another sensor mode: reopen
    std::atomic<ULONGLONG> m_lastFrameTick = 0;
    HANDLE m_wake = nullptr;
    std::thread m_supervisor;

    EffectSettings m_effect;
    ULONGLONG m_settingsTick = 0;
    bool m_calibPending = false;
    uint32_t m_calibFailures = 0;
    uint32_t m_nextCalibFrame = 10;
    uint32_t m_frameCount = 0;
    uint32_t m_badFrames = 0;
    // Raw frames for tuning (ps5cam-ctl record N), written on the reader thread under m_lock.
    HANDLE m_record = INVALID_HANDLE_VALUE;
    uint32_t m_recordLeft = 0;
    uint32_t m_recordFrameBytes = 0;  // frame size the file started with; another one ends it

    // Status accounting.
    std::atomic<ULONGLONG> m_statusTick = 0;
    uint32_t m_framesInWindow = 0;
    float m_lastGpuMs = 0;
    float m_lastFocus = 0;
    std::mutex m_errorLock;
    std::wstring m_lastError;
    std::wstring m_formatText;
};

}  // namespace ps5cam
