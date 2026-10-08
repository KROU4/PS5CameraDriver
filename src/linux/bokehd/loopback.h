#pragma once
// The v4l2loopback node that programs open as "PS5 Camera": we are its writer and hand it YUYV
// 1920x1080 frames with write(). v4l2loopback 0.13.0 and newer also tell the writer whether a
// program streams from the device (the client-usage event, see loopback.cpp).
#include <cstdint>
#include <string>
#include <vector>

namespace ps5cam {

class Loopback {
public:
    static constexpr uint32_t kWidth = 1920;
    static constexpr uint32_t kHeight = 1080;
    static constexpr uint32_t kPitch = kWidth * 2;
    static constexpr uint32_t kFrameBytes = kPitch * kHeight;

    Loopback();
    ~Loopback() { Close(); }
    Loopback(const Loopback&) = delete;
    Loopback& operator=(const Loopback&) = delete;

    // Opens the node as its writer, sets the format and frame rate, writes a black frame (with
    // exclusive_caps=1 programs see a camera only once the writer has delivered a frame) and
    // subscribes to the client-usage event.
    bool Open(const std::string& path, uint32_t fps, std::string& error);
    void Close();
    bool IsOpen() const { return m_fd >= 0; }
    int Fd() const { return m_fd; }
    const std::string& Path() const { return m_path; }

    // Whether the module reports its readers; without that we cannot know when a program watches.
    bool UsageEvents() const { return m_usageEvents; }
    // Reads the queued events: the reader count of the newest client-usage event, or -1 if none came.
    int TakeUsage();

    // The frame rate readers are told.
    bool SetFps(uint32_t fps, std::string& error);

    // One frame of kFrameBytes; false when the device is no longer usable (error says why).
    bool Write(const uint8_t* frame, std::string& error);
    bool WriteBlack(std::string& error) { return Write(m_black.data(), error); }

private:
    int m_fd = -1;
    std::string m_path;
    bool m_usageEvents = false;
    bool m_shortWriteLogged = false;
    std::vector<uint8_t> m_black;
};

}  // namespace ps5cam
