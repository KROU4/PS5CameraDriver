#include "vcamreg.h"

#include <initguid.h>
#include <cfgmgr32.h>
#include <ks.h>
#include <ksmedia.h>
#include <mfapi.h>
#include <mfvirtualcamera.h>
#include <wrl/client.h>

#include <algorithm>
#include <vector>

#include "ids.h"

using Microsoft::WRL::ComPtr;

namespace ps5cam {

namespace {

std::wstring Lower(std::wstring s)
{
    std::transform(s.begin(), s.end(), s.begin(), ::towlower);
    return s;
}

std::wstring Hex(HRESULT hr)
{
    wchar_t b[16];
    swprintf_s(b, L"0x%08lX", static_cast<unsigned long>(hr));
    return b;
}

struct MfInit {
    HRESULT co;
    MfInit() : co(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) { MFStartup(MF_VERSION, MFSTARTUP_LITE); }
    ~MfInit()
    {
        MFShutdown();
        if (SUCCEEDED(co)) CoUninitialize();
    }
};

HRESULT OpenVirtualCamera(ComPtr<IMFVirtualCamera>& vcam)
{
    return MFCreateVirtualCamera(MFVirtualCameraType_SoftwareCameraSource, MFVirtualCameraLifetime_System,
        MFVirtualCameraAccess_AllUsers, kCameraName, kSourceClsidString, nullptr, 0, &vcam);
}

}  // namespace

std::wstring FindPhysicalCameraLink()
{
    const GUID categories[] = {KSCATEGORY_VIDEO_CAMERA, KSCATEGORY_SENSOR_CAMERA, KSCATEGORY_VIDEO};
    for (const GUID& cat : categories) {
        ULONG len = 0;
        if (CM_Get_Device_Interface_List_SizeW(&len, const_cast<GUID*>(&cat), nullptr,
                CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS || len < 2)
            continue;
        std::vector<wchar_t> list(len);
        if (CM_Get_Device_Interface_ListW(const_cast<GUID*>(&cat), nullptr, list.data(), len,
                CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS)
            continue;
        for (const wchar_t* p = list.data(); *p; p += wcslen(p) + 1)
            if (Lower(p).find(L"vid_05a9&pid_058c&mi_00") != std::wstring::npos) return p;
    }
    return {};
}

std::wstring FindPhysicalCameraInstance()
{
    ULONG len = 0;
    if (CM_Get_Device_ID_List_SizeW(&len, L"USB", CM_GETIDLIST_FILTER_ENUMERATOR | CM_GETIDLIST_FILTER_PRESENT) != CR_SUCCESS)
        return {};
    std::vector<wchar_t> ids(len);
    if (CM_Get_Device_ID_ListW(L"USB", ids.data(), len, CM_GETIDLIST_FILTER_ENUMERATOR | CM_GETIDLIST_FILTER_PRESENT) !=
        CR_SUCCESS)
        return {};
    for (const wchar_t* p = ids.data(); *p; p += wcslen(p) + 1)
        if (Lower(p).find(L"vid_05a9&pid_058c&mi_00") != std::wstring::npos) return p;
    return {};
}

HRESULT RegisterVirtualCamera(std::wstring& message)
{
    MfInit mf;
    ComPtr<IMFVirtualCamera> vcam;
    HRESULT hr = OpenVirtualCamera(vcam);
    if (FAILED(hr)) {
        message = L"MFCreateVirtualCamera failed " + Hex(hr);
        return hr;
    }
    std::wstring link = FindPhysicalCameraLink();
    if (!link.empty()) {
        HRESULT ah = vcam->AddDeviceSourceInfo(link.c_str());
        message = L"associated " + link + L" (" + Hex(ah) + L"); ";
    } else {
        message = L"physical camera not present, will be found at stream start; ";
    }
    hr = vcam->Start(nullptr);
    message += L"Start " + Hex(hr);
    vcam->Shutdown();
    return hr;
}

HRESULT RemoveVirtualCamera(std::wstring& message)
{
    MfInit mf;
    ComPtr<IMFVirtualCamera> vcam;
    HRESULT hr = OpenVirtualCamera(vcam);
    if (FAILED(hr)) {
        message = L"MFCreateVirtualCamera failed " + Hex(hr);
        return hr;
    }
    hr = vcam->Remove();
    message = L"Remove " + Hex(hr);
    vcam->Shutdown();
    return hr;
}

bool IsPhysicalCameraHidden()
{
    std::wstring id = FindPhysicalCameraInstance();
    DEVINST inst = 0;
    if (id.empty() || CM_Locate_DevNodeW(&inst, const_cast<wchar_t*>(id.c_str()), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS)
        return false;
    HKEY key = nullptr;
    if (CM_Open_DevNode_Key(inst, KEY_READ, 0, RegDisposition_OpenExisting, &key, CM_REGISTRY_HARDWARE) != CR_SUCCESS)
        return false;
    DWORD v = 0, size = sizeof(v);
    bool hidden = RegQueryValueExW(key, L"SkipCameraEnumeration", nullptr, nullptr, reinterpret_cast<BYTE*>(&v), &size) ==
                      ERROR_SUCCESS && v == 1;
    RegCloseKey(key);
    return hidden;
}

namespace {

std::vector<std::wstring> AllCameraInstances()
{
    // Includes cameras that are not plugged in right now (phantom devnodes keep their parameters).
    std::vector<std::wstring> result;
    ULONG len = 0;
    if (CM_Get_Device_ID_List_SizeW(&len, L"USB", CM_GETIDLIST_FILTER_ENUMERATOR) != CR_SUCCESS) return result;
    std::vector<wchar_t> ids(len);
    if (CM_Get_Device_ID_ListW(L"USB", ids.data(), len, CM_GETIDLIST_FILTER_ENUMERATOR) != CR_SUCCESS) return result;
    for (const wchar_t* p = ids.data(); *p; p += wcslen(p) + 1)
        if (Lower(p).find(L"vid_05a9&pid_058c&mi_00") != std::wstring::npos) result.emplace_back(p);
    return result;
}

DWORD RestartDevice(const std::wstring& id)
{
    // The UVC driver reads the parameters when it starts, so restart the camera function.
    std::wstring cmd = L"pnputil.exe /restart-device \"" + id + L"\"";
    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    DWORD code = 1;
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        if (WaitForSingleObject(pi.hProcess, 30000) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
        else code = WAIT_TIMEOUT;
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    return code;
}

}  // namespace

HRESULT SetPhysicalCameraHidden(bool hidden, std::wstring& message)
{
    std::wstring present = FindPhysicalCameraInstance();
    std::vector<std::wstring> targets = hidden ? std::vector<std::wstring>{} : AllCameraInstances();
    if (hidden && !present.empty()) targets.push_back(present);
    if (targets.empty()) {
        message = hidden ? L"physical camera not present" : L"no camera instances found";
        return hidden ? HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED) : S_OK;
    }
    HRESULT result = S_OK;
    message.clear();
    for (const auto& id : targets) {
        DEVINST inst = 0;
        if (CM_Locate_DevNodeW(&inst, const_cast<wchar_t*>(id.c_str()), CM_LOCATE_DEVNODE_PHANTOM) != CR_SUCCESS) continue;
        HKEY key = nullptr;
        if (CM_Open_DevNode_Key(inst, KEY_READ | KEY_WRITE, 0, RegDisposition_OpenAlways, &key, CM_REGISTRY_HARDWARE) !=
            CR_SUCCESS) {
            message = L"cannot open Device Parameters (run as administrator)";
            result = HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
            continue;
        }
        if (hidden) {
            DWORD one = 1;
            RegSetValueExW(key, L"SensorCameraMode", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one), sizeof(one));
            RegSetValueExW(key, L"SkipCameraEnumeration", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one), sizeof(one));
        } else {
            RegDeleteValueW(key, L"SensorCameraMode");
            RegDeleteValueW(key, L"SkipCameraEnumeration");
        }
        RegCloseKey(key);
        if (_wcsicmp(id.c_str(), present.c_str()) == 0) {
            DWORD code = RestartDevice(id);
            message = std::wstring(hidden ? L"hidden" : L"shown") + L", device restart exit code " + std::to_wstring(code);
            if (code != 0) result = HRESULT_FROM_WIN32(ERROR_GEN_FAILURE);
        } else if (message.empty()) {
            message = hidden ? L"hidden" : L"shown (camera not connected)";
        }
    }
    return result;
}

}  // namespace ps5cam