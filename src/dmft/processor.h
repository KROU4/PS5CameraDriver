#pragma once
// The GPU effect for the device MFT: takes camera frames (YUY2 in one of the sensor modes of
// cameramodes.h) and returns finished NV12/YUY2 samples of the output type the app chose. Settings
// come from the registry like for the virtual camera, so the tray and ps5cam-ctl work unchanged.
#include <windows.h>
#include <mfidl.h>
#include <wrl/client.h>

#include <memory>
#include <string>

#include "cameramodes.h"
#include "pipeline.h"

namespace ps5cam {

class FrameProcessor {
public:
    // Input: the camera's UVC format; output: one of OutputTypes(). False if either is not ours.
    bool Configure(IMFMediaType* input, IMFMediaType* output);
    void Reset();  // stream stopped: drop the GPU pipeline and temporal state

    // The processed frame, one frame late (the GPU pipeline overlaps frames), or null.
    Microsoft::WRL::ComPtr<IMFSample> Process(IMFSample* input);

    // The sensor mode the current settings want; differs from the input after the effect changed.
    const wchar_t* WantedKey();
    const wchar_t* InputKey() const { return m_configured ? m_mode.key : L""; }
    bool Configured() const { return m_configured; }

private:
    bool EnsurePipeline();
    void RefreshSettings(bool force);
    void Calibrate(const uint8_t* yuy2, uint32_t pitch);
    IMFSample* NewOutputSample(BYTE** scan0, LONG* pitch, Microsoft::WRL::ComPtr<IMF2DBuffer2>& lockOut);
    void PublishStatus();

    bool m_configured = false;  // Configure succeeded since the last Reset
    SensorMode m_mode = {};
    StereoFormat m_stereo;
    OutputFormat m_output;
    uint32_t m_fps = 30;
    std::unique_ptr<StereoPipeline> m_pipeline;
    ULONGLONG m_retryTick = 0;
    uint32_t m_failures = 0;

    EffectSettings m_effect;
    ULONGLONG m_settingsTick = 0;
    bool m_calibPending = false;
    uint32_t m_calibFailures = 0;
    uint32_t m_nextCalibFrame = 10;
    uint32_t m_frameCount = 0;
    LONGLONG m_lastTime = -1;  // time stamp of the frame the pipeline holds

    ULONGLONG m_statusTick = 0;
    uint32_t m_framesInWindow = 0;
    float m_lastGpuMs = 0, m_lastFocus = 0;
    std::wstring m_formatText;
};

}  // namespace ps5cam
