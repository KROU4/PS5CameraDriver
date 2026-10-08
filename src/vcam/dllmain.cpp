// In-proc COM server for the PS5 Camera media source (loaded by Frame Server).
#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>

#include "source.h"
#include "../common/ids.h"
#include "../common/log.h"

using Microsoft::WRL::ComPtr;

namespace {

HMODULE g_module = nullptr;
std::once_flag g_logOnce;  // logging is set up on first use, never under the loader lock

class ClassFactory : public IClassFactory {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++m_ref; }
    STDMETHODIMP_(ULONG) Release() override
    {
        ULONG r = --m_ref;
        if (r == 0) delete this;
        return r;
    }
    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        ComPtr<ps5cam::Ps5Activate> activate;
        HRESULT hr = Microsoft::WRL::MakeAndInitialize<ps5cam::Ps5Activate>(&activate);
        if (FAILED(hr)) return hr;
        return activate->QueryInterface(riid, ppv);
    }
    STDMETHODIMP LockServer(BOOL) override { return S_OK; }

private:
    std::atomic<ULONG> m_ref = 1;
};

LSTATUS SetValue(HKEY key, const wchar_t* name, const std::wstring& value)
{
    return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (clsid != ps5cam::kSourceClsid) return CLASS_E_CLASSNOTAVAILABLE;
    std::call_once(g_logOnce, [] { ps5cam::LogInit(L"vcam"); });
    auto* factory = new (std::nothrow) ClassFactory();
    if (!factory) return E_OUTOFMEMORY;
    HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

STDAPI DllCanUnloadNow()
{
    // Engine threads may outlive individual COM objects briefly; Frame Server keeps us loaded anyway.
    return S_FALSE;
}

STDAPI DllRegisterServer()
{
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(g_module, path, MAX_PATH)) return HRESULT_FROM_WIN32(GetLastError());
    std::wstring key = std::wstring(L"Software\\Classes\\CLSID\\") + ps5cam::kSourceClsidString;
    HKEY clsidKey = nullptr, inproc = nullptr;
    LSTATUS st = RegCreateKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &clsidKey, nullptr);
    if (st != ERROR_SUCCESS) return HRESULT_FROM_WIN32(st);
    SetValue(clsidKey, nullptr, L"PS5 Camera Media Source");
    st = RegCreateKeyExW(clsidKey, L"InprocServer32", 0, nullptr, 0, KEY_WRITE, nullptr, &inproc, nullptr);
    if (st == ERROR_SUCCESS) {
        SetValue(inproc, nullptr, path);
        SetValue(inproc, L"ThreadingModel", L"Both");
        RegCloseKey(inproc);
    }
    RegCloseKey(clsidKey);
    return HRESULT_FROM_WIN32(st);
}

STDAPI DllUnregisterServer()
{
    std::wstring key = std::wstring(L"Software\\Classes\\CLSID\\") + ps5cam::kSourceClsidString;
    LSTATUS st = RegDeleteTreeW(HKEY_LOCAL_MACHINE, key.c_str());
    return st == ERROR_SUCCESS || st == ERROR_FILE_NOT_FOUND ? S_OK : HRESULT_FROM_WIN32(st);
}
