#pragma once
// Test mode without a camera: the pipeline over a recorded clip, with the same output as the
// Windows pipeline gives for it, so that the two GPU backends can be compared byte for byte.
#include <cstdint>
#include <string>

#include "pipeline.h"

namespace ps5cam {

struct TestOptions {
    std::string input;   // packed 2448x1088 YUY2 frames (the firmware e9 frame), back to back
    std::string output;  // the processed frames, back to back
    uint32_t frames = 1000000;
    PixelFormat format = PixelFormat::NV12;
    uint32_t mode = 0;      // ViewMode, the debug views of the disparity included
    uint32_t blur = 60;     // 0..100
    uint32_t denoise = 70;  // 0..100, the pipeline's default
    bool haveDy = false;    // dy instead of calibrating on the first frame
    float dy = 0;
    int depthPlane = -1;    // also write the depth camera's plane: -1 no, 0 disparity, 1 matte
};

// Exit codes: 2 a file cannot be opened, 3 the pipeline does not start, 4 a frame fails, 5 the
// output cannot be written.
int RunTest(const TestOptions& options);

}  // namespace ps5cam
