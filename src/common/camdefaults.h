#pragma once
// Windows' "Background effects" for the camera (Settings > Bluetooth & devices > Cameras, and the
// camera effects of apps): the device MFT answers the background segmentation control with the
// bokeh. Windows also keeps a default for it once the user has touched it there and writes it to the
// camera whenever an app starts it; the service keeps that default in line with the bokeh switch of
// the tray and ps5cam-ctl, or Windows would switch the effect back at the next start.
#include <windows.h>

#include <cstdint>
#include <string>

#include "settings.h"

namespace ps5cam {

// KSCAMERA_EXTENDEDPROP_BACKGROUNDSEGMENTATION_* flags for the settings: off, or blur (standard),
// or blur + shallow focus (portrait).
uint64_t BackgroundEffectFlags(const Settings& s);

// Updates Windows' saved default of the background effect, if there is one, to the settings.
// Needs administrator rights (the service does it). S_OK: updated; S_FALSE: nothing to update;
// HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED): the camera is not connected.
HRESULT SyncBackgroundEffectDefault(const Settings& s, std::wstring* message = nullptr);

// Windows' saved default of the background effect, in words (for ps5cam-ctl status).
std::wstring DescribeBackgroundEffectDefault();

}  // namespace ps5cam
