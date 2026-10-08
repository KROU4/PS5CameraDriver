#pragma once
// The writer side of DepthShare for whoever runs the GPU pipeline on camera frames (the device MFT,
// the virtual camera): while the "PS5 Camera Depth" camera is open somewhere, the pipeline also
// renders the depth plane and it is published.
#include <windows.h>

#include <cstdint>
#include <vector>

#include "depthshare.h"
#include "pipeline.h"

namespace ps5cam {

class DepthPublisher {
public:
    // Somebody watches the depth camera (looked up at most every 250 ms).
    bool Wanted()
    {
        const ULONGLONG now = GetTickCount64();
        if (now - m_checkTick >= 250) {
            m_checkTick = now;
            if (!m_open && now - m_openTick >= 1000) {
                m_openTick = now;
                m_open = m_share.Open();
            }
            m_wanted = m_open && m_share.ReaderActive();
        }
        return m_wanted;
    }

    // The plane to hand to StereoPipeline::Process, or null when nobody watches.
    DepthPlane* Plane(uint32_t width, uint32_t height)
    {
        if (!Wanted() || width > DepthShare::kMaxWidth || height > DepthShare::kMaxHeight) return nullptr;
        m_buffer.resize(size_t(width) * height);
        m_plane.data = m_buffer.data();
        m_plane.pitch = width;
        m_plane.written = false;
        m_width = width;
        m_height = height;
        return &m_plane;
    }

    // After Process: publishes the plane if the delivered frame had one.
    void Publish(LONGLONG time)
    {
        if (m_plane.written) m_share.Publish(m_buffer.data(), m_width, m_height, m_width, time);
        m_plane.written = false;
    }

private:
    DepthShare m_share;
    bool m_open = false, m_wanted = false;
    ULONGLONG m_checkTick = 0, m_openTick = 0;
    std::vector<uint8_t> m_buffer;
    DepthPlane m_plane;
    uint32_t m_width = 0, m_height = 0;
};

}  // namespace ps5cam
