#pragma once
// Registry settings -> the GPU pipeline's effect settings, shared by the device MFT and the virtual
// camera so both show the same picture for the same settings.
#include "picture.h"
#include "pipeline.h"
#include "settings.h"

namespace ps5cam {

inline void ApplySettings(const Settings& s, EffectSettings& e)
{
    ApplyPicture(s, e);
    // Windows' "Standard blur" hides the room at a fixed strength, stronger than the default
    // "Portrait blur" (the blur setting, light unless changed).
    if (s.mode == 0 && s.blurStyle == kBlurStandard) e.blurStrength = kStandardBlurStrength;
    e.depthView = s.depthView;
}

}  // namespace ps5cam
