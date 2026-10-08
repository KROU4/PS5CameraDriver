#pragma once
// UVC controls of the physical camera that the driver sets itself.
#include <windows.h>
#include <unknwn.h>

namespace ps5cam {

// Anti-flicker (power line frequency): 0 off, 1 50 Hz, 2 60 Hz. camera: anything that answers
// IKsControl for the camera filter (its media source, or the device MFT's view of it).
HRESULT SetPowerLineFrequency(IUnknown* camera, int value);

}  // namespace ps5cam
