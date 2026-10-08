#pragma once
// Frames of the "PS5 Camera Depth" camera: at the requested rate, the newest depth plane the
// device MFT (or the virtual camera) published for the frames "PS5 Camera" delivers to another
// program, as grey NV12/YUY2. Black while nothing is published (the camera itself not in use).
#include <windows.h>

#include <atomic>
#include <thread>
#include <vector>

#include "../common/depthshare.h"
#include "engine.h"

namespace ps5cam {

class DepthEngine : public FrameEngine {
public:
    ~DepthEngine() override;
    HRESULT Start(const OutputRequest& req, FrameSink sink) override;
    void Stop() override;

private:
    void Run();
    Microsoft::WRL::ComPtr<IMFSample> MakeFrame(bool fresh);

    OutputRequest m_req;
    FrameSink m_sink;
    std::thread m_thread;
    HANDLE m_stop = nullptr;
    DepthShare m_share;
    bool m_shareOpen = false;
    std::vector<uint8_t> m_plane;
    uint32_t m_planeW = 0, m_planeH = 0;
    uint64_t m_frame = 0;
};

}  // namespace ps5cam
