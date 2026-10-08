#include "firmware.h"

#include <initguid.h>
#include <cfgmgr32.h>
#include <usbiodef.h>
#include <winusb.h>

#include <algorithm>
#include <cwctype>
#include <fstream>

namespace ps5cam {

namespace {

// Bootloader protocol (OV580 ROM, same as PS4 camera): vendor OUT requests with bRequest 0,
// wValue = low 16 bits of the load address, wIndex = 0x14 + high part; then a single byte 0x5B
// written to wValue 0x2200 / wIndex 0x8018 jumps into the loaded image.
constexpr uint16_t kChunk = 512;
constexpr uint16_t kBaseIndex = 0x14;

std::wstring Lower(std::wstring s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

std::wstring Win32Message(DWORD code)
{
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
        code, MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
    std::wstring s = buf ? buf : L"";
    LocalFree(buf);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r')) s.pop_back();
    return std::to_wstring(code) + L" " + s;
}

}  // namespace

bool IsBootDevicePath(const std::wstring& path)
{
    return Lower(path).find(L"vid_05a9&pid_0580") != std::wstring::npos;
}

bool LoadFirmwareFile(const std::wstring& path, std::vector<uint8_t>& image, std::wstring& error)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        error = L"cannot open " + path;
        return false;
    }
    auto size = static_cast<size_t>(f.tellg());
    if (size < 4096 || size > 0x40000) {
        error = L"unexpected firmware size " + std::to_wstring(size);
        return false;
    }
    image.resize(size);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(image.data()), static_cast<std::streamsize>(size));
    return true;
}

bool UploadFirmware(const std::wstring& devicePath, const std::vector<uint8_t>& image, std::wstring& error)
{
    HANDLE file = CreateFileW(devicePath.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = L"CreateFile: " + Win32Message(GetLastError());
        return false;
    }
    WINUSB_INTERFACE_HANDLE usb = nullptr;
    if (!WinUsb_Initialize(file, &usb)) {
        error = L"WinUsb_Initialize: " + Win32Message(GetLastError());
        CloseHandle(file);
        return false;
    }
    ULONG timeout = 2000;
    WinUsb_SetPipePolicy(usb, 0, PIPE_TRANSFER_TIMEOUT, sizeof(timeout), &timeout);

    bool ok = true;
    uint32_t address = 0;
    for (size_t pos = 0; pos < image.size() && ok; pos += kChunk) {
        auto size = static_cast<uint16_t>(std::min<size_t>(kChunk, image.size() - pos));
        WINUSB_SETUP_PACKET setup = {0x40, 0x00, static_cast<USHORT>(address & 0xFFFF),
            static_cast<USHORT>(kBaseIndex + (address >> 16)), size};
        ULONG sent = 0;
        if (!WinUsb_ControlTransfer(usb, setup, const_cast<uint8_t*>(image.data() + pos), size, &sent, nullptr) ||
            sent != size) {
            error = L"chunk at " + std::to_wstring(pos) + L": " + Win32Message(GetLastError());
            ok = false;
        }
        address += size;
    }
    if (ok) {
        uint8_t go = 0x5B;
        WINUSB_SETUP_PACKET setup = {0x40, 0x00, 0x2200, 0x8018, 1};
        ULONG sent = 0;
        // The device may drop off the bus before acknowledging the jump; that is not a failure.
        WinUsb_ControlTransfer(usb, setup, &go, 1, &sent, nullptr);
    }
    WinUsb_Free(usb);
    CloseHandle(file);
    return ok;
}

std::vector<std::wstring> FindBootDevices()
{
    std::vector<std::wstring> result;
    ULONG len = 0;
    if (CM_Get_Device_Interface_List_SizeW(&len, const_cast<GUID*>(&GUID_DEVINTERFACE_USB_DEVICE), nullptr,
            CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS || len < 2)
        return result;
    std::vector<wchar_t> list(len);
    if (CM_Get_Device_Interface_ListW(const_cast<GUID*>(&GUID_DEVINTERFACE_USB_DEVICE), nullptr, list.data(), len,
            CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS)
        return result;
    for (const wchar_t* p = list.data(); *p; p += wcslen(p) + 1)
        if (IsBootDevicePath(p)) result.emplace_back(p);
    return result;
}

}  // namespace ps5cam
