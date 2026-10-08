#pragma once
// Streaming capture of the camera's e9 frame (YUYV 2448x1088) through mmap'ed driver buffers.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ps5cam {

class Capture {
public:
    Capture() = default;
    ~Capture() { Close(); }
    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;

    // Opens the node, sets the format and frame rate and starts streaming; error says why not.
    bool Open(const std::string& path, uint32_t fps, std::string& error);
    // Stops streaming and releases the device.
    void Close();
    bool IsOpen() const { return m_fd >= 0; }
    int Fd() const { return m_fd; }
    const std::string& Path() const { return m_path; }
    uint32_t Pitch() const { return m_pitch; }
    double Fps() const { return m_fps; }

    enum class Result { Frame, None, Skipped, Lost };
    struct Drops {
        uint32_t late = 0;        // complete frames given back unprocessed because a newer one came
        uint32_t incomplete = 0;  // frames cut short or flagged as damaged by the driver
    };
    // Takes every filled buffer without waiting and keeps the newest complete frame, so that a
    // GPU slower than the camera drops frames instead of falling behind. Frame: *data holds it until
    // Requeue. None: no frame yet. Skipped: only incomplete frames came. Lost: the camera is gone or
    // broken (error says how); Close and look for it again.
    Result Dequeue(const uint8_t** data, Drops& drops, std::string& error);
    // Gives the buffer of the last Frame back to the driver.
    bool Requeue(std::string& error);

    // The UVC power line frequency control (V4L2_CID_POWER_LINE_FREQUENCY: 0 off, 1 50 Hz, 2 60 Hz).
    bool SetPowerLine(int value, std::string& error);
    bool GetPowerLine(int& value);

private:
    struct Buffer {
        void* data = nullptr;
        size_t length = 0;
    };
    bool Start(uint32_t fps, std::string& error);
    bool Queue(uint32_t index, std::string& error);
    Result DequeueOne(uint32_t* index, std::string& error);

    int m_fd = -1;
    std::string m_path;
    std::vector<Buffer> m_buffers;
    uint32_t m_pitch = 0;
    double m_fps = 0;
    int m_dequeued = -1;
    bool m_streaming = false;
};

}  // namespace ps5cam
