#pragma once
// Who has a device node open, for v4l2loopback older than 0.13 (no client-usage event): inotify
// reports every open and close of the node, and opens minus closes says whether a program may be
// watching. Best effort: programs that had the node open before the watch started are not counted,
// and the kernel merges identical events that were not read in between (so read promptly).
#include <cstdint>
#include <string>

namespace ps5cam {

// The open count after one inotify event (its mask). A lost event (queue overflow) counts as a
// program watching: better a camera running for nobody than one that stops on a viewer.
int CountOpens(int count, uint32_t mask);

class OpenWatch {
public:
    OpenWatch() = default;
    ~OpenWatch() { Stop(); }
    OpenWatch(const OpenWatch&) = delete;
    OpenWatch& operator=(const OpenWatch&) = delete;

    // Watches opens and closes of path from now on, with the count at 0.
    bool Start(const std::string& path, std::string& error);
    void Stop();
    bool IsActive() const { return m_fd >= 0; }
    int Fd() const { return m_fd; }  // readable when events wait
    // Takes the waiting events; the opens not closed yet.
    int Read();
    int Count() const { return m_count; }

private:
    int m_fd = -1;
    int m_count = 0;
};

}  // namespace ps5cam
