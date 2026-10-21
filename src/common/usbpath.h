#pragma once
// Where the camera sits on USB: the hubs between it and the host controller. Its video endpoint
// (isochronous, 3 x 16 x 1024 bytes per 125 us, the only alternate setting, whatever the format)
// reserves nearly all periodic bandwidth of a SuperSpeed link, so a hub whose upstream link it
// shares with other devices can leave it streaming without a single frame.
#include <cwctype>
#include <string>
#include <vector>

namespace ps5cam {

struct UsbPath {
    bool found = false;              // the running camera (05A9:058C) is plugged in
    bool complete = false;           // its parents were read up to the root hub
    std::vector<std::wstring> hubs;  // external hubs up to the root hub, nearest first (IDs without instance)
    unsigned neighbours = 0;         // other devices on the camera's own external hub
};

// Device IDs of root hubs (USB\ROOT_HUB30\..., USB\ROOT_HUB20\...) end the chain.
inline bool IsRootHubId(const std::wstring& id)
{
    static const wchar_t kPrefix[] = L"usb\\root_hub";
    for (size_t i = 0; kPrefix[i]; ++i)
        if (i >= id.size() || std::towlower(id[i]) != kPrefix[i]) return false;
    return true;
}

// "USB\VID_174C&PID_3074\6&10cdd27&0&3" -> "USB\VID_174C&PID_3074": the model, not the port.
inline std::wstring WithoutInstance(const std::wstring& id)
{
    const size_t slash = id.rfind(L'\\');
    return slash == std::wstring::npos || slash == 0 ? id : id.substr(0, slash);
}

// One line for logs and ps5cam-ctl status, e.g. "behind 1 USB hub (USB\VID_174C&PID_3074), 2 other
// devices on it" or "directly on a root port".
inline std::wstring DescribeUsbPath(const UsbPath& p)
{
    if (!p.found) return L"camera not found";
    std::wstring s;
    if (!p.complete) {
        s = L"path unknown";
        if (!p.hubs.empty()) s += L", at least " + std::to_wstring(p.hubs.size()) + L" USB hub(s) (" + p.hubs[0] + L")";
        return s;
    }
    if (p.hubs.empty()) {
        s = L"directly on a root port";
    } else {
        s = L"behind " + std::to_wstring(p.hubs.size()) + (p.hubs.size() == 1 ? L" USB hub (" : L" USB hubs (");
        for (size_t i = 0; i < p.hubs.size(); ++i) s += (i ? L" -> " : L"") + p.hubs[i];
        s += L")";
    }
    if (p.neighbours)
        s += L", " + std::to_wstring(p.neighbours) + (p.neighbours == 1 ? L" other device on it" : L" other devices on it");
    return s;
}

// Reads the plugged-in camera's parent chain from the device tree (no rights needed).
UsbPath FindCameraUsbPath();

}  // namespace ps5cam
