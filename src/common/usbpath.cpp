#include "usbpath.h"

#include <windows.h>
#include <cfgmgr32.h>

namespace ps5cam {

namespace {

std::wstring DeviceId(DEVINST inst)
{
    wchar_t id[MAX_DEVICE_ID_LEN] = {};
    return CM_Get_Device_IDW(inst, id, MAX_DEVICE_ID_LEN, 0) == CR_SUCCESS ? id : L"";
}

DEVINST FindCameraDevice()
{
    const ULONG flags = CM_GETIDLIST_FILTER_ENUMERATOR | CM_GETIDLIST_FILTER_PRESENT;
    std::vector<wchar_t> ids;
    CONFIGRET r = CR_BUFFER_SMALL;
    for (int attempt = 0; r == CR_BUFFER_SMALL && attempt < 3; ++attempt) {  // a device arriving meanwhile
        ULONG len = 0;
        if (CM_Get_Device_ID_List_SizeW(&len, L"USB", flags) != CR_SUCCESS) return 0;
        ids.assign(len, L'\0');
        r = CM_Get_Device_ID_ListW(L"USB", ids.data(), len, flags);
    }
    if (r != CR_SUCCESS) return 0;
    for (const wchar_t* p = ids.data(); *p; p += wcslen(p) + 1) {
        if (_wcsnicmp(p, L"USB\\VID_05A9&PID_058C\\", 22) != 0) continue;  // the composite device, not an interface
        DEVINST dev = 0;
        if (CM_Locate_DevNodeW(&dev, const_cast<wchar_t*>(p), CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS) return dev;
    }
    return 0;
}

}  // namespace

UsbPath FindCameraUsbPath()
{
    UsbPath path;
    const DEVINST camera = FindCameraDevice();
    if (!camera) return path;
    path.found = true;
    DEVINST parent = 0;
    if (CM_Get_Parent(&parent, camera, 0) != CR_SUCCESS) return path;
    // Everything else plugged into the same external hub shares its upstream link with the camera
    // (root ports have a link each).
    DEVINST child = 0;
    if (!IsRootHubId(DeviceId(parent)))
        for (CONFIGRET r = CM_Get_Child(&child, parent, 0); r == CR_SUCCESS; r = CM_Get_Sibling(&child, child, 0))
            if (child != camera) ++path.neighbours;
    for (int depth = 0; depth < 8; ++depth) {  // USB allows five hubs; 8 bounds a strange tree
        const std::wstring id = DeviceId(parent);
        if (IsRootHubId(id)) {
            path.complete = true;
            break;
        }
        if (_wcsnicmp(id.c_str(), L"USB\\", 4) != 0) break;  // also an empty ID: the walk failed
        path.hubs.push_back(WithoutInstance(id));
        if (CM_Get_Parent(&parent, parent, 0) != CR_SUCCESS) break;
    }
    return path;
}

}  // namespace ps5cam
