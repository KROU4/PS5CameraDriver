// ps5cam-svc: Windows service that makes the PS5 HD Camera plug and play.
//  * The camera powers up in OV580 ROM boot mode (05A9:0580, bound to WinUSB); the service uploads
//    firmware.bin from its own directory, after which the camera re-enumerates as UVC 05A9:058C.
//  * When the running camera appears it hides the raw stereo device from apps (optional) and keeps
//    the \"PS5 Camera\" virtual camera registered and associated with it.
#include <windows.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <usbiodef.h>
#include <userenv.h>
#include <wtsapi32.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include "../common/camdefaults.h"
#include "../common/firmware.h"
#include "../common/ids.h"
#include "../common/log.h"
#include "../common/settings.h"
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
enum class JobKind { Boot, CameraReady, Register, DepthCamera };
struct Job {
    JobKind kind;
    std::wstring path;
};
std::deque<Job> g_queue;
std::atomic<bool> g_stopping = false;
HANDLE g_settingsEvent = nullptr;  // settings changed, or the camera arrived: check the effect default
std::atomic<bool> g_resyncEffects = false;  // somebody signed in: sync the effect default again

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

// Service\UseDeviceMft = 1: the effect runs in ps5cam-dmft.dll inside the camera itself, so the
// camera stays visible under its own name and there is no virtual camera.
bool UseDeviceMft()
{
    return ReadRegDword(L"UseDeviceMft", 0) != 0;
}

void DeviceMftReady()
{
    std::wstring msg;
    if (IsPhysicalCameraHidden()) {
        // Coming from the virtual camera: let go of the hidden camera before restarting it, or the
        // restart may be refused (and Frame Server then wants a reboot).
        RemoveVirtualCamera(msg);
        Log(L"virtual camera removed: %ls", msg.c_str());
        WriteRegString(L"AssociatedLink", L"");
        StopFrameServer();
        SetPhysicalCameraHidden(false, msg);
        Log(L"raw camera: %ls", msg.c_str());
        Sleep(1500);  // the device restarts and re-registers its interfaces
        WaitForCameraLink(10000);
    }
    // Another USB port gives the camera new interfaces without the device MFT (this check also
    // ends the arrival that our own device restart causes). No Frame Server restart: it has no
    // pipeline for interfaces that just appeared, and stopping it would cut every app's camera.
    if (!IsDeviceMftSet()) {
        SetDeviceMft(true, msg, false);
        Log(L"%ls", msg.c_str());
    }
    if (!ReadRegString(L"AssociatedLink").empty()) {
        RemoveVirtualCamera(msg);
        Log(L"virtual camera removed: %ls", msg.c_str());
        WriteRegString(L"AssociatedLink", L"");
    }
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
    if (CameraNeedsRestart()) {
        // Left waiting for a reboot by a refused device restart: re-enumerate it instead (its
        // arrival brings us back here).
        Log(L"camera waits for a reboot, re-enumerating it: %ls", CycleCamera() ? L"ok" : L"failed");
        return;
    }
    if (UseDeviceMft()) {
        DeviceMftReady();
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

// An installation without internet access leaves no firmware.bin (and a broken one is no better):
// build it with the installer's firmware.ps1 (Sony's original from the sources in
// ps5cam-firmware.json plus the driver's changes, both checked by hash), at most every 9 minutes
// (RunLoop looks every minute), waiting up to 3 minutes for the download. The script runs in a job
// that dies with the service, its output goes to service\firmware.log, and a service stop ends it.
bool BuildFirmware()
{
    static ULONGLONG lastTry = 0;  // the worker thread's (or `load`'s) only
    const ULONGLONG now = GetTickCount64();
    if (lastTry && now - lastTry < 9 * 60 * 1000) return false;
    lastTry = now;
    const std::wstring dir = ModuleDir();
    wchar_t system[MAX_PATH], logPath[MAX_PATH];
    if (!GetSystemDirectoryW(system, MAX_PATH) ||
        !ExpandEnvironmentStringsW(L"%ProgramData%\\PS5Camera\\service\\firmware.log", logPath, MAX_PATH))
        return false;
    std::wstring cmd = L"\"" + std::wstring(system) + L"\\WindowsPowerShell\\v1.0\\powershell.exe\" -NoProfile "
                       L"-NonInteractive -ExecutionPolicy Bypass -File \"" + dir + L"\\firmware.ps1\" -Patch \"" + dir +
                       L"\\ps5cam-firmware.json\" -Out \"" + dir + L"\\firmware.bin\"";
    SECURITY_ATTRIBUTES inherit = {sizeof(inherit), nullptr, TRUE};
    HANDLE log = CreateFileW(logPath, FILE_APPEND_DATA, FILE_SHARE_READ, &inherit, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW si = {sizeof(si)};
    if (log != INVALID_HANDLE_VALUE) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = si.hStdError = log;
    }
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }
    PROCESS_INFORMATION pi = {};
    const BOOL started = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, log != INVALID_HANDLE_VALUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, dir.c_str(), &si, &pi);
    const DWORD startError = GetLastError();
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    if (!started) {
        if (job) CloseHandle(job);
        Log(L"firmware: cannot run firmware.ps1 (%lu)", startError);
        return false;
    }
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    DWORD code = 1;
    HANDLE waits[] = {pi.hProcess, g_stopEvent};
    const DWORD waited = WaitForMultipleObjects(g_stopEvent ? 2 : 1, waits, FALSE, 3 * 60 * 1000);
    if (waited == WAIT_OBJECT_0) {
        GetExitCodeProcess(pi.hProcess, &code);
    } else {
        TerminateProcess(pi.hProcess, 1);
        if (waited == WAIT_TIMEOUT) Log(L"firmware: firmware.ps1 timed out");
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (job) CloseHandle(job);
    if (waited != WAIT_OBJECT_0) return false;
    if (code == 0) Log(L"firmware built from Sony's original");
    else if (code == 2) Log(L"firmware: Sony's original could not be downloaded (offline?), next try in 10 minutes");
    else Log(L"firmware: firmware.ps1 failed (%lu), see firmware.log", code);
    return code == 0;
}

void BootDevice(const std::wstring& path)
{
    std::vector<uint8_t> image;
    std::wstring error;
    std::wstring fw = g_firmwareOverride.empty() ? ModuleDir() + L"\\firmware.bin" : g_firmwareOverride;
    if (!LoadFirmwareFile(fw, image, error)) {
        Log(L"firmware: %ls", error.c_str());
        if (!g_firmwareOverride.empty() || !BuildFirmware() || !LoadFirmwareFile(fw, image, error)) return;
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
            SetEvent(g_settingsEvent);
        } else if (job.kind == JobKind::DepthCamera) {
            // The tray and ps5cam-ctl switch it through the settings; registering needs SYSTEM.
            const bool on = LoadSettings().depthCamera;
            std::wstring msg;
            SetDepthCamera(on, msg);
            Log(L"depth camera %ls: %ls", on ? L"on" : L"off", msg.c_str());
        } else if (!UseDeviceMft()) {
            std::wstring msg;
            RegisterVirtualCamera(msg);
            Log(L"virtual camera: %ls", msg.c_str());
        }
    }
}

// The signed-in user's administrator token as a primary token: the elevated half of their split
// token, or the token itself for an administrator without UAC. Null for a standard user or when
// nobody is signed in. Windows keeps the camera defaults per user and lets only administrators
// save them.
HANDLE UserAdminToken()
{
    const DWORD session = WTSGetActiveConsoleSessionId();
    HANDLE user = nullptr;
    if (session == 0xFFFFFFFF || !WTSQueryUserToken(session, &user)) return nullptr;
    HANDLE candidate = nullptr;
    TOKEN_ELEVATION_TYPE type = TokenElevationTypeDefault;
    DWORD len = 0;
    if (GetTokenInformation(user, TokenElevationType, &type, sizeof(type), &len)) {
        TOKEN_LINKED_TOKEN linked = {};
        if (type == TokenElevationTypeLimited) {
            if (GetTokenInformation(user, TokenLinkedToken, &linked, sizeof(linked), &len)) candidate = linked.LinkedToken;
        } else {
            DuplicateTokenEx(user, MAXIMUM_ALLOWED, nullptr, SecurityImpersonation, TokenPrimary, &candidate);
        }
    }
    CloseHandle(user);
    HANDLE primary = nullptr, check = nullptr;
    BOOL admin = FALSE;
    BYTE sid[SECURITY_MAX_SID_SIZE];
    DWORD sidSize = sizeof(sid);
    if (candidate && DuplicateTokenEx(candidate, MAXIMUM_ALLOWED, nullptr, SecurityImpersonation, TokenPrimary, &primary) &&
        DuplicateTokenEx(primary, TOKEN_QUERY, nullptr, SecurityIdentification, TokenImpersonation, &check) &&
        CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, sid, &sidSize))
        CheckTokenMembership(check, sid, &admin);
    if (check) CloseHandle(check);
    if (candidate) CloseHandle(candidate);
    if (!admin && primary) {
        CloseHandle(primary);
        primary = nullptr;
    }
    return primary;
}

// Runs "ps5cam-ctl effectsync" as that user (in a process of their own, so that it works with their
// profile); its exit code, or -1.
int RunEffectSync(HANDLE token)
{
    std::wstring cmd = L"\"" + ModuleDir() + L"\\ps5cam-ctl.exe\" effectsync";
    void* env = nullptr;
    if (!CreateEnvironmentBlock(&env, token, FALSE)) env = nullptr;
    STARTUPINFOW si = {sizeof(si)};
    wchar_t desktop[] = L"winsta0\\default";
    si.lpDesktop = desktop;
    PROCESS_INFORMATION pi = {};
    const BOOL started = CreateProcessAsUserW(token, nullptr, cmd.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, env, ModuleDir().c_str(), &si, &pi);
    if (env) DestroyEnvironmentBlock(env);
    if (!started) return -1;
    DWORD code = 1;
    if (WaitForSingleObject(pi.hProcess, 30000) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    else TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

// Windows keeps a default for the camera's "Background effects" once the user has set them in
// Settings, and writes it to the camera at every start. The service follows the bokeh switch of
// the tray and ps5cam-ctl into it, as the signed-in administrator (see camdefaults.h), and again
// when somebody signs in.
void EffectDefaultWatcher()
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kRegRoot, 0, nullptr, 0, KEY_READ | KEY_NOTIFY, nullptr, &key, nullptr) !=
        ERROR_SUCCESS) {
        Log(L"background effects default: cannot watch the settings");
        return;
    }
    uint64_t synced = ~0ULL;
    bool toldNoAdmin = false;
    int depthCamera = -1;  // the state last asked of the worker
    while (!g_stopping) {
        if (g_resyncEffects.exchange(false)) synced = ~0ULL;
        // Armed before reading, so a change while syncing wakes the next round; without the
        // notification, the settings are looked at every minute.
        const bool notified =
            RegNotifyChangeKeyValue(key, FALSE, REG_NOTIFY_CHANGE_LAST_SET, g_settingsEvent, TRUE) == ERROR_SUCCESS;
        const Settings settings = LoadSettings();
        if (int(settings.depthCamera) != depthCamera) {
            depthCamera = settings.depthCamera;
            Enqueue(JobKind::DepthCamera);
        }
        const uint64_t flags = BackgroundEffectFlags(settings);
        if (flags != synced) {
            if (HANDLE admin = UserAdminToken()) {
                const int code = RunEffectSync(admin);
                CloseHandle(admin);
                if (code == 0 || code == 3) synced = flags;  // 2: no camera; 1, -1: failed (retried on change)
                if (code != 2 && code != 3) Log(L"background effects default: ps5cam-ctl effectsync exit %d", code);
                toldNoAdmin = false;
            } else if (!toldNoAdmin) {
                Log(L"background effects default: no signed-in administrator to update it");
                toldNoAdmin = true;
            }
        }
        HANDLE waits[] = {g_settingsEvent, g_stopEvent};
        WaitForMultipleObjects(2, waits, FALSE, notified ? INFINITE : 60000);
    }
    RegCloseKey(key);
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
    g_settingsEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::thread worker(Worker);
    std::thread effects([] {
        try {
            if (g_settingsEvent) EffectDefaultWatcher();
        } catch (...) {
            Log(L"background effects default: the watcher failed");  // the camera itself is not affected
        }
    });

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

    // A camera left in boot mode because its firmware could not be built yet (an installation
    // without internet access) gets another try: looked at every minute, BuildFirmware spaces the
    // downloads out.
    while (WaitForSingleObject(g_stopEvent, 60 * 1000) == WAIT_TIMEOUT) {
        if (!g_firmwareOverride.empty()) continue;
        const auto boot = FindBootDevices();
        if (boot.empty()) continue;
        std::vector<uint8_t> image;
        std::wstring error;
        if (!LoadFirmwareFile(ModuleDir() + L"\\firmware.bin", image, error))
            for (const auto& p : boot) Enqueue(JobKind::Boot, p);
    }
    g_stopping = true;
    g_queueCv.notify_all();
    if (notify) CM_Unregister_Notification(notify);
    worker.join();
    effects.join();  // g_stopEvent is set: the watcher's wait returns
    Log(L"stopped");
    return 0;
}

std::mutex g_statusMutex;  // SetState runs on the main thread and on the control-handler thread

void SetState(DWORD state)
{
    std::lock_guard lock(g_statusMutex);
    g_status.dwCurrentState = state;
    g_status.dwControlsAccepted =
        state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_SESSIONCHANGE : 0;
    // Stopping can wait for a device restart (pnputil, up to 30 s).
    g_status.dwWaitHint = (state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING) ? 45000 : 0;
    if (g_status.dwWaitHint) ++g_status.dwCheckPoint;
    SetServiceStatus(g_statusHandle, &g_status);
}

DWORD WINAPI ServiceCtrl(DWORD control, DWORD type, LPVOID, LPVOID)
{
    if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
        SetState(SERVICE_STOP_PENDING);
        SetEvent(g_stopEvent);
    } else if (control == SERVICE_CONTROL_SESSIONCHANGE && type == WTS_SESSION_LOGON && g_settingsEvent) {
        g_resyncEffects = true;
        SetEvent(g_settingsEvent);
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
