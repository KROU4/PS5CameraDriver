// ps5cam-svc: Windows service that makes the PS5 HD Camera plug and play.
//  * The camera powers up in OV580 ROM boot mode (05A9:0580, bound to WinUSB); the service uploads
//    firmware.bin from its own directory, after which the camera re-enumerates as UVC 05A9:058C.
//  * When the running camera appears it hides the raw stereo device from apps (optional) and keeps
//    the \"PS5 Camera\" virtual camera registered and associated with it.
#include <windows.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <usbiodef.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include "../common/firmware.h"
#include "../common/ids.h"
#include "../common/log.h"
#include "../common/vcamreg.h"

using namespace ps5cam;

namespace {

constexpr wchar_t kServiceName[] = L"PS5CameraService";
constexpr wchar_t kDisplayName[] = L"PS5 HD Camera Service";

SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
SERVICE_STATUS g_status = {SERVICE_WIN32_OWN_PROCESS, SERVICE_START_PENDING, 0, NO_ERROR, 0, 0, 0};
HANDLE g_stopEvent = nullptr;

std::mutex g_queueMutex;
std::condition_variable g_queueCv;
enum class JobKind { Boot, CameraReady, Register };
struct Job {
    JobKind kind;
    std::wstring path;
};
std::deque<Job> g_queue;
std::atomic<bool> g_stopping = false;

std::wstring ModuleDir()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s = path;
    return s.substr(0, s.find_last_of(L'\\'));
}

void Enqueue(JobKind kind, const std::wstring& path = {})
{
    {
        std::lock_guard lock(g_queueMutex);
        for (const auto& j : g_queue)
            if (j.kind == kind && _wcsicmp(j.path.c_str(), path.c_str()) == 0) return;
        g_queue.push_back({kind, path});
    }
    g_queueCv.notify_one();
}

DWORD ReadRegDword(const wchar_t* name, DWORD fallback)
{
    DWORD v = fallback, size = sizeof(v);
    RegGetValueW(HKEY_LOCAL_MACHINE, kRegServiceKey, name, RRF_RT_REG_DWORD, nullptr, &v, &size);
    return v;
}

std::wstring ReadRegString(const wchar_t* name)
{
    wchar_t buf[512] = {};
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, kRegServiceKey, name, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return {};
    return buf;
}

void WriteRegString(const wchar_t* name, const std::wstring& v)
{
    RegSetKeyValueW(HKEY_LOCAL_MACHINE, kRegServiceKey, name, REG_SZ, v.c_str(), static_cast<DWORD>((v.size() + 1) * sizeof(wchar_t)));
}

std::wstring WaitForCameraLink(DWORD timeoutMs)
{
    for (DWORD waited = 0; !g_stopping; waited += 250) {
        std::wstring link = FindPhysicalCameraLink();
        if (!link.empty() || waited >= timeoutMs) return link;
        Sleep(250);
    }
    return {};
}

// The running camera is present: apply the raw-camera visibility preference and make sure the
// virtual camera exists and is associated with this camera's current interface.
void CameraReady()
{
    std::wstring link = WaitForCameraLink(10000);
    if (link.empty()) {
        Log(L"camera interface did not appear");
        return;
    }
    std::wstring msg;
    bool wantHidden = ReadRegDword(L"HideRawCamera", 1) != 0;
    if (wantHidden != IsPhysicalCameraHidden()) {
        SetPhysicalCameraHidden(wantHidden, msg);
        Log(L"raw camera: %ls", msg.c_str());
        Sleep(1500);  // the device restarts and re-registers its interfaces
        link = WaitForCameraLink(10000);
    }
    std::wstring stored = ReadRegString(L"AssociatedLink");
    if (!link.empty() && _wcsicmp(stored.c_str(), link.c_str()) != 0) {
        // First run, other USB port or visibility change: rebuild the association.
        RemoveVirtualCamera(msg);
        Log(L"virtual camera removed for re-association: %ls", msg.c_str());
    }
    HRESULT hr = RegisterVirtualCamera(msg);
    Log(L"virtual camera: %ls", msg.c_str());
    if (SUCCEEDED(hr) && !link.empty()) WriteRegString(L"AssociatedLink", link);
}

std::wstring g_firmwareOverride;  // `load FILE` uses a specific image (firmware experiments)

void BootDevice(const std::wstring& path)
{
    std::vector<uint8_t> image;
    std::wstring error;
    std::wstring fw = g_firmwareOverride.empty() ? ModuleDir() + L"\\firmware.bin" : g_firmwareOverride;
    if (!LoadFirmwareFile(fw, image, error)) {
        Log(L"firmware: %ls", error.c_str());
        return;
    }
    for (int attempt = 1; attempt <= 5 && !g_stopping; ++attempt) {
        DWORD t0 = GetTickCount();
        if (UploadFirmware(path, image, error)) {
            Log(L"firmware uploaded (%zu bytes, %lu ms) to %ls", image.size(), GetTickCount() - t0, path.c_str());
            return;
        }
        Log(L"upload attempt %d failed: %ls", attempt, error.c_str());
        // The interface can arrive before WinUSB is ready to be opened; back off and retry.
        Sleep(400 * attempt);
        if (FindBootDevices().empty()) {
            Log(L"boot device disappeared, giving up");
            return;
        }
    }
}

void Worker()
{
    while (!g_stopping) {
        Job job;
        {
            std::unique_lock lock(g_queueMutex);
            g_queueCv.wait(lock, [] { return g_stopping || !g_queue.empty(); });
            if (g_stopping) return;
            job = g_queue.front();
            g_queue.pop_front();
        }
        if (job.kind == JobKind::Boot) {
            BootDevice(job.path);
        } else if (job.kind == JobKind::CameraReady) {
            CameraReady();
        } else {
            std::wstring msg;
            RegisterVirtualCamera(msg);
            Log(L"virtual camera: %ls", msg.c_str());
        }
    }
}

bool IsRunningCameraPath(std::wstring s)
{
    for (auto& ch : s) ch = static_cast<wchar_t>(towlower(ch));
    return s.find(L"vid_05a9&pid_058c") != std::wstring::npos;
}

DWORD CALLBACK OnDeviceChange(HCMNOTIFICATION, PVOID, CM_NOTIFY_ACTION action, PCM_NOTIFY_EVENT_DATA data, DWORD)
{
    if (action == CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL) {
        std::wstring link = data->u.DeviceInterface.SymbolicLink;
        if (IsBootDevicePath(link)) {
            Log(L"boot-mode camera arrived: %ls", link.c_str());
            Enqueue(JobKind::Boot, link);
        } else if (IsRunningCameraPath(link)) {
            Log(L"camera running: %ls", link.c_str());
            Enqueue(JobKind::CameraReady);
        }
    }
    return ERROR_SUCCESS;
}

// Runs until g_stopEvent is signalled. Shared by service and console modes.
int RunLoop()
{
    Log(L"started, firmware dir %ls", ModuleDir().c_str());
    std::thread worker(Worker);

    CM_NOTIFY_FILTER filter = {};
    filter.cbSize = sizeof(filter);
    filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
    filter.u.DeviceInterface.ClassGuid = GUID_DEVINTERFACE_USB_DEVICE;
    HCMNOTIFICATION notify = nullptr;
    CONFIGRET cr = CM_Register_Notification(&filter, nullptr, OnDeviceChange, &notify);
    if (cr != CR_SUCCESS) Log(L"CM_Register_Notification failed: %lu", cr);

    for (const auto& p : FindBootDevices()) {
        Log(L"boot-mode camera present at start: %ls", p.c_str());
        Enqueue(JobKind::Boot, p);
    }
    // Re-assert the virtual camera at every start (covers reboots), with the camera if it is running.
    Enqueue(FindPhysicalCameraInstance().empty() ? JobKind::Register : JobKind::CameraReady);

    WaitForSingleObject(g_stopEvent, INFINITE);
    g_stopping = true;
    g_queueCv.notify_all();
    if (notify) CM_Unregister_Notification(notify);
    worker.join();
    Log(L"stopped");
    return 0;
}

std::mutex g_statusMutex;  // SetState runs on the main thread and on the control-handler thread

void SetState(DWORD state)
{
    std::lock_guard lock(g_statusMutex);
    g_status.dwCurrentState = state;
    g_status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    // Stopping can wait for a device restart (pnputil, up to 30 s).
    g_status.dwWaitHint = (state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING) ? 45000 : 0;
    if (g_status.dwWaitHint) ++g_status.dwCheckPoint;
    SetServiceStatus(g_statusHandle, &g_status);
}

DWORD WINAPI ServiceCtrl(DWORD control, DWORD, LPVOID, LPVOID)
{
    if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
        SetState(SERVICE_STOP_PENDING);
        SetEvent(g_stopEvent);
    }
    return NO_ERROR;
}

void WINAPI ServiceMain(DWORD, LPWSTR*)
{
    g_statusHandle = RegisterServiceCtrlHandlerExW(kServiceName, ServiceCtrl, nullptr);
    if (!g_statusHandle) return;
    SetState(SERVICE_START_PENDING);
    SetState(SERVICE_RUNNING);
    RunLoop();
    SetState(SERVICE_STOPPED);
}

int Install()
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\"";
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!scm) {
        fwprintf(stderr, L"OpenSCManager failed %lu (run elevated)\n", GetLastError());
        return 1;
    }
    SC_HANDLE svc = CreateServiceW(scm, kServiceName, kDisplayName, SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, cmd.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);
    if (!svc && GetLastError() == ERROR_SERVICE_EXISTS) {
        svc = OpenServiceW(scm, kServiceName, SERVICE_ALL_ACCESS);
        ChangeServiceConfigW(svc, SERVICE_NO_CHANGE, SERVICE_AUTO_START, SERVICE_NO_CHANGE, cmd.c_str(), nullptr,
            nullptr, nullptr, nullptr, nullptr, kDisplayName);
    }
    if (!svc) {
        fwprintf(stderr, L"CreateService failed %lu\n", GetLastError());
        CloseServiceHandle(scm);
        return 1;
    }
    SERVICE_DESCRIPTIONW desc = {const_cast<wchar_t*>(L"Uploads firmware to the PlayStation 5 HD Camera when it is connected.")};
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &desc);
    SC_ACTION actions[3] = {{SC_ACTION_RESTART, 2000}, {SC_ACTION_RESTART, 5000}, {SC_ACTION_NONE, 0}};
    SERVICE_FAILURE_ACTIONSW fa = {86400, nullptr, nullptr, 3, actions};
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &fa);
    if (!StartServiceW(svc, 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING)
        fwprintf(stderr, L"StartService failed %lu\n", GetLastError());
    wprintf(L"service %ls installed and started\n", kServiceName);
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return 0;
}

int Uninstall()
{
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    SC_HANDLE svc = scm ? OpenServiceW(scm, kServiceName, SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS) : nullptr;
    if (!svc) {
        fwprintf(stderr, L"service not found (%lu)\n", GetLastError());
        if (scm) CloseServiceHandle(scm);
        return 1;
    }
    SERVICE_STATUS st = {};
    ControlService(svc, SERVICE_CONTROL_STOP, &st);
    for (int i = 0; i < 50 && QueryServiceStatus(svc, &st) && st.dwCurrentState != SERVICE_STOPPED; ++i) Sleep(100);
    BOOL ok = DeleteService(svc);
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    wprintf(ok ? L"service removed\n" : L"DeleteService failed\n");
    return ok ? 0 : 1;
}

BOOL WINAPI ConsoleCtrl(DWORD)
{
    SetEvent(g_stopEvent);
    return TRUE;
}

}  // namespace

int wmain(int argc, wchar_t** argv)
{
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::wstring cmd = argc > 1 ? argv[1] : L"";
    if (cmd == L"install") return Install();
    if (cmd == L"uninstall") return Uninstall();
    if (cmd == L"load") {  // one-shot upload, for diagnostics: load [FIRMWARE] [WAIT_SECONDS]
        LogInit(L"service\\service", true);
        if (argc > 2) g_firmwareOverride = argv[2];
        int waitSeconds = argc > 3 ? _wtoi(argv[3]) : 0;
        auto devices = FindBootDevices();
        for (int i = 0; devices.empty() && i < waitSeconds * 4; ++i) {
            Sleep(250);
            devices = FindBootDevices();
        }
        if (devices.empty()) {
            wprintf(L"no boot-mode camera found\n");
            return 2;
        }
        for (const auto& dev : devices) BootDevice(dev);
        return 0;
    }
    if (cmd == L"console") {
        LogInit(L"service\\service", true);
        SetConsoleCtrlHandler(ConsoleCtrl, TRUE);
        return RunLoop();
    }
    LogInit(L"service\\service");
    SERVICE_TABLE_ENTRYW table[] = {{const_cast<wchar_t*>(kServiceName), ServiceMain}, {nullptr, nullptr}};
    if (!StartServiceCtrlDispatcherW(table)) {
        wprintf(L"usage: ps5cam-svc install | uninstall | console | load\n");
        return 1;
    }
    return 0;
}
