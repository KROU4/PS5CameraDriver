#pragma once
// The sensors' alignment found by StereoPipeline::Calibrate, kept across restarts as the text
// "dy rotation" in the state directory: $STATE_DIRECTORY (systemd's StateDirectory=), else
// /var/lib/ps5cam.
#include <string>

#include "pipeline.h"

namespace ps5cam {

// Calibrate searches dy in -13..13 eye pixels and the roll in -1..1 degrees: a file outside that
// (with some margin) is damaged or from something else.
constexpr float kMaxCalibrationDy = 13.0f;
constexpr float kMaxCalibrationRotation = 1.25f;

std::string StateDirectory();

// False when there is no usable saved calibration (missing, unreadable or out of range; logged).
bool LoadCalibration(const std::string& dir, Rectification& r);
// Replaces the file atomically, so a crash never leaves half of it; failures are logged.
bool SaveCalibration(const std::string& dir, const Rectification& r);

}  // namespace ps5cam
