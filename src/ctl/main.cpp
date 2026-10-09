// ps5cam-ctl: administrative command line for the PS5 Camera driver package.
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>

#include <cstdio>
#include <string>

#include "../common/camdefaults.h"
#include "../common/ids.h"
#include "../common/settings.h"
#include "../common/vcamreg.h"

using namespace ps5cam;

static int Report(HRESULT hr, const std::wstring& msg)
{
    wprintf(L"%ls\n", msg.c_str());
    return SUCCEEDED(hr) ? 0 : 1;
}

// Creates HKLM\SOFTWARE\PS5Camera and %ProgramData%\PS5Camera with protected ACLs and an
// Administrators owner (a pre-created object owned by a standard user is taken over):
//  - registry root: Users and LOCAL SERVICE may read, set values and create subkeys (tray settings,
//    status written by the media source), but not change permissions or delete the key;
//  - registry Service subkey: values that make the SYSTEM service act on devices, admins only;
//  - log folder: LOCAL SERVICE (Frame Server) may write, Users only read.
// SetNamedSecurityInfo propagates the inheritable entries to existing subkeys and files.
static void EnablePrivilege(const wchar_t* name)
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) return;
    TOKEN_PRIVILEGES tp = {1};
    if (LookupPrivilegeValueW(nullptr, name, &tp.Privileges[0].Luid)) {
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    }
    CloseHandle(token);
}

static bool ApplySecurity(const wchar_t* object, SE_OBJECT_TYPE type, const wchar_t* sddl)
{
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, nullptr)) return false;
    BOOL present = FALSE, defaulted = FALSE;
    PACL dacl = nullptr;
    PSID owner = nullptr;
    bool ok = GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted) && present &&
              GetSecurityDescriptorOwner(sd, &owner, &defaulted) && owner &&
              SetNamedSecurityInfoW(const_cast<wchar_t*>(object), type,
                  OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, owner,
                  nullptr, dacl, nullptr) == ERROR_SUCCESS;
    LocalFree(sd);
    return ok;
}

static int Setup()
{
    EnablePrivilege(SE_TAKE_OWNERSHIP_NAME);
    EnablePrivilege(SE_RESTORE_NAME);
    HKEY key = nullptr;
    LSTATUS st = RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\PS5Camera\\Service", 0, nullptr, 0, KEY_READ, nullptr,
        &key, nullptr);
    if (st == ERROR_SUCCESS) RegCloseKey(key);
    bool regOk = st == ERROR_SUCCESS &&
                 ApplySecurity(L"MACHINE\\SOFTWARE\\PS5Camera", SE_REGISTRY_KEY,
                     L"O:BAD:P(A;CI;KA;;;SY)(A;CI;KA;;;BA)(A;CI;0x2001f;;;BU)(A;CI;0x2001f;;;LS)") &&
                 ApplySecurity(L"MACHINE\\SOFTWARE\\PS5Camera\\Service", SE_REGISTRY_KEY,
                     L"O:BAD:P(A;CI;KA;;;SY)(A;CI;KA;;;BA)(A;CI;KR;;;BU)(A;CI;KR;;;LS)");
    wprintf(L"registry key: %ls\n", regOk ? L"ok" : L"failed");
    // Run by the installer as the installing user: their country decides the mains frequency
    // (anti-flicker), which Frame Server cannot see. (From the MSI it runs as SYSTEM, whose region is
    // the one Windows was set up with; the tray corrects it at sign-in.)
    if (regOk) WriteSetting(L"MainsHz", RegionMainsHz());

    wchar_t dir[MAX_PATH];
    ExpandEnvironmentStringsW(L"%ProgramData%\\PS5Camera", dir, MAX_PATH);
    DWORD attr = GetFileAttributesW(dir);
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT)) RemoveDirectoryW(dir);  // no junctions
    CreateDirectoryW(dir, nullptr);
    bool dirOk = ApplySecurity(dir, SE_FILE_OBJECT,
        L"O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1301bf;;;LS)(A;OICI;0x1200a9;;;BU)");
    wprintf(L"log folder  : %ls\n", dirOk ? L"ok" : L"failed");
    return regOk && dirOk ? 0 : 1;
}
// Restarts PS5CameraService if it runs: at start it sets up the camera the way Service\UseDeviceMft
// says (virtual camera and hiding, or the device MFT).
static void RestartCameraService()
{
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return;
    SC_HANDLE svc = OpenServiceW(scm, L"PS5CameraService", SERVICE_STOP | SERVICE_START | SERVICE_QUERY_STATUS);
    SERVICE_STATUS st = {};
    if (svc && QueryServiceStatus(svc, &st) && st.dwCurrentState == SERVICE_RUNNING &&
        ControlService(svc, SERVICE_CONTROL_STOP, &st)) {
        for (int i = 0; i < 100 && QueryServiceStatus(svc, &st) && st.dwCurrentState != SERVICE_STOPPED; ++i) Sleep(100);
        wprintf(L"camera service restart: %ls\n", StartServiceW(svc, 0, nullptr) ? L"ok" : L"failed");
    }
    if (svc) CloseServiceHandle(svc);
    CloseServiceHandle(scm);
}

int wmain(int argc, wchar_t** argv)
{
    std::wstring cmd = argc > 1 ? argv[1] : L"";
    std::wstring msg;
    if (cmd == L"setup") return Setup();
    if (cmd == L"register") return Report(RegisterVirtualCamera(msg), msg);
    if (cmd == L"remove") {
        // Both virtual cameras (the uninstaller runs this); the depth camera may not exist.
        std::wstring depthMsg;
        SetDepthCamera(false, depthMsg);
        wprintf(L"depth camera: %ls\n", depthMsg.c_str());
        return Report(RemoveVirtualCamera(msg), msg);
    }
    if (cmd == L"hide") return Report(SetPhysicalCameraHidden(true, msg), msg);
    if (cmd == L"unhide") return Report(SetPhysicalCameraHidden(false, msg), msg);
    if (cmd == L"dmft" && argc == 3 && (wcscmp(argv[2], L"on") == 0 || wcscmp(argv[2], L"off") == 0)) {
        // The service follows the choice on every plug-in: on keeps the camera visible with the
        // device MFT and removes the virtual camera; off goes back to the virtual camera.
        const DWORD on = wcscmp(argv[2], L"on") == 0;
        if (RegSetKeyValueW(HKEY_LOCAL_MACHINE, kRegServiceKey, L"UseDeviceMft", REG_DWORD, &on, sizeof(on)) !=
            ERROR_SUCCESS) {
            wprintf(L"failed: run as administrator\n");
            return 1;
        }
        bool frameServerStopped = false;
        if (on) {
            RemoveVirtualCamera(msg);
            if (IsPhysicalCameraHidden()) {
                // Let go of the hidden camera first, or its restart may be refused (and Frame Server
                // then wants a reboot).
                frameServerStopped = StopFrameServer();
                SetPhysicalCameraHidden(false, msg);
                wprintf(L"%ls\n", msg.c_str());
            }
        }
        HRESULT hr = SetDeviceMft(on != 0, msg, !frameServerStopped);
        int code = Report(hr, msg);
        if (!on) RestartCameraService();  // it brings the virtual camera back right away
        return code;
    }
    if (cmd == L"defaults") {
        Settings s;
        s.mainsHz = LoadSettings().mainsHz;  // a fact about the user, not a preference
        return SaveSettings(s) ? 0 : 1;
    }
    if (cmd == L"set" && argc == 4) {
        Settings s = LoadSettings();
        std::wstring k = argv[2];
        uint32_t v = static_cast<uint32_t>(_wtoi(argv[3]));
        if (k == L"mode" && v > 4) {
            wprintf(L"mode: 0 bokeh, 1 main sensor, 2 second sensor, 3 depth, 4 side by side\n");
            return 1;
        }
        if (k == L"mode") s.mode = v;
        else if (k == L"blur") s.blur = v, s.blurStyle = kBlurPortrait;  // a chosen strength, not "Standard blur"
        else if (k == L"autofocus") s.autoFocus = v != 0;
        else if (k == L"focus") s.focus = v;
        else if (k == L"prefer60") s.prefer60 = v != 0;
        else if (k == L"fullhdonly") s.fullHdOnly = v != 0;
        else if (k == L"highlights") s.highlights = v;
        else if (k == L"temporal") s.temporal = v;
        else if (k == L"autobrightness") s.autoBrightness = v != 0;
        else if (k == L"maxgain") s.maxGain = v;
        else if (k == L"denoise") s.denoise = v;
        else if (k == L"sharpen") s.sharpen = v;
        else if (k == L"antiflicker" && v <= 3) s.antiFlicker = v;
        else if (k == L"blurstyle" && v <= 1) s.blurStyle = v;
        else if (k == L"depthcamera") s.depthCamera = v != 0;
        else if (k == L"depthview" && v <= 1) s.depthView = v;
        else {
            wprintf(L"unknown setting\n");
            return 1;
        }
        return SaveSettings(s) ? 0 : 1;
    }
    if (cmd == L"record" && argc == 3) {
        // Raw camera frames for tuning; the source writes them while an app uses the camera.
        EnablePrivilege(SE_TAKE_OWNERSHIP_NAME);
        EnablePrivilege(SE_RESTORE_NAME);
        if (!RequestRecording(static_cast<uint32_t>(_wtoi(argv[2])))) {
            wprintf(L"failed: run as administrator\n");
            return 1;
        }
        wprintf(L"the next frames go to %%ProgramData%%\\PS5Camera\\record-<sensor mode>.raw, e.g. record-1080h.raw\n"
                L"(written while an app uses the camera; readable by administrators only)\n");
        return 0;
    }
    if (cmd == L"effectsync") {
        // Run by the service as the signed-in administrator (Windows keeps the default per user and
        // lets only administrators save it): 0 updated, 3 nothing to update, 2 no camera, 1 failed.
        std::wstring result;
        const HRESULT hr = SyncBackgroundEffectDefault(LoadSettings(), &result);
        wprintf(L"Windows background effects default: %ls (0x%08lX), now %ls\n", result.c_str(), static_cast<unsigned long>(hr),
            DescribeBackgroundEffectDefault().c_str());
        return hr == HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED) ? 2 : FAILED(hr) ? 1 : hr == S_FALSE ? 3 : 0;
    }
    if (cmd == L"recalibrate") {
        BumpCalibrationRequest();
        return 0;
    }
    if (cmd == L"status") {
        Settings s = LoadSettings();
        Status st = LoadStatus();
        std::wstring link = FindPhysicalCameraLink();
        wprintf(L"physical camera : %ls\n", link.empty() ? L"not connected" : link.c_str());
        wprintf(L"raw camera hidden: %ls\n", IsPhysicalCameraHidden() ? L"yes" : L"no");
        wprintf(L"device MFT      : %ls\n", IsDeviceMftSet() ? L"on" : L"off");
        wprintf(L"Windows default : background effects %ls\n", DescribeBackgroundEffectDefault().c_str());
        wprintf(L"settings        : mode %u blur %u autofocus %u focus %u prefer60 %u fullhdonly %u highlights %u "
                L"temporal %u autobrightness %u maxgain %u denoise %u sharpen %u antiflicker %u blurstyle %u "
                L"mains %u Hz depthcamera %u depthview %u\n",
            s.mode, s.blur, s.autoFocus, s.focus, s.prefer60, s.fullHdOnly, s.highlights, s.temporal, s.autoBrightness,
            s.maxGain, s.denoise, s.sharpen, s.antiFlicker, s.blurStyle, s.mainsHz ? s.mainsHz : RegionMainsHz(),
            s.depthCamera, s.depthView);
        wprintf(L"stream          : %ls %ls fps %.2f gpu %.2f ms focus %.2f %ls\n", st.streaming ? L"active" : L"idle",
            st.format.c_str(), st.fpsX100 / 100.0, st.gpuUs / 1000.0, st.focusX100 / 100.0, st.error.c_str());
        // Stereo alignment per sensor mode; none until a calibration succeeded (the effect then runs
        // with no correction, or 1080h with 1080's inverted, and keeps trying while the camera is in use).
        wprintf(L"calibration     :");
        for (const wchar_t* sensor : {L"1080h", L"1080", L"800"}) {
            StoredCalibration c = LoadCalibration(sensor);
            if (c.valid) wprintf(L" %ls dy %.2f roll %.2f;", sensor, c.dy, c.rotation);
            else wprintf(L" %ls none;", sensor);
        }
        wprintf(L"%ls\n", LoadCalibrationRequest() != LoadCalibrationHandled() ? L" recalibration requested" : L"");
        return 0;
    }
    wprintf(L"usage: ps5cam-ctl setup | register | remove | hide | unhide | dmft on|off | status | recalibrate |\n"
            L"                  record N | defaults |\n"
            L"                  set mode|blur|autofocus|focus|prefer60|fullhdonly|highlights|temporal|autobrightness|\n"
            L"                      maxgain|denoise|sharpen|antiflicker|blurstyle|depthcamera|depthview VALUE\n"
            L"  fullhdonly 1: apps are offered 1920x1080 at 60 fps only; 0: also 1280x720 and 30 fps\n"
            L"  denoise 0..100: temporal noise reduction of the picture (0 off)\n"
            L"  sharpen 0..100: edge sharpening of the picture, leaving the noise alone (0 off)\n"
            L"  antiflicker 0 auto (dim scenes without lamp flicker get the longer exposure), 1 50 Hz, 2 60 Hz, 3 off\n"
            L"  blurstyle 0 portrait blur (the bokeh as set), 1 standard blur (strongest): Windows' background effects\n"
            L"  depthcamera 1: a \"PS5 Camera Depth\" camera shows the depth of what PS5 Camera streams (e.g. for OBS);\n"
            L"  depthview 0 depth (near = bright), 1 subject matte (white = the subject and what is in front of it)\n"
            L"  (formats apply when no app has the camera open)\n");
    return 1;
}
