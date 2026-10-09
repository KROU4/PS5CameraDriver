#include "vcamreg.h"

#include <initguid.h>
#include <cfgmgr32.h>
#include <devpkey.h>
#include <winioctl.h>
#include <usbioctl.h>
#include <usbiodef.h>
#include <ks.h>
#include <ksmedia.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfvirtualcamera.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
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

// MFCreateVirtualCamera exists from Windows 11 on. Looked up when needed, not imported: an import
// would keep the programs that link this file (ps5cam-ctl, the service) from starting on Windows 10.
HRESULT CreateVirtualCamera(LPCWSTR name, LPCWSTR clsid, ComPtr<IMFVirtualCamera>& vcam)
{
    using CreateFn = decltype(&MFCreateVirtualCamera);
    static std::atomic<CreateFn> found{nullptr};  // only a success is kept: a failed load is tried again
    CreateFn create = found.load();
    if (!create) {
        HMODULE dll = LoadLibraryExW(L"mfsensorgroup.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (dll) create = reinterpret_cast<CreateFn>(GetProcAddress(dll, "MFCreateVirtualCamera"));
        if (!create) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);  // Windows 10: no virtual cameras
        found.store(create);
    }
    return create(MFVirtualCameraType_SoftwareCameraSource, MFVirtualCameraLifetime_System,
        MFVirtualCameraAccess_AllUsers, name, clsid, nullptr, 0, &vcam);
}

HRESULT OpenVirtualCamera(ComPtr<IMFVirtualCamera>& vcam)
{
    return CreateVirtualCamera(kCameraName, kSourceClsidString, vcam);
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

HRESULT SetDepthCamera(bool on, std::wstring& message)
{
    MfInit mf;
    ComPtr<IMFVirtualCamera> vcam;
    HRESULT hr = CreateVirtualCamera(kDepthCameraName, kDepthSourceClsidString, vcam);
    if (FAILED(hr)) {
        message = L"MFCreateVirtualCamera failed " + Hex(hr);
        return hr;
    }
    // No physical camera of its own: it shows what the device MFT publishes while "PS5 Camera" runs.
    hr = on ? vcam->Start(nullptr) : vcam->Remove();
    message = (on ? L"Start " : L"Remove ") + Hex(hr);
    vcam->Shutdown();
    if (!on && hr == MF_E_INVALIDREQUEST) {  // what Remove says when it was not registered
        message = L"not registered";
        return S_FALSE;
    }
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

// Re-enumerates the whole camera through its hub port (IOCTL_USB_HUB_CYCLE_PORT). The port keeps
// VBUS, so the firmware in the camera's RAM stays; the camera comes back in a few seconds.
bool CycleCameraPort()
{
    ULONG len = 0;
    if (CM_Get_Device_ID_List_SizeW(&len, L"USB", CM_GETIDLIST_FILTER_ENUMERATOR | CM_GETIDLIST_FILTER_PRESENT) != CR_SUCCESS)
        return false;
    std::vector<wchar_t> ids(len);
    if (CM_Get_Device_ID_ListW(L"USB", ids.data(), len, CM_GETIDLIST_FILTER_ENUMERATOR | CM_GETIDLIST_FILTER_PRESENT) !=
        CR_SUCCESS)
        return false;
    DEVINST dev = 0;
    for (const wchar_t* p = ids.data(); *p && !dev; p += wcslen(p) + 1) {
        std::wstring id = Lower(p);
        if (id.rfind(L"usb\\vid_05a9&pid_058c\\", 0) == 0)  // the composite device, not an interface
            CM_Locate_DevNodeW(&dev, const_cast<wchar_t*>(p), CM_LOCATE_DEVNODE_NORMAL);
    }
    DEVINST hub = 0;
    ULONG port = 0, size = sizeof(port);
    DEVPROPTYPE type = 0;
    wchar_t hubId[MAX_DEVICE_ID_LEN] = {};
    if (!dev || CM_Get_Parent(&hub, dev, 0) != CR_SUCCESS ||
        CM_Get_DevNode_PropertyW(dev, &DEVPKEY_Device_Address, &type, reinterpret_cast<PBYTE>(&port), &size, 0) !=
            CR_SUCCESS ||
        CM_Get_Device_IDW(hub, hubId, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS)
        return false;
    if (CM_Get_Device_Interface_List_SizeW(&len, const_cast<GUID*>(&GUID_DEVINTERFACE_USB_HUB), hubId,
            CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS || len < 2)
        return false;
    std::vector<wchar_t> hubPath(len);
    if (CM_Get_Device_Interface_ListW(const_cast<GUID*>(&GUID_DEVINTERFACE_USB_HUB), hubId, hubPath.data(), len,
            CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS)
        return false;
    HANDLE h = CreateFileW(hubPath.data(), GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    USB_CYCLE_PORT_PARAMS params = {port, 0};
    DWORD ret = 0;
    BOOL ok = DeviceIoControl(h, IOCTL_USB_HUB_CYCLE_PORT, &params, sizeof(params), &params, sizeof(params), &ret, nullptr);
    CloseHandle(h);
    return ok != FALSE;
}

bool NeedsRestart(const std::wstring& id)
{
    DEVINST inst = 0;
    ULONG status = 0, problem = 0;
    return CM_Locate_DevNodeW(&inst, const_cast<wchar_t*>(id.c_str()), CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS &&
           CM_Get_DevNode_Status(&status, &problem, inst, 0) == CR_SUCCESS && (status & DN_NEED_RESTART);
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
    // Refused while an app holds the camera (e.g. a background app that uses it as its source),
    // PnP marks the device as needing a reboot and Frame Server then refuses it with
    // MF_E_REBOOT_REQUIRED. Re-enumerating the camera through its port clears that.
    if (code != 0 || NeedsRestart(id)) {
        if (CycleCameraPort()) {
            for (int i = 0; i < 40 && FindPhysicalCameraInstance().empty(); ++i) Sleep(250);
            Sleep(500);
            code = FindPhysicalCameraInstance().empty() ? ERROR_DEVICE_NOT_CONNECTED : 0;
        }
    }
    return code;
}

}  // namespace

namespace {

// Every interface Frame Server may open, of the plugged-in camera or of every instance the camera
// ever had (each USB port gives it another one).
std::vector<std::wstring> CameraInterfaces(bool presentOnly)
{
    std::vector<std::wstring> links;
    const GUID categories[] = {KSCATEGORY_VIDEO_CAMERA, KSCATEGORY_SENSOR_CAMERA, KSCATEGORY_CAPTURE, KSCATEGORY_VIDEO};
    std::vector<std::wstring> instances = presentOnly ? std::vector<std::wstring>{} : AllCameraInstances();
    if (presentOnly) {
        std::wstring present = FindPhysicalCameraInstance();
        if (!present.empty()) instances.push_back(present);
    }
    for (const std::wstring& id : instances) {
        for (const GUID& cat : categories) {
            ULONG len = 0;
            if (CM_Get_Device_Interface_List_SizeW(&len, const_cast<GUID*>(&cat), const_cast<wchar_t*>(id.c_str()),
                    CM_GET_DEVICE_INTERFACE_LIST_ALL_DEVICES) != CR_SUCCESS || len < 2)
                continue;
            std::vector<wchar_t> list(len);
            if (CM_Get_Device_Interface_ListW(const_cast<GUID*>(&cat), const_cast<wchar_t*>(id.c_str()), list.data(), len,
                    CM_GET_DEVICE_INTERFACE_LIST_ALL_DEVICES) != CR_SUCCESS)
                continue;
            for (const wchar_t* p = list.data(); *p; p += wcslen(p) + 1) links.emplace_back(p);
        }
    }
    return links;
}

bool StopService(SC_HANDLE scm, const wchar_t* name)
{
    SC_HANDLE svc = OpenServiceW(scm, name, SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (!svc) return false;
    SERVICE_STATUS st = {};
    bool ok = ControlService(svc, SERVICE_CONTROL_STOP, &st) || GetLastError() == ERROR_SERVICE_NOT_ACTIVE;
    for (int i = 0; ok && i < 50 && QueryServiceStatus(svc, &st) && st.dwCurrentState != SERVICE_STOPPED; ++i) Sleep(100);
    ok = ok && QueryServiceStatus(svc, &st) && st.dwCurrentState == SERVICE_STOPPED;
    CloseServiceHandle(svc);
    return ok;
}

std::wstring ReadString(HKEY key, const wchar_t* name)
{
    wchar_t value[256] = {};
    DWORD size = sizeof(value) - sizeof(wchar_t), type = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(value), &size) != ERROR_SUCCESS ||
        type != REG_SZ)
        return {};
    return value;
}

bool WriteString(HKEY key, const wchar_t* name, const std::wstring& value)
{
    return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
               static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

bool DeleteValue(HKEY key, const wchar_t* name)
{
    LSTATUS st = RegDeleteValueW(key, name);
    return st == ERROR_SUCCESS || st == ERROR_FILE_NOT_FOUND;
}

// One interface: our device MFT and the PS5 Camera name, or back to the camera's own name.
bool ApplyDeviceMft(const std::wstring& link, bool on)
{
    HKEY key = nullptr;
    if (CM_Open_Device_Interface_KeyW(link.c_str(), KEY_READ | KEY_SET_VALUE, RegDisposition_OpenAlways, &key, 0) !=
        CR_SUCCESS)
        return false;
    // Apps list the camera by the interface's FriendlyName ("USB Camera-OV580" from the USB
    // descriptors): with the effect inside it, call it PS5 Camera, keeping the original to restore.
    const std::wstring name = ReadString(key, L"FriendlyName");
    const std::wstring saved = ReadString(key, L"PS5CameraOriginalName");
    bool ok = true;
    if (on) {
        // Both the single-MFT value and the chain (Windows 11 reads the chain first).
        const std::wstring chain = std::wstring(kDmftClsidString) + L'\0';
        ok = WriteString(key, L"CameraDeviceMftClsid", kDmftClsidString) &&
             RegSetValueExW(key, L"CameraDeviceMftCLSIDChain", 0, REG_MULTI_SZ,
                 reinterpret_cast<const BYTE*>(chain.c_str()),
                 static_cast<DWORD>((chain.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
        if (!name.empty() && saved.empty() && name != kCameraName)
            ok = WriteString(key, L"PS5CameraOriginalName", name) && ok;
        if (!name.empty()) ok = WriteString(key, L"FriendlyName", kCameraName) && ok;
    } else {
        ok = DeleteValue(key, L"CameraDeviceMftClsid") && DeleteValue(key, L"CameraDeviceMftCLSIDChain");
        if (!saved.empty()) ok = WriteString(key, L"FriendlyName", saved) && DeleteValue(key, L"PS5CameraOriginalName") && ok;
    }
    RegCloseKey(key);
    return ok;
}

}  // namespace

bool CameraNeedsRestart()
{
    std::wstring id = FindPhysicalCameraInstance();
    return !id.empty() && NeedsRestart(id);
}

bool CycleCamera()
{
    return CycleCameraPort();
}

bool StopFrameServer()
{
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    StopService(scm, L"FrameServerMonitor");
    bool ok = StopService(scm, L"FrameServer");
    CloseServiceHandle(scm);
    return ok;
}

HRESULT SetDeviceMft(bool on, std::wstring& message, bool restartFrameServer)
{
    std::vector<std::wstring> links = CameraInterfaces(false);
    if (links.empty()) {
        message = L"no camera interfaces found (plug the camera in once)";
        return HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED);
    }
    const bool wasOn = IsDeviceMftSet();
    // Every interface gets its turn even if one fails (an old phantom one may be broken); only the
    // plugged-in camera's must succeed, or it would stay half switched.
    std::vector<std::wstring> present = CameraInterfaces(true);
    int done = 0, failed = 0;
    bool presentFailed = false;
    for (const std::wstring& link : links) {
        if (ApplyDeviceMft(link, on)) {
            ++done;
            continue;
        }
        ++failed;
        for (const std::wstring& p : present)
            if (_wcsicmp(p.c_str(), link.c_str()) == 0) presentFailed = true;
    }
    message = std::wstring(on ? L"device MFT set on " : L"device MFT removed from ") + std::to_wstring(done) +
              L" camera interfaces";
    if (failed) message += L", " + std::to_wstring(failed) + L" failed (run as administrator?)";
    // Frame Server reads the values whenever it builds the camera's pipeline; stopping it (it starts
    // again on demand) drops pipelines it keeps cached. No device restart: one that is refused while
    // something holds the camera leaves Frame Server answering MF_E_REBOOT_REQUIRED until a replug.
    if (restartFrameServer && wasOn != on)
        message += StopFrameServer() ? L", Frame Server restarted" : L", Frame Server not restarted";
    if (presentFailed || (failed && !done)) return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
    return S_OK;
}

bool IsDeviceMftSet()
{
    // On the plugged-in camera: the device MFT named on every interface, and the name applied where
    // there is one (a reinstalled usbvideo.sys rewrites both).
    std::vector<std::wstring> links = CameraInterfaces(true);
    if (links.empty()) return false;
    for (const std::wstring& link : links) {
        HKEY key = nullptr;
        if (CM_Open_Device_Interface_KeyW(link.c_str(), KEY_READ, RegDisposition_OpenExisting, &key, 0) != CR_SUCCESS)
            return false;
        const std::wstring clsid = ReadString(key, L"CameraDeviceMftClsid");
        const std::wstring name = ReadString(key, L"FriendlyName");
        RegCloseKey(key);
        if (_wcsicmp(clsid.c_str(), kDmftClsidString) != 0 || (!name.empty() && name != kCameraName)) return false;
    }
    return true;
}

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
        // Only a real change needs the device restart (a refused one would leave Frame Server
        // asking for a reboot), so look first.
        DWORD v = 0, size = sizeof(v);
        const bool wasHidden = RegQueryValueExW(key, L"SkipCameraEnumeration", nullptr, nullptr,
                                   reinterpret_cast<BYTE*>(&v), &size) == ERROR_SUCCESS && v == 1;
        if (hidden) {
            DWORD one = 1;
            RegSetValueExW(key, L"SensorCameraMode", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one), sizeof(one));
            RegSetValueExW(key, L"SkipCameraEnumeration", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one), sizeof(one));
        } else {
            RegDeleteValueW(key, L"SensorCameraMode");
            RegDeleteValueW(key, L"SkipCameraEnumeration");
        }
        RegCloseKey(key);
        if (_wcsicmp(id.c_str(), present.c_str()) == 0 && wasHidden == hidden) {
            message = hidden ? L"already hidden" : L"already shown";
        } else if (_wcsicmp(id.c_str(), present.c_str()) == 0) {
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