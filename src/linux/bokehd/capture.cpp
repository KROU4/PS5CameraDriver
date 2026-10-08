#include "capture.h"

#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "devices.h"

namespace ps5cam {

namespace {

constexpr uint32_t kBufferCount = 4;
constexpr uint32_t kRowBytes = kCameraWidth * 2;  // YUYV

std::string Errno(const std::string& what)
{
    return what + ": " + strerror(errno);
}

}  // namespace

bool Capture::Open(const std::string& path, uint32_t fps, std::string& error)
{
    Close();
    m_fd = open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (m_fd < 0) {
        error = Errno("open " + path);
        return false;
    }
    m_path = path;
    if (!Start(fps, error)) {
        Close();
        return false;
    }
    return true;
}

bool Capture::Start(uint32_t fps, std::string& error)
{
    v4l2_format format = {};
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width = kCameraWidth;
    format.fmt.pix.height = kCameraHeight;
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    format.fmt.pix.field = V4L2_FIELD_NONE;
    if (Ioctl(m_fd, VIDIOC_S_FMT, &format) != 0) {
        // EBUSY: another program streams from the camera.
        error = Errno("VIDIOC_S_FMT");
        return false;
    }
    // The driver answers with the nearest format it has instead of failing.
    if (format.fmt.pix.width != kCameraWidth || format.fmt.pix.height != kCameraHeight ||
        format.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV) {
        error = "the camera has no YUYV 2448x1088 format (it offered " + std::to_string(format.fmt.pix.width) + "x" +
                std::to_string(format.fmt.pix.height) + ")";
        return false;
    }
    m_pitch = format.fmt.pix.bytesperline ? format.fmt.pix.bytesperline : kRowBytes;
    if (m_pitch < kRowBytes) {
        error = "the driver reports " + std::to_string(m_pitch) + " bytes per line";
        return false;
    }

    v4l2_streamparm parm = {};
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe = {1, fps};
    if (Ioctl(m_fd, VIDIOC_S_PARM, &parm) != 0) {
        error = Errno("VIDIOC_S_PARM");
        return false;
    }
    const v4l2_fract& tpf = parm.parm.capture.timeperframe;
    m_fps = tpf.numerator ? double(tpf.denominator) / tpf.numerator : fps;

    v4l2_requestbuffers request = {};
    request.count = kBufferCount;
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    request.memory = V4L2_MEMORY_MMAP;
    if (Ioctl(m_fd, VIDIOC_REQBUFS, &request) != 0) {
        error = Errno("VIDIOC_REQBUFS");
        return false;
    }
    if (request.count < 2) {
        error = "the driver gave " + std::to_string(request.count) + " buffer(s)";
        return false;
    }
    const size_t frameBytes = size_t(m_pitch) * (kCameraHeight - 1) + kRowBytes;
    for (uint32_t i = 0; i < request.count; ++i) {
        v4l2_buffer buf = {};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (Ioctl(m_fd, VIDIOC_QUERYBUF, &buf) != 0) {
            error = Errno("VIDIOC_QUERYBUF");
            return false;
        }
        if (buf.length < frameBytes) {
            error = "a driver buffer of " + std::to_string(buf.length) + " bytes cannot hold a frame";
            return false;
        }
        void* data = mmap(nullptr, buf.length, PROT_READ, MAP_SHARED, m_fd, buf.m.offset);
        if (data == MAP_FAILED) {
            error = Errno("mmap");
            return false;
        }
        m_buffers.push_back({data, buf.length});
    }
    for (uint32_t i = 0; i < m_buffers.size(); ++i)
        if (!Queue(i, error)) return false;
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (Ioctl(m_fd, VIDIOC_STREAMON, &type) != 0) {
        error = Errno("VIDIOC_STREAMON");
        return false;
    }
    m_streaming = true;
    return true;
}

void Capture::Close()
{
    if (m_fd < 0) return;
    if (m_streaming) {
        // Fails on an unplugged camera, which is stopped anyway; close below releases it either way.
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        Ioctl(m_fd, VIDIOC_STREAMOFF, &type);
    }
    for (const Buffer& b : m_buffers) munmap(b.data, b.length);
    m_buffers.clear();
    close(m_fd);
    m_fd = -1;
    m_streaming = false;
    m_dequeued = -1;
    m_pitch = 0;
    m_fps = 0;
}

bool Capture::Queue(uint32_t index, std::string& error)
{
    v4l2_buffer buf = {};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = index;
    if (Ioctl(m_fd, VIDIOC_QBUF, &buf) == 0) return true;
    error = Errno("VIDIOC_QBUF");
    return false;
}

Capture::Result Capture::DequeueOne(uint32_t* index, std::string& error)
{
    v4l2_buffer buf = {};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    if (Ioctl(m_fd, VIDIOC_DQBUF, &buf) != 0) {
        if (errno == EAGAIN) return Result::None;
        // ENODEV, or EIO from a queue the driver cancelled on disconnect.
        error = Errno("VIDIOC_DQBUF");
        return Result::Lost;
    }
    if (buf.index >= m_buffers.size()) {
        error = "VIDIOC_DQBUF returned buffer " + std::to_string(buf.index);
        return Result::Lost;
    }
    // A frame cut short by a USB error would show the previous frame's bottom (or garbage).
    const size_t frameBytes = size_t(m_pitch) * (kCameraHeight - 1) + kRowBytes;
    if ((buf.flags & V4L2_BUF_FLAG_ERROR) || buf.bytesused < frameBytes)
        return Queue(buf.index, error) ? Result::Skipped : Result::Lost;
    *index = buf.index;
    return Result::Frame;
}

Capture::Result Capture::Dequeue(const uint8_t** data, Drops& drops, std::string& error)
{
    Result result = Result::None;
    // Bounded: the driver cannot have more filled buffers than it has, even refilling one meanwhile.
    for (size_t i = 0; i < m_buffers.size() * 2; ++i) {
        uint32_t index = 0;
        const Result r = DequeueOne(&index, error);
        if (r == Result::None) break;
        if (r == Result::Lost) return r;
        if (r == Result::Skipped) {
            ++drops.incomplete;
            if (result == Result::None) result = Result::Skipped;
            continue;
        }
        if (m_dequeued >= 0) {
            if (!Queue(static_cast<uint32_t>(m_dequeued), error)) return Result::Lost;
            ++drops.late;
        }
        m_dequeued = static_cast<int>(index);
        result = Result::Frame;
    }
    if (result == Result::Frame) *data = static_cast<const uint8_t*>(m_buffers[size_t(m_dequeued)].data);
    return result;
}

bool Capture::Requeue(std::string& error)
{
    if (m_dequeued < 0) return true;
    const auto index = static_cast<uint32_t>(m_dequeued);
    m_dequeued = -1;
    return Queue(index, error);
}

bool Capture::SetPowerLine(int value, std::string& error)
{
    v4l2_control control = {};
    control.id = V4L2_CID_POWER_LINE_FREQUENCY;
    control.value = value;
    if (Ioctl(m_fd, VIDIOC_S_CTRL, &control) == 0) return true;
    error = Errno("VIDIOC_S_CTRL power_line_frequency");
    return false;
}

}  // namespace ps5cam
