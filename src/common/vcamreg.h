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
// Registers (or removes) the "PS5 Camera Depth" virtual camera. Requires administrator rights.
HRESULT SetDepthCamera(bool on, std::wstring& message);

// Hides (or shows) the raw "USB Camera-OV580" from normal camera enumeration via the documented
// SensorCameraMode / SkipCameraEnumeration device parameters, then restarts the device.
HRESULT SetPhysicalCameraHidden(bool hidden, std::wstring& message);
bool IsPhysicalCameraHidden();

// Names (or stops naming) the device MFT (ps5cam-dmft.dll, COM-registered separately) on the
// camera's interfaces, so Frame Server runs the effect inside the camera itself, and renames the
// camera to PS5 Camera (or back). With restartFrameServer, restarts Frame Server when that changes
// for the plugged-in camera (this ends every app's camera stream). Requires administrator rights.
HRESULT SetDeviceMft(bool on, std::wstring& message, bool restartFrameServer = true);
bool IsDeviceMftSet();  // fully on for the plugged-in camera
// Stops Frame Server (it starts again when an app opens a camera). False if it did not stop.
bool StopFrameServer();

// A device restart refused while an app held the camera leaves it waiting for a reboot (Frame
// Server answers MF_E_REBOOT_REQUIRED); re-enumerating it through its USB port (VBUS stays on, so
// does the firmware) clears that. Requires administrator rights.
bool CameraNeedsRestart();
bool CycleCamera();

}  // namespace ps5cam
