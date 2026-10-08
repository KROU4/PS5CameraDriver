// ps5cam-ctl: administrative command line for the PS5 Camera driver package.
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>

#include <cstdio>
#include <string>

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
int wmain(int argc, wchar_t** argv)
{
    std::wstring cmd = argc > 1 ? argv[1] : L"";
    std::wstring msg;
    if (cmd == L"setup") return Setup();
    if (cmd == L"register") return Report(RegisterVirtualCamera(msg), msg);
    if (cmd == L"remove") return Report(RemoveVirtualCamera(msg), msg);
    if (cmd == L"hide") return Report(SetPhysicalCameraHidden(true, msg), msg);
    if (cmd == L"unhide") return Report(SetPhysicalCameraHidden(false, msg), msg);
    if (cmd == L"defaults") return SaveSettings(Settings{}) ? 0 : 1;
    if (cmd == L"set" && argc == 4) {
        Settings s = LoadSettings();
        std::wstring k = argv[2];
        uint32_t v = static_cast<uint32_t>(_wtoi(argv[3]));
        if (k == L"mode") s.mode = v;
        else if (k == L"blur") s.blur = v;
        else if (k == L"autofocus") s.autoFocus = v != 0;
        else if (k == L"focus") s.focus = v;
        else if (k == L"prefer60") s.prefer60 = v != 0;
        else if (k == L"highlights") s.highlights = v;
        else if (k == L"temporal") s.temporal = v;
        else if (k == L"autobrightness") s.autoBrightness = v != 0;
        else if (k == L"maxgain") s.maxGain = v;
        else {
            wprintf(L"unknown setting\n");
            return 1;
        }
        return SaveSettings(s) ? 0 : 1;
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
        wprintf(L"settings        : mode %u blur %u autofocus %u focus %u prefer60 %u highlights %u temporal %u\n",
            s.mode, s.blur, s.autoFocus, s.focus, s.prefer60, s.highlights, s.temporal);
        wprintf(L"stream          : %ls %ls fps %.2f gpu %.2f ms focus %.2f %ls\n", st.streaming ? L"active" : L"idle",
            st.format.c_str(), st.fpsX100 / 100.0, st.gpuUs / 1000.0, st.focusX100 / 100.0, st.error.c_str());
        return 0;
    }
    wprintf(L"usage: ps5cam-ctl setup | register | remove | hide | unhide | status | recalibrate | defaults |\n"
            L"                  set mode|blur|autofocus|focus|prefer60|highlights|temporal|autobrightness|maxgain VALUE\n");
    return 1;
}
