// ps5cam-bokehd: the PS5 HD Camera's bokeh on Linux. Without arguments it is the service
// (daemon.h); --input runs the pipeline over a recorded clip instead (testmode.h), --list shows
// the devices it would use.
#include <algorithm>
#include <cstdio>
#include <string>
#include <string_view>

#include "config.h"
#include "daemon.h"
#include "devices.h"
#include "testmode.h"
#include "version.h"

using namespace ps5cam;

namespace {

void Usage(FILE* to)
{
    fputs("usage: ps5cam-bokehd [--config FILE]\n"
          "         the service: camera -> GPU -> the v4l2loopback device \"PS5 Camera\"; settings in\n"
          "         /etc/ps5cam/bokeh.conf (or FILE), applied when the file changes and on SIGHUP\n"
          "       ps5cam-bokehd --list | --version\n"
          "         the camera and loopback devices found | the release number\n"
          "       ps5cam-bokehd --input CLIP --output OUT [--frames N] [--format nv12|yuy2] [--mode M]\n"
          "                     [--blur 0..100] [--denoise 0..100] [--dy DY] [--depthplane 0|1]\n"
          "         test without a camera: the pipeline over CLIP (recorded camera frames: packed\n"
          "         2448x1088 YUY2, 5326848 bytes each) into OUT, NV12 by default (the same output on\n"
          "         Windows and Linux, so that the GPU backends can be compared)\n"
          "         --mode: 0 bokeh, 1 main, 2 second, 3 depth, 4 side by side, 5..8 debug views of the\n"
          "         disparity (raw, checked, hole-filled, grey)\n"
          "         --dy: vertical offset of the second sensor instead of calibrating on the first frame\n"
          "         --depthplane: also write the depth camera's plane (0 disparity, 1 matte) to OUT.depth\n"
          "         defaults: mode 0, blur 60, denoise 70\n"
          "environment PS5CAM_GPU=NAME: the GPU whose name contains NAME; CPU Vulkan drivers (lavapipe)\n"
          "         are used only when named, e.g. PS5CAM_GPU=llvmpipe for tests on a machine without a GPU\n",
        to);
}

int List()
{
    bool camera = false;
    const auto cameras = FindCameraNodes();
    if (cameras.empty()) printf("camera: none (USB 05a9:058c: is it plugged in, with the firmware loaded?)\n");
    for (const VideoNode& n : cameras) {
        printf("camera: %s (%s)%s%s\n", n.path.c_str(), n.name.c_str(), n.problem.empty() ? "" : ": ",
            n.problem.c_str());
        camera = camera || n.problem.empty();
    }
    bool output = false;
    const auto outputs = FindLoopbackNodes();
    if (outputs.empty()) printf("output: none (no v4l2loopback device named \"%s\")\n", kLoopbackLabel);
    for (const VideoNode& n : outputs) {
        printf("output: %s (%s)%s%s\n", n.path.c_str(), n.name.c_str(), n.problem.empty() ? "" : ": ",
            n.problem.c_str());
        output = output || n.problem.empty();
    }
    return camera && output ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv)
{
    static constexpr std::string_view kWithValue[] = {"--config", "--input", "--output", "--frames", "--format",
        "--mode", "--blur", "--denoise", "--dy", "--depthplane"};
    std::string config = kDefaultConfigPath;
    bool list = false;
    bool test = false;
    TestOptions options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            Usage(stdout);
            return 0;
        }
        if (arg == "--version") {
            printf("ps5cam-bokehd %s\n", PS5CAM_VERSION);
            return 0;
        }
        if (arg == "--list") {
            list = true;
            continue;
        }
        if (std::find(std::begin(kWithValue), std::end(kWithValue), arg) == std::end(kWithValue)) {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            Usage(stderr);
            return 2;
        }
        if (i + 1 >= argc) {
            fprintf(stderr, "%s needs a value\n", argv[i]);
            return 2;
        }
        const std::string value = argv[++i];
        uint32_t number = 0;
        bool ok = true;
        if (arg == "--config") config = value;
        else if (arg == "--input") options.input = value;
        else if (arg == "--output") options.output = value;
        else if (arg == "--frames") ok = ParseUint(value, options.frames);
        else if (arg == "--format") {
            ok = value == "nv12" || value == "yuy2";
            options.format = value == "yuy2" ? PixelFormat::YUY2 : PixelFormat::NV12;
        } else if (arg == "--mode") ok = ParseUint(value, options.mode) && options.mode <= 8;
        else if (arg == "--blur") ok = ParseUint(value, options.blur) && options.blur <= 100;
        else if (arg == "--denoise") ok = ParseUint(value, options.denoise) && options.denoise <= 100;
        else if (arg == "--dy") ok = options.haveDy = ParseFloat(value, options.dy);
        else if (arg == "--depthplane") {
            ok = ParseUint(value, number) && number <= 1;
            options.depthPlane = static_cast<int>(number);
        }
        if (!ok) {
            fprintf(stderr, "bad value for %s: %s\n", argv[i - 1], value.c_str());
            return 2;
        }
        test = test || arg != "--config";
    }
    if (list) return List();
    if (test) {
        if (options.input.empty() || options.output.empty()) {
            fprintf(stderr, "the test mode needs both --input CLIP and --output OUT\n");
            return 2;
        }
        return RunTest(options);
    }
    return RunDaemon(config);
}
