#pragma once
// Registry settings -> the GPU pipeline's effect settings, shared by the device MFT and the virtual
// camera so both show the same picture for the same settings.
#include "pipeline.h"
#include "settings.h"

namespace ps5cam {

inline void ApplySettings(const Settings& s, EffectSettings& e)
{
    e.mode = static_cast<ViewMode>(s.mode);
    e.blurStrength = s.blur / 100.0f;
    // Windows' "Standard blur" is meant to hide the room rather than to look like a lens.
    if (s.mode == 0 && s.blurStyle == kBlurStandard) e.blurStrength = 1.0f;
    e.autoFocus = s.autoFocus;
    e.manualFocus = s.focus / 100.0f;
    e.highlights = s.highlights / 100.0f;
    e.temporal = s.temporal / 100.0f;
    e.autoBrightness = s.autoBrightness;
    e.maxGain = s.maxGain / 10.0f;
    e.denoise = s.denoise / 100.0f;
}

}  // namespace ps5cam
