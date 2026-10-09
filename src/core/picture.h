#pragma once
// The picture settings a user changes — ps5cam-ctl set and the tray on Windows, /etc/ps5cam/bokeh.conf
// on Linux — with one set of names, units, defaults and limits, and how they become EffectSettings.
#include <algorithm>
#include <cstdint>

#include "pipeline.h"

namespace ps5cam {

struct PictureSettings {
    uint32_t mode = 0;          // ViewMode 0..4
    uint32_t blur = 25;         // 0..100 (25 light, 50 medium, 75 strong, 100 maximum)
    bool autoFocus = true;
    uint32_t focus = 50;        // manual focus 0 (far) .. 100 (near)
    uint32_t highlights = 150;  // bokeh highlight gain x100, 0..400
    uint32_t temporal = 40;     // temporal weight x100, 5..100
    bool autoBrightness = true;
    uint32_t maxGain = 60;      // x10, 10..160
    uint32_t denoise = 90;      // temporal noise reduction 0 (off) .. 100
    uint32_t antiFlicker = 0;   // AntiFlicker (flicker.h): 0 auto, 1 50 Hz, 2 60 Hz, 3 off
    uint32_t sharpen = 50;      // edge sharpening 0 (off) .. 100
    // The picture's tone, 50 = as the camera gives it (Windows' camera settings: Brightness,
    // Contrast, Saturation): brightness 0..100 is -1..+1 stop on top of the auto brightness,
    // contrast x0.5..x1.5 around mid grey, saturation x0..x2 of the colour.
    uint32_t brightness = 50;
    uint32_t contrast = 50;
    uint32_t saturation = 50;
};

// Values out of range: a scale is clamped, a choice falls back to its default.
inline void ClampPicture(PictureSettings& p)
{
    const PictureSettings defaults;
    if (p.mode > 4) p.mode = defaults.mode;
    p.blur = std::min<uint32_t>(p.blur, 100);
    p.focus = std::min<uint32_t>(p.focus, 100);
    p.highlights = std::min<uint32_t>(p.highlights, 400);
    p.temporal = std::clamp<uint32_t>(p.temporal, 5, 100);
    p.maxGain = std::clamp<uint32_t>(p.maxGain, 10, 160);
    p.denoise = std::min<uint32_t>(p.denoise, 100);
    if (p.antiFlicker > 3) p.antiFlicker = defaults.antiFlicker;
    p.sharpen = std::min<uint32_t>(p.sharpen, 100);
    p.brightness = std::min<uint32_t>(p.brightness, 100);
    p.contrast = std::min<uint32_t>(p.contrast, 100);
    p.saturation = std::min<uint32_t>(p.saturation, 100);
}

inline void ApplyPicture(const PictureSettings& p, EffectSettings& e)
{
    e.mode = static_cast<ViewMode>(p.mode);
    e.blurStrength = p.blur / 100.0f;
    e.autoFocus = p.autoFocus;
    e.manualFocus = p.focus / 100.0f;
    e.highlights = p.highlights / 100.0f;
    e.temporal = p.temporal / 100.0f;
    e.autoBrightness = p.autoBrightness;
    e.maxGain = p.maxGain / 10.0f;
    e.denoise = p.denoise / 100.0f;
    e.sharpen = p.sharpen / 100.0f;
    e.brightness = (static_cast<float>(p.brightness) - 50.0f) / 50.0f;
    e.contrast = 0.5f + p.contrast / 100.0f;
    e.saturation = p.saturation / 50.0f;
}

}  // namespace ps5cam
