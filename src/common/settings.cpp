#include "settings.h"

#include <aclapi.h>
#include <sddl.h>

#include <algorithm>

#include "ids.h"

namespace ps5cam {

namespace {

DWORD ReadDword(HKEY key, const wchar_t* name, DWORD fallback)
{
    DWORD v = 0, size = sizeof(v), type = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&v), &size) == ERROR_SUCCESS &&
        type == REG_DWORD)
        return v;
    return fallback;
}

void WriteDword(HKEY key, const wchar_t* name, DWORD v)
{
    RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v));
}

std::wstring ReadString(HKEY key, const wchar_t* name)
{
    wchar_t buf[256] = {};
    DWORD size = sizeof(buf) - sizeof(wchar_t), type = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(buf), &size) == ERROR_SUCCESS &&
        type == REG_SZ)
        return buf;
    return {};
}

void WriteString(HKEY key, const wchar_t* name, const std::wstring& v)
{
    RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()),
        static_cast<DWORD>((v.size() + 1) * sizeof(wchar_t)));
}

HKEY Open(const wchar_t* sub, bool write)
{
    std::wstring path = kRegRoot;
    if (sub && *sub) path += std::wstring(L"\\") + sub;
    HKEY key = nullptr;
    LSTATUS st = write ? RegCreateKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, nullptr, 0, KEY_READ | KEY_WRITE,
                             nullptr, &key, nullptr)
                       : RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ, &key);
    return st == ERROR_SUCCESS ? key : nullptr;
}

}  // namespace

uint32_t RegionMainsHz()
{
    // ISO 3166 codes of the countries on 60 Hz mains (Japan, half and half, is left at 50 Hz).
    static const wchar_t* const k60[] = {L"US", L"CA", L"MX", L"GT", L"BZ", L"SV", L"HN", L"NI", L"CR", L"PA",
        L"CO", L"VE", L"EC", L"PE", L"BR", L"SR", L"GY", L"CU", L"DO", L"HT", L"PR", L"BS", L"BM", L"KY", L"TC",
        L"VG", L"VI", L"AG", L"KN", L"LC", L"TT", L"AW", L"MS", L"KR", L"TW", L"PH", L"SA", L"LR", L"GU", L"AS",
        L"MP", L"FM", L"MH", L"PW"};
    wchar_t iso[8] = {};
    const GEOID geo = GetUserGeoID(GEOCLASS_NATION);
    if (geo == GEOID_NOT_AVAILABLE || !GetGeoInfoW(geo, GEO_ISO2, iso, 8, 0)) return 50;
    for (const wchar_t* c : k60)
        if (wcscmp(iso, c) == 0) return 60;
    return 50;
}

Settings LoadSettings()
{
    Settings s;
    HKEY key = Open(L"", false);
    if (!key) return s;
    s.mode = std::min<DWORD>(ReadDword(key, L"Mode", s.mode), 4);
    s.blur = std::min<DWORD>(ReadDword(key, L"Blur", s.blur), 100);
    s.autoFocus = ReadDword(key, L"AutoFocus", s.autoFocus) != 0;
    s.focus = std::min<DWORD>(ReadDword(key, L"Focus", s.focus), 100);
    s.prefer60 = ReadDword(key, L"Prefer60", s.prefer60) != 0;
    s.fullHdOnly = ReadDword(key, L"FullHdOnly", s.fullHdOnly) != 0;
    s.highlights = std::min<DWORD>(ReadDword(key, L"Highlights", s.highlights), 400);
    s.temporal = std::clamp<DWORD>(ReadDword(key, L"Temporal", s.temporal), 5, 100);
    s.autoBrightness = ReadDword(key, L"AutoBrightness", s.autoBrightness) != 0;
    s.maxGain = std::clamp<DWORD>(ReadDword(key, L"MaxGain", s.maxGain), 10, 160);
    s.denoise = std::min<DWORD>(ReadDword(key, L"Denoise", s.denoise), 100);
    s.antiFlicker = ReadDword(key, L"AntiFlicker", s.antiFlicker);
    if (s.antiFlicker > 3) s.antiFlicker = 0;
    s.blurStyle = ReadDword(key, L"BlurStyle", s.blurStyle) == kBlurStandard ? kBlurStandard : kBlurPortrait;
    s.mainsHz = ReadDword(key, L"MainsHz", s.mainsHz);
    if (s.mainsHz != 50 && s.mainsHz != 60) s.mainsHz = 0;
    s.depthCamera = ReadDword(key, L"DepthCamera", s.depthCamera) != 0;
    s.depthView = ReadDword(key, L"DepthView", s.depthView) == 1 ? 1 : 0;
    RegCloseKey(key);
    return s;
}

bool WriteSetting(const wchar_t* name, uint32_t value)
{
    HKEY key = Open(L"", true);
    if (!key) return false;
    const DWORD v = value;
    const bool ok = RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v)) == ERROR_SUCCESS;
    RegCloseKey(key);
    return ok;
}

bool SaveSettings(const Settings& s)
{
    HKEY key = Open(L"", true);
    if (!key) return false;
    WriteDword(key, L"Mode", s.mode);
    WriteDword(key, L"Blur", s.blur);
    WriteDword(key, L"AutoFocus", s.autoFocus);
    WriteDword(key, L"Focus", s.focus);
    WriteDword(key, L"Prefer60", s.prefer60);
    WriteDword(key, L"FullHdOnly", s.fullHdOnly);
    WriteDword(key, L"Highlights", s.highlights);
    WriteDword(key, L"Temporal", s.temporal);
    WriteDword(key, L"AutoBrightness", s.autoBrightness);
    WriteDword(key, L"MaxGain", s.maxGain);
    WriteDword(key, L"Denoise", s.denoise);
    WriteDword(key, L"AntiFlicker", s.antiFlicker);
    WriteDword(key, L"BlurStyle", s.blurStyle);
    WriteDword(key, L"MainsHz", s.mainsHz);
    WriteDword(key, L"DepthCamera", s.depthCamera);
    WriteDword(key, L"DepthView", s.depthView);
    RegCloseKey(key);
    return true;
}

void SaveStatus(const Status& s)
{
    HKEY key = Open(L"Status", true);
    if (!key) return;
    WriteDword(key, L"Streaming", s.streaming);
    WriteDword(key, L"FpsX100", s.fpsX100);
    WriteDword(key, L"GpuUs", s.gpuUs);
    WriteDword(key, L"FocusX100", static_cast<DWORD>(s.focusX100));
    WriteString(key, L"Format", s.format);
    WriteString(key, L"Error", s.error);
    RegCloseKey(key);
}

Status LoadStatus()
{
    Status s;
    HKEY key = Open(L"Status", false);
    if (!key) return s;
    s.streaming = ReadDword(key, L"Streaming", 0) != 0;
    s.fpsX100 = ReadDword(key, L"FpsX100", 0);
    s.gpuUs = ReadDword(key, L"GpuUs", 0);
    s.focusX100 = static_cast<int32_t>(ReadDword(key, L"FocusX100", 0));
    s.format = ReadString(key, L"Format");
    s.error = ReadString(key, L"Error");
    RegCloseKey(key);
    return s;
}

StoredCalibration LoadCalibration(const wchar_t* sensorKey)
{
    StoredCalibration c;
    HKEY key = Open(L"Calibration", false);
    if (!key) return c;
    std::wstring base = sensorKey;
    DWORD valid = ReadDword(key, (base + L"Valid").c_str(), 0);
    c.valid = valid != 0;
    c.dy = static_cast<int32_t>(ReadDword(key, (base + L"DyX1000").c_str(), 0)) / 1000.0f;
    c.rotation = static_cast<int32_t>(ReadDword(key, (base + L"RotX1000").c_str(), 0)) / 1000.0f;
    RegCloseKey(key);
    return c;
}

void SaveCalibration(const wchar_t* sensorKey, const StoredCalibration& c)
{
    HKEY key = Open(L"Calibration", true);
    if (!key) return;
    std::wstring base = sensorKey;
    WriteDword(key, (base + L"Valid").c_str(), c.valid);
    WriteDword(key, (base + L"DyX1000").c_str(), static_cast<DWORD>(static_cast<int32_t>(c.dy * 1000)));
    WriteDword(key, (base + L"RotX1000").c_str(), static_cast<DWORD>(static_cast<int32_t>(c.rotation * 1000)));
    RegCloseKey(key);
}

uint32_t LoadCalibrationRequest()
{
    HKEY key = Open(L"Calibration", false);
    if (!key) return 0;
    DWORD v = ReadDword(key, L"Request", 0);
    RegCloseKey(key);
    return v;
}

uint32_t LoadCalibrationHandled()
{
    HKEY key = Open(L"Calibration", false);
    if (!key) return 0;
    DWORD v = ReadDword(key, L"Handled", 0);
    RegCloseKey(key);
    return v;
}

void SaveCalibrationHandled(uint32_t value)
{
    HKEY key = Open(L"Calibration", true);
    if (!key) return;
    WriteDword(key, L"Handled", value);
    RegCloseKey(key);
}

void BumpCalibrationRequest()
{
    HKEY key = Open(L"Calibration", true);
    if (!key) return;
    WriteDword(key, L"Request", ReadDword(key, L"Request", 0) + 1);
    RegCloseKey(key);
}

uint32_t TakeRecordRequest()
{
    // Users may create keys under the root (and write to keys that inherit its ACL), so a request
    // counts only in a key owned by Administrators or SYSTEM with its own protected ACL, which is
    // what ps5cam-ctl record sets up.
    std::wstring path = std::wstring(kRegRoot) + L"\\Debug";
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ | KEY_SET_VALUE, &key) != ERROR_SUCCESS) return 0;
    DWORD frames = ReadDword(key, L"RecordFrames", 0);
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    bool trusted = GetSecurityInfo(key, SE_REGISTRY_KEY, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner,
                       nullptr, nullptr, nullptr, &sd) == ERROR_SUCCESS &&
                   owner && (IsWellKnownSid(owner, WinBuiltinAdministratorsSid) || IsWellKnownSid(owner, WinLocalSystemSid)) &&
                   GetSecurityDescriptorControl(sd, &control, &revision) && (control & SE_DACL_PROTECTED);
    if (sd) LocalFree(sd);
    // A request that cannot be cleared would start a new recording after every finished one.
    const DWORD zero = 0;
    bool cleared = !frames || RegSetValueExW(key, L"RecordFrames", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&zero),
                                  sizeof(zero)) == ERROR_SUCCESS;
    RegCloseKey(key);
    return trusted && cleared ? std::min<DWORD>(frames, 600) : 0;
}

bool RequestRecording(uint32_t frames)
{
    // Owner Administrators; SYSTEM and admins write, the media source (LOCAL SERVICE) reads and
    // clears the request, users only read. Re-applied every time, so a key made by a user is taken over.
    HKEY key = Open(L"Debug", true);  // fails on a key whose ACL shuts admins out; taken over below
    if (key) RegCloseKey(key);
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                    L"O:BAD:P(A;CI;KA;;;SY)(A;CI;KA;;;BA)(A;CI;0x2001b;;;LS)(A;CI;KR;;;BU)", SDDL_REVISION_1, &sd,
                    nullptr))
        return false;
    BOOL present = FALSE, defaulted = FALSE;
    PACL dacl = nullptr;
    PSID owner = nullptr;
    std::wstring path = std::wstring(L"MACHINE\\") + kRegRoot + L"\\Debug";
    bool ok = GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted) && present &&
              GetSecurityDescriptorOwner(sd, &owner, &defaulted) && owner &&
              SetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()), SE_REGISTRY_KEY,
                  OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, owner,
                  nullptr, dacl, nullptr) == ERROR_SUCCESS;
    LocalFree(sd);
    if (!ok || (key = Open(L"Debug", true)) == nullptr) return false;
    WriteDword(key, L"RecordFrames", frames);
    RegCloseKey(key);
    return true;
}

}  // namespace ps5cam
