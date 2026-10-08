#pragma once
// GPU stereo pipeline: packed YUY2 from the PS5 HD Camera in (side by side, one sensor, or the
// firmware e9 frame with the second sensor at half size), NV12 or YUY2 out.
// Depth comes from census + 4-path SGM on the two sensors, the main sensor image is shown
// with a disparity-driven depth-of-field (bokeh) effect, or one of the diagnostic views.
// Runs on Direct3D 11 on Windows and on Vulkan elsewhere (gpu.h).
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "platform.h"

namespace ps5cam {

// DebugRaw / DebugFilled / DebugHoles (bench only): disparity after SGM and the confidence tests in
// wta.hlsl, after the left-right check and the fill of occlusions (before the 2D hole fill), and
// after the hole fill and temporal smoothing (before the guided filter). DebugGrey: the final
// disparity as grey levels for measurements (Y = 16 + 219 d / 64).
enum class ViewMode : uint32_t {
    Bokeh = 0, Main = 1, Second = 2, Depth = 3, SideBySide = 4, DebugRaw = 5, DebugFilled = 6, DebugHoles = 7,
    DebugGrey = 8
};
enum class PixelFormat : uint32_t { NV12 = 0, YUY2 = 1 };

struct EffectSettings {
    ViewMode mode = ViewMode::Bokeh;
    float blurStrength = 0.6f;    // 0..1
    bool autoFocus = true;
    float manualFocus = 0.5f;     // 0 (far) .. 1 (near), used when autoFocus is off
    float focusRange = 1.0f;      // sharp zone half-width in working disparity units
    float foregroundBlur = 0.35f; // relative blur for objects in front of the focus plane
    float temporal = 0.4f;        // weight of the newest disparity (1 = no smoothing)
    float highlights = 1.5f;      // bokeh highlight emphasis
    bool autoBrightness = true;   // digital exposure compensation for dim rooms
    float maxGain = 6.0f;
    float denoise = 0.7f;         // temporal noise reduction of the image, 0 (off) .. 1
    bool depthPlane = false;      // also render the depth camera's plane (computes depth in any view)
    uint32_t depthView = 0;       // that plane: 0 disparity (near = bright), 1 subject matte
};

// The depth camera's plane of a delivered frame: one byte per output pixel, studio range.
struct DepthPlane {
    uint8_t* data = nullptr;  // the caller's buffer, Output().height rows of Output().width bytes
    uint32_t pitch = 0;
    bool written = false;     // set by Process when the delivered frame has a plane
};

struct StereoFormat {
    uint32_t eyeWidth = 1920;   // main sensor (a multiple of 8 for halfSecond)
    uint32_t eyeHeight = 1080;  // (even for halfSecond)
    bool mainIsSecondHalf = true;  // side by side only: packed frame is [right stream | left stream]
    bool mono = false;             // input is one sensor only (eyeWidth wide): no depth, Main view only
    // Firmware e9 frame (the bridge's merger sends the main sensor and the first level of the second
    // sensor's image pyramid, an exact 2x2 average): every line starts with a kHalfHeaderPixels header,
    // then the main sensor (the left stream) at full size, then the second sensor at half size as
    // YUY2 with each of its rows folded over two frame lines, starting at line 1. eyeHeight + 8 lines.
    bool halfSecond = false;

    static constexpr uint32_t kHalfHeaderPixels = 48;
    static constexpr uint32_t kHalfExtraLines = 8;

    // The depth passes are written for the right stream as main (see shaders/downscale.hlsl).
    bool MainIsRightStream() const { return !mono && !halfSecond && mainIsSecondHalf; }
    bool Valid() const
    {
        return eyeWidth >= 64 && eyeHeight >= 64 && eyeWidth % 2 == 0 &&
               (!halfSecond || (eyeWidth % 8 == 0 && eyeHeight % 2 == 0 && !mono));
    }

    uint32_t SecondWidth() const { return halfSecond ? eyeWidth / 2 : eyeWidth; }
    uint32_t SecondHeight() const { return halfSecond ? eyeHeight / 2 : eyeHeight; }
    // Size of the camera frame in pixels (YUY2, two bytes each).
    uint32_t PackedWidth() const
    {
        return mono ? eyeWidth : halfSecond ? kHalfHeaderPixels + eyeWidth + SecondWidth() / 2 : eyeWidth * 2;
    }
    uint32_t PackedHeight() const { return halfSecond ? eyeHeight + kHalfExtraLines : eyeHeight; }
};

struct OutputFormat {
    uint32_t width = 1920;
    uint32_t height = 1080;
    PixelFormat format = PixelFormat::NV12;
    float cropX = 0, cropY = 0, cropW = 1920, cropH = 1080;  // eye-space region scaled to the output
};

struct Rectification {
    float dy = 0;        // vertical offset of the second sensor, eye pixels
    float rotation = 0;  // degrees
    float quality = 0;   // mean best census cost of the last calibration (lower is better), 0 = unknown
};

struct FrameStats {
    float gpuMs = 0;
    float focusDisparity = 0;
    float gain = 1;
    float noise = 0;  // typical 3x3-mean luma change of a still scene between frames (denoise on)
};

class Gpu;

class StereoPipeline {
public:
    StereoPipeline();
    ~StereoPipeline();
    StereoPipeline(const StereoPipeline&) = delete;
    StereoPipeline& operator=(const StereoPipeline&) = delete;

    // Creates the GPU device (Gpu::Initialize) and all GPU resources.
    HRESULT Initialize(const StereoFormat& stereo, const OutputFormat& output);
    bool IsInitialized() const { return m_impl != nullptr; }
    std::string GpuName() const;
    const StereoFormat& Stereo() const { return m_stereo; }
    const OutputFormat& Output() const { return m_output; }

    // yuy2: packed frame of Stereo().PackedWidth() x PackedHeight() pixels. Output: NV12 writes the Y plane to dst and the
    // interleaved UV plane to dstUV; YUY2 writes packed rows to dst (dstUV unused).
    // Pipelined: the output is the previous input frame (one frame of latency). Returns S_FALSE
    // and writes nothing for the first frame after Initialize/Reset. With settings.depthPlane and a
    // stereo input, depth gets the delivered frame's depth plane.
    HRESULT Process(const uint8_t* yuy2, uint32_t yuy2Pitch, const EffectSettings& settings, uint8_t* dst,
        uint8_t* dstUV, uint32_t dstPitch, FrameStats* stats = nullptr, DepthPlane* depth = nullptr);

    // Searches the vertical offset / roll that best aligns the two sensors on this frame.
    HRESULT Calibrate(const uint8_t* yuy2, uint32_t yuy2Pitch, Rectification* result);
    void SetRectification(const Rectification& r);
    Rectification GetRectification() const { return m_rect; }

    // Forget temporal state (call after a stream restart).
    void Reset();

private:
    struct Impl;
    HRESULT CreateResources();
    void Upload(const uint8_t* yuy2, uint32_t pitch);
    void UpdateConstants(const EffectSettings* s, uint32_t pathDir);
    void RunDepth(const EffectSettings& s);
    float ScoreAlignment(float dy, float rotationDeg);
    void UpdateFocus(const uint32_t* histogram, const EffectSettings& s);
    void UpdateGain(const uint32_t* histogram, const EffectSettings& s);
    void UpdateNoise(const uint32_t* histogram);

    std::mutex m_lock;
    StereoFormat m_stereo;
    OutputFormat m_output;
    uint32_t m_workW = 640, m_workH = 360;
    Rectification m_rect;
    float m_focus = -1;
    static constexpr uint32_t kFocusSwitchFrames = 20;  // ~1/3 s at 60 fps
    int m_focusPeak = -1;                               // histogram peak (bin) autofocus follows
    int m_focusCandidate = -1;                          // peak waiting to take the focus over
    uint32_t m_focusCandidateFrames = 0;
    float m_gain = 1;
    float m_noise = -1;  // < 0: not measured yet
    bool m_haveHistory = false;
    bool m_haveClean = false;  // a denoised previous frame to blend with
    uint32_t m_frame = 0;
    uint32_t m_lastDepthFrame = 0xFFFFFFF0;
    uint32_t m_lastDenoiseFrame = 0xFFFFFFF0;

    std::unique_ptr<Gpu> m_gpu;
    Impl* m_impl = nullptr;
};

}  // namespace ps5cam
