#pragma once
// System registration of the "PS5 Camera" virtual camera and hiding of the raw OV580 camera.
#include <windows.h>

#include <string>

namespace ps5cam {

// Symbolic link of the physical camera's video interface (MI_00 of 05A9:058C), empty if absent.
std::wstring FindPhysicalCameraLink();
// Device instance ID of the physical camera interface (USB\VID_05A9&PID_058C&MI_00\...), empty if absent.
std::wstring FindPhysicalCameraInstance();

// Creates (or reopens) the system-wide virtual camera and starts it. Associates the physical camera
// when it is present so Frame Server hands it to the media source. Requires administrator rights.
HRESULT RegisterVirtualCamera(std::wstring& message);
HRESULT RemoveVirtualCamera(std::wstring& message);

// Hides (or shows) the raw "USB Camera-OV580" from normal camera enumeration via the documented
// SensorCameraMode / SkipCameraEnumeration device parameters, then restarts the device.
HRESULT SetPhysicalCameraHidden(bool hidden, std::wstring& message);
bool IsPhysicalCameraHidden();

}  // namespace ps5cam
