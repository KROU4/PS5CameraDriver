#pragma once
// The service: the camera's e9 frames through the GPU pipeline into the "PS5 Camera" loopback
// device, while a program watches it (or all the time with a v4l2loopback that does not say).
#include <string>

namespace ps5cam {

// Runs until SIGTERM or SIGINT (returns 0) or a fatal error (returns 1).
int RunDaemon(const std::string& configPath);

}  // namespace ps5cam
