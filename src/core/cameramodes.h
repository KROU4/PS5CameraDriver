#pragma once
// What the PS5 camera streams for each effect, shared by the virtual camera (src/vcam) and the
// device MFT (src/dmft): sensor modes as UVC formats, how a frame of each is laid out, and the
// output media types offered to apps.
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>

#include <cstdint>
#include <vector>

#include "pipeline.h"

namespace ps5cam {

enum class Layout { Mono, SideBySide, HalfSecond };

struct SensorMode {
    uint32_t packedW, packedH, fps;  // the UVC frame (YUY2)
    Layout layout;
    const wchar_t* key;  // calibration key
};

inline constexpr const wchar_t* kMonoKey = L"1080m";
inline constexpr const wchar_t* kHalfKey = L"1080h";

// Every view but Main (the diagnostic Second and SideBySide included) uses the depth mode, so
// switching between them never reopens the camera; Second shows the second sensor at 960x540 then.
const wchar_t* WantedKey(ViewMode view);

// Sensor modes for an output frame rate, preferred first; the camera may not offer every one (the
// firmware lives in RAM, so an older image keeps running until the next replug).
std::vector<SensorMode> SensorModes(uint32_t outputFps, ViewMode view);

// The sensor mode a UVC format belongs to, or false for a format we do not process.
bool SensorModeOf(uint32_t packedW, uint32_t packedH, uint32_t fps, SensorMode* mode);

StereoFormat FormatOf(const SensorMode& m);

// Output frame of the given size, cropped from the main sensor image to keep the aspect ratio.
OutputFormat OutputFor(const StereoFormat& sf, uint32_t width, uint32_t height, PixelFormat format);

struct TypeSpec {
    uint32_t w, h, fps;
    PixelFormat fmt;
};

// The output sizes and rates offered to apps, NV12 then YUY2: Full HD at 60 fps only, so every app
// gets that whatever it would pick by itself (Chromium takes the format nearest 640x480 at 30 fps),
// or Full HD and HD at 60 and 30 fps.
std::vector<TypeSpec> OutputTypes(bool prefer60, bool fullHdOnly);

HRESULT MakeVideoType(const TypeSpec& t, IMFMediaType** out);

}  // namespace ps5cam
