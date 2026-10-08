#pragma once
// User-facing settings and runtime status, persisted under HKLM\SOFTWARE\PS5Camera so that the
// tray app (user session) and the media source (Frame Server, LOCAL SERVICE) see the same values.
#include <windows.h>

#include <cstdint>
#include <string>

#include "../core/picture.h"

namespace ps5cam {

// The picture settings shared with Linux (mode, blur, focus, ... anti-flicker) and Windows' own.
struct Settings : PictureSettings {
    bool prefer60 = true;     // list 60 fps media types first
    bool fullHdOnly = true;   // offer apps 1920x1080 at 60 fps only (else also 1280x720 and 30 fps)
    uint32_t blurStyle = 0;   // kBlurPortrait / kBlurStandard: what Windows' "Background effects" chose
    uint32_t mainsHz = 0;     // 50 / 60 by the signed-in user's region (tray, installer); 0 unknown
    bool depthCamera = false; // the "PS5 Camera Depth" virtual camera (the service registers it)
    uint32_t depthView = 0;   // what it shows: 0 depth (near = bright), 1 subject matte
};

// Mains frequency (50 or 60) where the account running this lives, by the country Windows is set
// to. In Frame Server (LOCAL SERVICE) that is not the user's: there Settings::mainsHz counts.
uint32_t RegionMainsHz();
inline bool Mains60(const Settings& s) { return (s.mainsHz ? s.mainsHz : RegionMainsHz()) == 60; }

// Windows' camera effects: "Portrait blur" is our bokeh as it is, "Standard blur" the strongest.
constexpr uint32_t kBlurPortrait = 0;
constexpr uint32_t kBlurStandard = 1;

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
// One value (its registry name, e.g. L"Mode"), for writers that change a single thing while the
// tray may be saving others.
bool WriteSetting(const wchar_t* name, uint32_t value);
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
