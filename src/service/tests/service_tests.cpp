#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#define OpenSCManagerW TestOpenSCManagerW
#define CreateServiceW TestCreateServiceW
#define OpenServiceW TestOpenServiceW
#define ChangeServiceConfigW TestChangeServiceConfigW
#define ChangeServiceConfig2W TestChangeServiceConfig2W
#define StartServiceW TestStartServiceW
#define CloseServiceHandle TestCloseServiceHandle
#define Sleep TestSleep
#define LoadFirmwareFile TestLoadFirmwareFile
#define UploadFirmware TestUploadFirmware
#define FindBootDevices TestFindBootDevices
#define LogInit TestLogInit
#define Log TestLog
#define wmain ps5cam_service_main

extern "C" SC_HANDLE WINAPI TestOpenSCManagerW(LPCWSTR, LPCWSTR, DWORD);
extern "C" SC_HANDLE WINAPI TestCreateServiceW(SC_HANDLE, LPCWSTR, LPCWSTR, DWORD, DWORD, DWORD, DWORD,
    LPCWSTR, LPCWSTR, LPDWORD, LPCWSTR, LPCWSTR, LPCWSTR);
extern "C" SC_HANDLE WINAPI TestOpenServiceW(SC_HANDLE, LPCWSTR, DWORD);
extern "C" BOOL WINAPI TestChangeServiceConfigW(SC_HANDLE, DWORD, DWORD, DWORD, LPCWSTR, LPCWSTR, LPDWORD,
    LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR);
extern "C" BOOL WINAPI TestChangeServiceConfig2W(SC_HANDLE, DWORD, LPVOID);
extern "C" BOOL WINAPI TestStartServiceW(SC_HANDLE, DWORD, LPCWSTR*);
extern "C" BOOL WINAPI TestCloseServiceHandle(SC_HANDLE);
extern "C" void WINAPI TestSleep(DWORD);

#include "../main.cpp"
#undef wmain

namespace {

enum class InstallFailure { None, ExistingConfig, Description, FailureActions, Start };

InstallFailure installFailure = InstallFailure::None;
bool serviceExists = false;
bool startAlreadyRunning = false;
bool firmwareReadable = true;
std::vector<std::wstring> bootDevices;
std::map<std::wstring, bool> uploadResults;
std::map<std::wstring, int> uploadAttempts;
int failures = 0;

void Reset()
{
    installFailure = InstallFailure::None;
    serviceExists = false;
    startAlreadyRunning = false;
    firmwareReadable = true;
    bootDevices.clear();
    uploadResults.clear();
    uploadAttempts.clear();
    g_firmwareOverride = L"test-firmware.bin";
    g_stopping = false;
}

void ExpectEqual(const char* name, int actual, int expected)
{
    if (actual == expected) return;
    std::fprintf(stderr, "FAIL %s: got %d, expected %d\n", name, actual, expected);
    ++failures;
}

void TestInstallSuccess()
{
    Reset();
    ExpectEqual("install succeeds", Install(), 0);
}

void TestInstallAlreadyRunning()
{
    Reset();
    startAlreadyRunning = true;
    ExpectEqual("already-running service is success", Install(), 0);
}

void TestInstallFailures()
{
    const struct {
        InstallFailure failure;
        bool existing;
        const char* name;
    } cases[] = {
        {InstallFailure::ExistingConfig, true, "existing service config failure"},
        {InstallFailure::Description, false, "description config failure"},
        {InstallFailure::FailureActions, false, "failure-actions config failure"},
        {InstallFailure::Start, false, "service start failure"},
    };
    for (const auto& test : cases) {
        Reset();
        serviceExists = test.existing;
        installFailure = test.failure;
        ExpectEqual(test.name, Install(), 1);
    }
}

int RunLoad(const std::vector<std::wstring>& devices)
{
    bootDevices = devices;
    wchar_t exe[] = L"ps5cam-svc";
    wchar_t command[] = L"load";
    wchar_t firmware[] = L"test-firmware.bin";
    wchar_t* argv[] = {exe, command, firmware};
    return ps5cam_service_main(3, argv);
}

void TestLoadNoDevices()
{
    Reset();
    ExpectEqual("no boot devices", RunLoad({}), 2);
}

void TestLoadReadFailure()
{
    Reset();
    firmwareReadable = false;
    ExpectEqual("firmware read failure", RunLoad({L"device-a"}), 1);
}

void TestLoadUploadFailure()
{
    Reset();
    uploadResults[L"device-a"] = false;
    ExpectEqual("firmware upload failure", RunLoad({L"device-a"}), 1);
}

void TestLoadAggregatesDeviceResults()
{
    Reset();
    uploadResults[L"device-a"] = false;
    uploadResults[L"device-b"] = true;
    ExpectEqual("one failed device makes load fail", RunLoad({L"device-a", L"device-b"}), 1);

    Reset();
    uploadResults[L"device-a"] = true;
    uploadResults[L"device-b"] = true;
    ExpectEqual("all devices uploaded", RunLoad({L"device-a", L"device-b"}), 0);
}

void TestBootDeviceResults()
{
    Reset();
    firmwareReadable = false;
    ExpectEqual("BootDevice reports firmware read failure", BootDevice(L"device-a") ? 1 : 0, 0);

    Reset();
    bootDevices = {L"device-a"};
    uploadResults[L"device-a"] = false;
    ExpectEqual("BootDevice reports upload failure", BootDevice(L"device-a") ? 1 : 0, 0);
    ExpectEqual("BootDevice retries upload five times", uploadAttempts[L"device-a"], 5);

    Reset();
    uploadResults[L"device-a"] = true;
    ExpectEqual("BootDevice reports upload success", BootDevice(L"device-a") ? 1 : 0, 1);
    ExpectEqual("BootDevice stops after upload success", uploadAttempts[L"device-a"], 1);
}

}  // namespace

extern "C" SC_HANDLE WINAPI TestOpenSCManagerW(LPCWSTR, LPCWSTR, DWORD)
{
    return reinterpret_cast<SC_HANDLE>(1);
}

extern "C" SC_HANDLE WINAPI TestCreateServiceW(SC_HANDLE, LPCWSTR, LPCWSTR, DWORD, DWORD, DWORD, DWORD,
    LPCWSTR, LPCWSTR, LPDWORD, LPCWSTR, LPCWSTR, LPCWSTR)
{
    if (!serviceExists) return reinterpret_cast<SC_HANDLE>(2);
    SetLastError(ERROR_SERVICE_EXISTS);
    return nullptr;
}

extern "C" SC_HANDLE WINAPI TestOpenServiceW(SC_HANDLE, LPCWSTR, DWORD)
{
    return reinterpret_cast<SC_HANDLE>(2);
}

extern "C" BOOL WINAPI TestChangeServiceConfigW(SC_HANDLE, DWORD, DWORD, DWORD, LPCWSTR, LPCWSTR, LPDWORD,
    LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR)
{
    if (installFailure != InstallFailure::ExistingConfig) return TRUE;
    SetLastError(ERROR_ACCESS_DENIED);
    return FALSE;
}

extern "C" BOOL WINAPI TestChangeServiceConfig2W(SC_HANDLE, DWORD level, LPVOID)
{
    if ((level == SERVICE_CONFIG_DESCRIPTION && installFailure == InstallFailure::Description) ||
        (level == SERVICE_CONFIG_FAILURE_ACTIONS && installFailure == InstallFailure::FailureActions)) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    return TRUE;
}

extern "C" BOOL WINAPI TestStartServiceW(SC_HANDLE, DWORD, LPCWSTR*)
{
    if (startAlreadyRunning) {
        SetLastError(ERROR_SERVICE_ALREADY_RUNNING);
        return FALSE;
    }
    if (installFailure != InstallFailure::Start) return TRUE;
    SetLastError(ERROR_ACCESS_DENIED);
    return FALSE;
}

extern "C" BOOL WINAPI TestCloseServiceHandle(SC_HANDLE)
{
    return TRUE;
}

extern "C" void WINAPI TestSleep(DWORD) {}

namespace ps5cam {

bool TestLoadFirmwareFile(const std::wstring&, std::vector<uint8_t>& image, std::wstring& error)
{
    if (!firmwareReadable) {
        error = L"invalid firmware";
        return false;
    }
    image.assign(1024, 0x5a);
    return true;
}

bool TestUploadFirmware(const std::wstring& path, const std::vector<uint8_t>&, std::wstring& error)
{
    ++uploadAttempts[path];
    const auto found = uploadResults.find(path);
    if (found == uploadResults.end() || found->second) return true;
    error = L"upload failed";
    return false;
}

std::vector<std::wstring> TestFindBootDevices()
{
    return bootDevices;
}

bool IsBootDevicePath(const std::wstring&)
{
    return false;
}

void TestLogInit(const wchar_t*, bool) {}
void TestLog(const wchar_t*, ...) {}

}  // namespace ps5cam

int main()
{
    TestInstallSuccess();
    TestInstallAlreadyRunning();
    TestInstallFailures();
    TestLoadNoDevices();
    TestLoadReadFailure();
    TestLoadUploadFailure();
    TestLoadAggregatesDeviceResults();
    TestBootDeviceResults();
    if (failures) {
        std::fprintf(stderr, "%d service test(s) failed\n", failures);
        return 1;
    }
    std::puts("service tests passed");
    return 0;
}
