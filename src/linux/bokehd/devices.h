#pragma once
// The camera and the loopback device among the V4L2 nodes, found through sysfs.
#include <linux/videodev2.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ps5cam {

// The firmware's e9 frame: a 48-pixel header on every line, the main sensor, then the second sensor
// at half size; 1088 lines (StereoFormat with halfSecond).
constexpr uint32_t kCameraWidth = 2448;
constexpr uint32_t kCameraHeight = 1088;
// The PS5 camera once its firmware is running.
constexpr const char* kCameraVendor = "05a9";
constexpr const char* kCameraProduct = "058c";
// v4l2loopback's card_label for our output and the node number (installer/linux/install.sh).
constexpr const char* kLoopbackLabel = "PS5 Camera";
constexpr const char* kLoopbackPreferred = "/dev/video42";
// VIDIOC_QUERYCAP's driver of a v4l2loopback device.
constexpr const char* kLoopbackDriver = "v4l2 loopback";

// ioctl, again when a signal interrupted it.
int Ioctl(int fd, unsigned long request, void* arg);
// VIDIOC_QUERYCAP's driver name.
std::string DriverOf(const v4l2_capability& cap);

struct VideoNode {
    std::string path;     // /dev/videoN
    std::string name;     // the driver's name for it
    std::string problem;  // why it cannot be used; empty when it can
};

// Video capture nodes of the camera (USB 05a9:058c); usable ones offer YUYV 2448x1088.
std::vector<VideoNode> FindCameraNodes();
// The first usable camera node, or empty.
std::string FindCamera();

// Nodes named "PS5 Camera", /dev/video42 first. Any device may call itself that, so the output
// is the first of them whose driver is v4l2loopback (Loopback::Open checks).
std::vector<std::string> LoopbackCandidates();
// The same with their driver checked, for --list.
std::vector<VideoNode> FindLoopbackNodes();

}  // namespace ps5cam
