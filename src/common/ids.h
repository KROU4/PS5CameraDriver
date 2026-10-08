#pragma once
#include <windows.h>

namespace ps5cam {

// COM class of the virtual camera media source (registered in HKLM by ps5cam-vcam.dll).
// {7A3C5E21-9B4D-4F6E-A1C2-5D8E9F0B3A47}
inline constexpr GUID kSourceClsid = {0x7a3c5e21, 0x9b4d, 0x4f6e, {0xa1, 0xc2, 0x5d, 0x8e, 0x9f, 0x0b, 0x3a, 0x47}};
inline constexpr wchar_t kSourceClsidString[] = L"{7A3C5E21-9B4D-4F6E-A1C2-5D8E9F0B3A47}";
inline constexpr wchar_t kCameraName[] = L"PS5 Camera";

// COM class of the depth camera's media source (ps5cam-vcam.dll too): the "PS5 Camera Depth"
// virtual camera shows the depth of the frames "PS5 Camera" delivers. {5A6706D5-9E82-4A92-9B48-1702B814BA51}
inline constexpr GUID kDepthSourceClsid = {0x5a6706d5, 0x9e82, 0x4a92, {0x9b, 0x48, 0x17, 0x02, 0xb8, 0x14, 0xba, 0x51}};
inline constexpr wchar_t kDepthSourceClsidString[] = L"{5A6706D5-9E82-4A92-9B48-1702B814BA51}";
inline constexpr wchar_t kDepthCameraName[] = L"PS5 Camera Depth";

// COM class of the device MFT (ps5cam-dmft.dll), named in the camera interface's
// CameraDeviceMftCLSIDChain value. {1E72D619-C6E0-408B-BD95-6D6334213E9B}
inline constexpr GUID kDmftClsid = {0x1e72d619, 0xc6e0, 0x408b, {0xbd, 0x95, 0x6d, 0x63, 0x34, 0x21, 0x3e, 0x9b}};
inline constexpr wchar_t kDmftClsidString[] = L"{1E72D619-C6E0-408B-BD95-6D6334213E9B}";

// Shared configuration and status. The installer grants Users and LOCAL SERVICE write access.
inline constexpr wchar_t kRegRoot[] = L"SOFTWARE\\PS5Camera";
// Values only administrators may change (they make the SYSTEM service restart devices).
inline constexpr wchar_t kRegServiceKey[] = L"SOFTWARE\\PS5Camera\\Service";

}  // namespace ps5cam
