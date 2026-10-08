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
    // Windows' "Standard blur" is meant to hide the room rather than to look like a lens.
    if (s.mode == 0 && s.blurStyle == kBlurStandard) e.blurStrength = 1.0f;
    e.depthView = s.depthView;
}

}  // namespace ps5cam
