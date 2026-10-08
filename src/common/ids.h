#pragma once
#include <windows.h>

namespace ps5cam {

// COM class of the virtual camera media source (registered in HKLM by ps5cam-vcam.dll).
// {7A3C5E21-9B4D-4F6E-A1C2-5D8E9F0B3A47}
inline constexpr GUID kSourceClsid = {0x7a3c5e21, 0x9b4d, 0x4f6e, {0xa1, 0xc2, 0x5d, 0x8e, 0x9f, 0x0b, 0x3a, 0x47}};
inline constexpr wchar_t kSourceClsidString[] = L"{7A3C5E21-9B4D-4F6E-A1C2-5D8E9F0B3A47}";
inline constexpr wchar_t kCameraName[] = L"PS5 Camera";

// Shared configuration and status. The installer grants Users and LOCAL SERVICE write access.
inline constexpr wchar_t kRegRoot[] = L"SOFTWARE\\PS5Camera";
// Values only administrators may change (they make the SYSTEM service restart devices).
inline constexpr wchar_t kRegServiceKey[] = L"SOFTWARE\\PS5Camera\\Service";

}  // namespace ps5cam
