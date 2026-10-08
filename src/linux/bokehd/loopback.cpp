#include "loopback.h"

#include <fcntl.h>
#include <linux/videodev2.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "devices.h"
#include "log.h"

namespace ps5cam {

namespace {

// v4l2loopback's private event, defined in v4l2loopback.c and in no installed header. It comes on
// subscription (with V4L2_EVENT_SUB_FL_SEND_INITIAL) and whenever a reader starts or stops
// streaming. Added in commit ceed405 (August 2020), first released in 0.13.0 (March 2024); 0.12.x
// (Debian 12, Ubuntu 24.04) rejects the subscription. count: 0.13.x the readers streaming (at most
// one may), 0.14.0 and newer 1 while the reader owning the capture queue streams, else 0.
constexpr uint32_t kEventClientUsage = V4L2_EVENT_PRIVATE_START + 0x08E00000 + 1;
struct ClientUsage {
    uint32_t count;
};

std::string Errno(const std::string& what)
{
    return what + ": " + strerror(errno);
}

}  // namespace

Loopback::Loopback() : m_black(kFrameBytes)
{
    // Studio-range black: Y 16, U and V 128.
    for (size_t i = 0; i < m_black.size(); i += 2) {
        m_black[i] = 16;
        m_black[i + 1] = 128;
    }
}

bool Loopback::Open(const std::string& path, uint32_t fps, std::string& error)
{
    Close();
    m_fd = open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (m_fd < 0) {
        error = Errno("open " + path);
        return false;
    }
    m_path = path;
    v4l2_capability cap = {};
    if (Ioctl(m_fd, VIDIOC_QUERYCAP, &cap) != 0) {
        error = Errno("VIDIOC_QUERYCAP");
        Close();
        return false;
    }
    // Any device may name itself "PS5 Camera": only v4l2loopback's is ours to write to.
    if (const std::string driver = DriverOf(cap); driver != kLoopbackDriver) {
        error = "driver \"" + driver + "\" is not v4l2loopback";
        Close();
        return false;
    }
    const uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
    if (!(caps & V4L2_CAP_VIDEO_OUTPUT)) {
        // With exclusive_caps=1 a device that already has a writer shows only its capture side.
        error = "not a video output device (does another program write to it?)";
        Close();
        return false;
    }

    v4l2_format format = {};
    format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    v4l2_pix_format& pix = format.fmt.pix;
    pix.width = kWidth;
    pix.height = kHeight;
    pix.pixelformat = V4L2_PIX_FMT_YUYV;
    pix.field = V4L2_FIELD_NONE;
    pix.bytesperline = kPitch;
    pix.sizeimage = kFrameBytes;
    // What the pipeline writes: BT.601 studio range.
    pix.colorspace = V4L2_COLORSPACE_SMPTE170M;
    pix.ycbcr_enc = V4L2_YCBCR_ENC_601;
    pix.quantization = V4L2_QUANTIZATION_LIM_RANGE;
    if (Ioctl(m_fd, VIDIOC_S_FMT, &format) != 0) {
        error = Errno("VIDIOC_S_FMT");
        Close();
        return false;
    }
    if (pix.width != kWidth || pix.height != kHeight || pix.pixelformat != V4L2_PIX_FMT_YUYV ||
        (pix.bytesperline && pix.bytesperline != kPitch)) {
        error = "the device took " + std::to_string(pix.width) + "x" + std::to_string(pix.height) +
                " instead of YUYV 1920x1080";
        Close();
        return false;
    }
    std::string fpsError;
    if (!SetFps(fps, fpsError)) Log("%s: %s (programs may be told another frame rate)", path.c_str(), fpsError.c_str());
    if (!WriteBlack(error)) {
        Close();
        return false;
    }

    v4l2_event_subscription sub = {};
    sub.type = kEventClientUsage;
    sub.flags = V4L2_EVENT_SUB_FL_SEND_INITIAL;
    m_usageEvents = Ioctl(m_fd, VIDIOC_SUBSCRIBE_EVENT, &sub) == 0;
    return true;
}

void Loopback::Close()
{
    if (m_fd < 0) return;
    close(m_fd);
    m_fd = -1;
    m_usageEvents = false;
}

int Loopback::TakeUsage()
{
    int count = -1;
    // Bounded: the queue holds one usage event (newer ones replace it), plus whatever else came.
    for (int i = 0; i < 64; ++i) {
        v4l2_event event = {};
        if (Ioctl(m_fd, VIDIOC_DQEVENT, &event) != 0) break;  // ENOENT: none left
        if (event.type != kEventClientUsage) continue;
        ClientUsage usage;
        memcpy(&usage, event.u.data, sizeof(usage));
        count = static_cast<int>(usage.count);
    }
    return count;
}

bool Loopback::SetFps(uint32_t fps, std::string& error)
{
    v4l2_streamparm parm = {};
    parm.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    parm.parm.output.timeperframe = {1, fps};
    if (Ioctl(m_fd, VIDIOC_S_PARM, &parm) == 0) return true;
    error = Errno("VIDIOC_S_PARM");
    return false;
}

bool Loopback::Write(const uint8_t* frame, std::string& error)
{
    for (;;) {
        const ssize_t n = write(m_fd, frame, kFrameBytes);
        if (n == ssize_t(kFrameBytes)) return true;
        if (n >= 0) {
            // The module's buffers are smaller than the format we set: readers get cut frames.
            if (!m_shortWriteLogged)
                Log("%s: wrote %zd of %u bytes per frame", m_path.c_str(), n, kFrameBytes);
            m_shortWriteLogged = true;
            return true;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN) return true;  // dropped; the next frame comes soon
        error = Errno("write " + m_path);
        return false;
    }
}

}  // namespace ps5cam
