#pragma once
// When to try aligning the two sensors (StereoPipeline::Calibrate) during a stream: a few frames
// after it starts, and after a failure (a dark or featureless scene) again after 1, 2, 4, 8 and then
// every 16 s. Shared by the device MFT, the virtual camera and the Linux daemon, which store the
// result each in their own place.
#include <algorithm>
#include <cstdint>

namespace ps5cam {

class CalibrationSchedule {
public:
    // At stream start: needed when the sensor mode has no stored alignment or one was requested.
    void Start(bool needed, uint32_t frame)
    {
        m_pending = needed;
        m_failures = 0;
        m_next = frame + 10;
    }
    // A request during the stream (tray, ps5cam-ctl): try again soon.
    void Request(uint32_t frame)
    {
        if (!m_pending) Start(true, frame);
    }
    bool Pending() const { return m_pending; }
    bool Due(uint32_t frame) const { return m_pending && frame >= m_next; }
    void Succeeded()
    {
        m_pending = false;
        m_failures = 0;
    }
    void Failed(uint32_t frame, uint32_t fps)
    {
        m_failures = std::min<uint32_t>(m_failures + 1, 5);
        m_next = frame + (30u << (m_failures - 1)) * std::max<uint32_t>(fps / 30, 1);
    }

private:
    bool m_pending = false;
    uint32_t m_failures = 0;
    uint32_t m_next = 10;
};

}  // namespace ps5cam
