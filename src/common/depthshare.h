#pragma once
// Shared memory between whatever computes the camera's depth (the device MFT or the virtual camera,
// both inside Frame Server) and the "PS5 Camera Depth" virtual camera that shows it. The writer
// fills one of two plane buffers and then advances a frame counter; a reader copies the newest
// plane and checks that the writer has not come round to it meanwhile. The reader also stamps a
// heartbeat, by which the writer knows that somebody watches and depth is worth computing.
#include <windows.h>

#include <cstdint>
#include <vector>

namespace ps5cam {

class DepthShare {
public:
    static constexpr uint32_t kMaxWidth = 1920, kMaxHeight = 1080;
    static constexpr ULONGLONG kReaderTimeoutMs = 2000;

    DepthShare() = default;
    ~DepthShare();
    DepthShare(const DepthShare&) = delete;
    DepthShare& operator=(const DepthShare&) = delete;

    bool Open();  // creates the mapping or opens the existing one; false if neither works

    // Writer: whether a reader stamped its heartbeat lately; publishing a plane.
    bool ReaderActive();
    void Publish(const uint8_t* plane, uint32_t width, uint32_t height, uint32_t pitch, LONGLONG time);

    // Reader: the heartbeat; the newest plane if it is newer than *frame (frame updated), its size
    // and time stamp. False when there is nothing newer.
    void Touch();
    bool Read(std::vector<uint8_t>& plane, uint32_t* width, uint32_t* height, uint64_t* frame,
        ULONGLONG* publishedTick);

private:
    struct Header;
    Header* m_header = nullptr;
    uint8_t* m_planes = nullptr;
    HANDLE m_mapping = nullptr;
};

}  // namespace ps5cam
