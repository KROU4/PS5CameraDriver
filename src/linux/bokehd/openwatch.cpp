#include "openwatch.h"

#include <sys/inotify.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace ps5cam {

int CountOpens(int count, uint32_t mask)
{
    if (mask & IN_Q_OVERFLOW) return std::max(count, 1);
    if (mask & IN_OPEN) ++count;
    if (mask & (IN_CLOSE_WRITE | IN_CLOSE_NOWRITE)) count = std::max(count - 1, 0);
    return count;
}

bool OpenWatch::Start(const std::string& path, std::string& error)
{
    Stop();
    m_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (m_fd < 0) {
        error = std::string("inotify_init1: ") + strerror(errno);
        return false;
    }
    if (inotify_add_watch(m_fd, path.c_str(), IN_OPEN | IN_CLOSE_WRITE | IN_CLOSE_NOWRITE) < 0) {
        error = "inotify_add_watch " + path + ": " + strerror(errno);
        Stop();
        return false;
    }
    m_count = 0;
    return true;
}

void OpenWatch::Stop()
{
    if (m_fd >= 0) close(m_fd);
    m_fd = -1;
    m_count = 0;
}

int OpenWatch::Read()
{
    // Room for many events (the node is watched without a name, so each is just the header).
    alignas(inotify_event) char buf[4096];
    for (;;) {
        const ssize_t n = read(m_fd, buf, sizeof(buf));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;  // EAGAIN: all taken
        for (ssize_t at = 0; at + ssize_t(sizeof(inotify_event)) <= n;) {
            inotify_event event;
            memcpy(&event, buf + at, sizeof(event));
            m_count = CountOpens(m_count, event.mask);
            at += ssize_t(sizeof(inotify_event) + event.len);
        }
    }
    return m_count;
}

}  // namespace ps5cam
