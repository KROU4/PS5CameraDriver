#include "log.h"

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace ps5cam {

namespace {
std::mutex g_mutex;
std::wstring g_path;
bool g_echo = false;
constexpr long long kMaxLogBytes = 2 * 1024 * 1024;

// A subfolder only SYSTEM and administrators can write (the service runs as SYSTEM and must not
// rotate files in a folder that less privileged accounts control). Returns false when the folder
// cannot be secured (e.g. it is owned by someone else and we may not take it over).
bool MakePrivateDir(const std::wstring& dir)
{
    DWORD attr = GetFileAttributesW(dir.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT)) RemoveDirectoryW(dir.c_str());
    CreateDirectoryW(dir.c_str(), nullptr);
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)", SDDL_REVISION_1, &sd, nullptr))
        return false;
    BOOL present = FALSE, defaulted = FALSE;
    PACL dacl = nullptr;
    PSID owner = nullptr;
    bool ok = GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted) && present &&
              GetSecurityDescriptorOwner(sd, &owner, &defaulted) &&
              SetNamedSecurityInfoW(const_cast<wchar_t*>(dir.c_str()), SE_FILE_OBJECT,
                  OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, owner,
                  nullptr, dacl, nullptr) == ERROR_SUCCESS;
    LocalFree(sd);
    attr = GetFileAttributesW(dir.c_str());
    return ok && attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_REPARSE_POINT);
}
void RotateIfLarge()
{
    WIN32_FILE_ATTRIBUTE_DATA info = {};
    if (GetFileAttributesExW(g_path.c_str(), GetFileExInfoStandard, &info) &&
        ((static_cast<long long>(info.nFileSizeHigh) << 32) | info.nFileSizeLow) > kMaxLogBytes)
        MoveFileExW(g_path.c_str(), (g_path + L".old").c_str(), MOVEFILE_REPLACE_EXISTING);
}
}  // namespace

void LogInit(const wchar_t* name, bool echoConsole)
{
    wchar_t* programData = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &programData))) return;
    std::wstring dir = std::wstring(programData) + L"\\PS5Camera";
    CoTaskMemFree(programData);
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring file = name;
    size_t slash = file.find_last_of(L'\\');
    bool usable = true;
    if (slash != std::wstring::npos) usable = MakePrivateDir(dir + L"\\" + file.substr(0, slash));
    std::lock_guard lock(g_mutex);
    if (!usable) return;  // never write into a folder we could not secure
    g_echo = echoConsole;
    g_path = dir + L"\\" + file + L".log";
    RotateIfLarge();
}

void Log(const wchar_t* fmt, ...)
{
    wchar_t msg[1024];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(msg, _TRUNCATE, fmt, args);
    va_end(args);
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t line[1200];
    _snwprintf_s(line, _TRUNCATE, L"%04u-%02u-%02u %02u:%02u:%02u.%03u [%lu] %ls\n", t.wYear, t.wMonth, t.wDay, t.wHour,
        t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentThreadId(), msg);
    std::lock_guard lock(g_mutex);
    if (!g_path.empty()) {
        static unsigned counter = 0;
        if (++counter % 256 == 0) RotateIfLarge();
        FILE* f = nullptr;
        if (_wfopen_s(&f, g_path.c_str(), L"a, ccs=UTF-8") == 0 && f) {
            fputws(line, f);
            fclose(f);
        }
    }
    if (g_echo) fputws(line, stderr);
}

}  // namespace ps5cam