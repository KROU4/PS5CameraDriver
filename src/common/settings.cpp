#include "settings.h"

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
    s.highlights = std::min<DWORD>(ReadDword(key, L"Highlights", s.highlights), 400);
    s.temporal = std::clamp<DWORD>(ReadDword(key, L"Temporal", s.temporal), 5, 100);
    s.autoBrightness = ReadDword(key, L"AutoBrightness", s.autoBrightness) != 0;
    s.maxGain = std::clamp<DWORD>(ReadDword(key, L"MaxGain", s.maxGain), 10, 160);
    RegCloseKey(key);
    return s;
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
    WriteDword(key, L"Highlights", s.highlights);
    WriteDword(key, L"Temporal", s.temporal);
    WriteDword(key, L"AutoBrightness", s.autoBrightness);
    WriteDword(key, L"MaxGain", s.maxGain);
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

}  // namespace ps5cam
