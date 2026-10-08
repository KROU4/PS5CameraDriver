#include "devices.h"

#include <dirent.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace ps5cam {

namespace {

constexpr const char* kSysClass = "/sys/class/video4linux";

std::string ReadAttribute(const std::string& path)
{
    std::ifstream in(path);
    std::string line;
    if (!in || !std::getline(in, line)) return {};
    while (!line.empty() && (line.back() == '\n' || line.back() == ' ')) line.pop_back();
    return line;
}

// "video0", "video1", ... in numeric order, so that the first camera node is the capture one.
std::vector<std::string> VideoNodeNames()
{
    std::vector<std::string> names;
    DIR* dir = opendir(kSysClass);
    if (!dir) return names;
    while (const dirent* e = readdir(dir))
        if (strncmp(e->d_name, "video", 5) == 0) names.emplace_back(e->d_name);
    closedir(dir);
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
        return atoi(a.c_str() + 5) < atoi(b.c_str() + 5);
    });
    return names;
}

bool OffersCameraFrame(int fd)
{
    // Bounded loops: a broken driver must not keep us enumerating forever.
    for (uint32_t i = 0; i < 64; ++i) {
        v4l2_fmtdesc format = {};
        format.index = i;
        format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (Ioctl(fd, VIDIOC_ENUM_FMT, &format) != 0) return false;
        if (format.pixelformat != V4L2_PIX_FMT_YUYV) continue;
        for (uint32_t j = 0; j < 256; ++j) {
            v4l2_frmsizeenum size = {};
            size.index = j;
            size.pixel_format = V4L2_PIX_FMT_YUYV;
            if (Ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &size) != 0) return false;
            if (size.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
                if (size.discrete.width == kCameraWidth && size.discrete.height == kCameraHeight) return true;
            } else {  // one stepwise or continuous range
                const v4l2_frmsize_stepwise& s = size.stepwise;
                return kCameraWidth >= s.min_width && kCameraWidth <= s.max_width && kCameraHeight >= s.min_height &&
                       kCameraHeight <= s.max_height;
            }
        }
        return false;
    }
    return false;
}

// False for nodes that are not video capture (uvcvideo's metadata node); problem is empty for a
// node we can stream from.
bool ProbeCameraNode(const std::string& path, std::string& problem)
{
    const int fd = open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        problem = std::string("cannot open: ") + strerror(errno);
        return true;
    }
    v4l2_capability cap = {};
    bool capture = false;
    if (Ioctl(fd, VIDIOC_QUERYCAP, &cap) != 0) {
        problem = std::string("VIDIOC_QUERYCAP: ") + strerror(errno);
        capture = true;
    } else {
        const uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
        capture = (caps & V4L2_CAP_VIDEO_CAPTURE) != 0;
        if (capture && !(caps & V4L2_CAP_STREAMING)) problem = "no streaming I/O";
        else if (capture && !OffersCameraFrame(fd))
            problem = "no YUYV 2448x1088 format (is the PS5 camera firmware with the stereo frame loaded?)";
    }
    close(fd);
    return capture;
}

}  // namespace

std::string DriverOf(const v4l2_capability& cap)
{
    const auto* driver = reinterpret_cast<const char*>(cap.driver);
    return std::string(driver, strnlen(driver, sizeof(cap.driver)));
}

int Ioctl(int fd, unsigned long request, void* arg)
{
    int r;
    do {
        r = ioctl(fd, request, arg);
    } while (r < 0 && errno == EINTR);
    return r;
}

std::vector<VideoNode> FindCameraNodes()
{
    std::vector<VideoNode> nodes;
    for (const std::string& n : VideoNodeNames()) {
        // device is the USB interface; its parent is the USB device with the ids.
        const std::string sys = std::string(kSysClass) + "/" + n;
        if (ReadAttribute(sys + "/device/../idVendor") != kCameraVendor ||
            ReadAttribute(sys + "/device/../idProduct") != kCameraProduct)
            continue;
        VideoNode node{"/dev/" + n, ReadAttribute(sys + "/name"), {}};
        if (ProbeCameraNode(node.path, node.problem)) nodes.push_back(node);
    }
    return nodes;
}

std::string FindCamera()
{
    for (const VideoNode& n : FindCameraNodes())
        if (n.problem.empty()) return n.path;
    return {};
}

std::vector<std::string> LoopbackCandidates()
{
    std::vector<std::string> paths;
    for (const std::string& n : VideoNodeNames())
        if (ReadAttribute(std::string(kSysClass) + "/" + n + "/name") == kLoopbackLabel) paths.push_back("/dev/" + n);
    std::stable_partition(paths.begin(), paths.end(), [](const std::string& p) { return p == kLoopbackPreferred; });
    return paths;
}

std::vector<VideoNode> FindLoopbackNodes()
{
    std::vector<VideoNode> nodes;
    for (const std::string& path : LoopbackCandidates()) {
        VideoNode node{path, kLoopbackLabel, {}};
        const int fd = open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        v4l2_capability cap = {};
        if (fd < 0) {
            node.problem = std::string("cannot open: ") + strerror(errno);
        } else {
            if (Ioctl(fd, VIDIOC_QUERYCAP, &cap) != 0)
                node.problem = std::string("VIDIOC_QUERYCAP: ") + strerror(errno);
            else if (const std::string driver = DriverOf(cap); driver != kLoopbackDriver)
                node.problem = "driver \"" + driver + "\" is not v4l2loopback";
            close(fd);
        }
        nodes.push_back(node);
    }
    return nodes;
}

}  // namespace ps5cam
