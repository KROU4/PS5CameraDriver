#pragma once
// User-facing settings and runtime status, persisted under HKLM\SOFTWARE\PS5Camera so that the
// tray app (user session) and the media source (Frame Server, LOCAL SERVICE) see the same values.
#include <windows.h>

#include <cstdint>
#include <string>

namespace ps5cam {

struct Settings {
    uint32_t mode = 0;        // ViewMode
    uint32_t blur = 60;       // 0..100
    bool autoFocus = true;
    uint32_t focus = 50;      // manual focus 0 (far) .. 100 (near)
    bool prefer60 = true;     // list 60 fps media types first
    bool fullHdOnly = true;   // offer apps 1920x1080 at 60 fps only (else also 1280x720 and 30 fps)
    uint32_t highlights = 150;  // bokeh highlight gain x100
    uint32_t temporal = 40;   // temporal weight x100
    bool autoBrightness = true;
    uint32_t maxGain = 60;    // x10
};

struct Status {
    bool streaming = false;
    uint32_t fpsX100 = 0;
    uint32_t gpuUs = 0;
    int32_t focusX100 = 0;
    std::wstring format;      // e.g. "1920x1080 NV12 @30"
    std::wstring error;
};

struct StoredCalibration {
    bool valid = false;
    float dy = 0;
    float rotation = 0;
};

Settings LoadSettings();
bool SaveSettings(const Settings& s);
void SaveStatus(const Status& s);
Status LoadStatus();
// sensorKey distinguishes sensor modes, e.g. L"1080" or L"800".
StoredCalibration LoadCalibration(const wchar_t* sensorKey);
void SaveCalibration(const wchar_t* sensorKey, const StoredCalibration& c);
// A counter the tray bumps to ask the source for a fresh calibration; Handled is the last value served.
uint32_t LoadCalibrationRequest();
void BumpCalibrationRequest();
uint32_t LoadCalibrationHandled();
void SaveCalibrationHandled(uint32_t value);
// Raw frame recording for tuning (ps5cam-ctl record N, administrators only): the source takes the
// request, clears it and writes the next N camera frames to %ProgramData%\PS5Camera\record-<key>.raw
// (key of the sensor mode: 1080h, 1080m, ...), readable by administrators only. RequestRecording
// needs admin rights (it secures the Debug key).
uint32_t TakeRecordRequest();
bool RequestRecording(uint32_t frames);

}  // namespace ps5cam
