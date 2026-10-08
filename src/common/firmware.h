#pragma once
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ps5cam {

// USB identity of the OV580 bridge in ROM boot mode and after the firmware has started.
constexpr uint16_t kVid = 0x05A9;
constexpr uint16_t kPidBoot = 0x0580;
constexpr uint16_t kPidCamera = 0x058C;

// Reads firmware.bin; returns false if missing or implausibly sized.
bool LoadFirmwareFile(const std::wstring& path, std::vector<uint8_t>& image, std::wstring& error);

// Uploads the image to a boot-mode device opened via its device interface path and starts it.
// The device re-enumerates as 05A9:058C shortly after success.
bool UploadFirmware(const std::wstring& devicePath, const std::vector<uint8_t>& image, std::wstring& error);

// Lists device interface paths of present boot-mode devices (any driver exposing GUID_DEVINTERFACE_USB_DEVICE).
std::vector<std::wstring> FindBootDevices();

bool IsBootDevicePath(const std::wstring& path);

}  // namespace ps5cam
