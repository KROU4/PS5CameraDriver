#pragma once
// What a virtual camera stream gets its frames from: the camera with the effect (CaptureEngine) or
// the depth the effect publishes (DepthEngine). Frames are finished NV12/YUY2 samples of the
// requested type, handed to the sink on the engine's own thread.
#include <windows.h>
#include <mfidl.h>
#include <wrl/client.h>

#include <cstdint>
#include <functional>

#include "pipeline.h"

namespace ps5cam {

struct OutputRequest {
    uint32_t width = 1920;
    uint32_t height = 1080;
    uint32_t fps = 30;
    PixelFormat format = PixelFormat::NV12;
};

class FrameEngine {
public:
    using FrameSink = std::function<void(IMFSample*)>;
    virtual ~FrameEngine() = default;

    virtual HRESULT Start(const OutputRequest& req, FrameSink sink) = 0;
    virtual void Stop() = 0;

    // For forwarding UVC controls (brightness etc.) to the physical camera; null without one.
    virtual Microsoft::WRL::ComPtr<IUnknown> PhysicalSource() { return nullptr; }
    // An app set the anti-flicker itself (see flicker.h).
    virtual void HoldPowerLine() {}
};

}  // namespace ps5cam
