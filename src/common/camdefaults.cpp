#include "camdefaults.h"

#include <initguid.h>
#include <cfgmgr32.h>
#include <ks.h>
#include <ksmedia.h>
#include <mfapi.h>
#include <mfidl.h>
#include <wrl/client.h>

#include <cwctype>
#include <functional>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace ps5cam {

namespace {

constexpr uint64_t kBlur = KSCAMERA_EXTENDEDPROP_BACKGROUNDSEGMENTATION_BLUR;
constexpr uint64_t kShallowFocus = KSCAMERA_EXTENDEDPROP_BACKGROUNDSEGMENTATION_SHALLOWFOCUS;

// The camera's video interface, the device Windows keeps the defaults for.
std::wstring CameraInterface()
{
    GUID category = KSCATEGORY_VIDEO_CAMERA;
    ULONG len = 0;
    if (CM_Get_Device_Interface_List_SizeW(&len, &category, nullptr, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS ||
        len < 2)
        return {};
    std::vector<wchar_t> list(len);
    if (CM_Get_Device_Interface_ListW(&category, nullptr, list.data(), len, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) !=
        CR_SUCCESS)
        return {};
    for (const wchar_t* p = list.data(); *p; p += wcslen(p) + 1) {
        std::wstring lower(p);
        for (auto& c : lower) c = static_cast<wchar_t>(towlower(c));
        if (lower.find(L"vid_05a9&pid_058c&mi_00") != std::wstring::npos) return p;
    }
    return {};
}

constexpr HRESULT kNotConnected = HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED);

// Calls visit for every saved default of the background effect (there may be one per
// configuration type); saves the collection if visit returns true for any. With removeIfNone, a
// default that is not listed is removed: Windows keeps "off" (set in Settings, or written here)
// out of the list but still applies it at every start.
HRESULT VisitBackgroundEffectDefaults(
    const std::function<bool(KSCAMERA_EXTENDEDPROP_HEADER&, MF_CAMERA_CONTROL_CONFIGURATION_TYPE)>& visit,
    bool removeIfNone = false, bool* removed = nullptr)
{
    const std::wstring link = CameraInterface();
    if (link.empty()) return kNotConnected;
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (SUCCEEDED(hr)) {
        ComPtr<IMFCameraConfigurationManager> manager;
        ComPtr<IMFAttributes> camera;
        ComPtr<IMFCameraControlDefaultsCollection> defaults;
        hr = CoCreateInstance(CLSID_CameraConfigurationManager, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&manager));
        if (SUCCEEDED(hr)) hr = MFCreateAttributes(&camera, 1);
        if (SUCCEEDED(hr)) hr = camera->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, link.c_str());
        if (SUCCEEDED(hr)) hr = manager->LoadDefaults(camera.Get(), &defaults);
        bool changed = false, listed = false;
        for (ULONG i = 0; SUCCEEDED(hr) && i < defaults->GetControlCount(); ++i) {
            ComPtr<IMFCameraControlDefaults> control;
            void* property = nullptr;
            void* data = nullptr;
            ULONG propertySize = 0, dataSize = 0;
            if (FAILED(defaults->GetControl(i, &control)) ||
                FAILED(control->LockControlData(&property, &propertySize, &data, &dataSize)))
                continue;
            const auto* p = static_cast<const KSPROPERTY*>(property);
            if (p && propertySize >= sizeof(KSPROPERTY) && IsEqualGUID(p->Set, KSPROPERTYSETID_ExtendedCameraControl) &&
                p->Id == KSPROPERTY_CAMERACONTROL_EXTENDED_BACKGROUNDSEGMENTATION && data &&
                dataSize >= sizeof(KSCAMERA_EXTENDEDPROP_HEADER)) {
                listed = true;
                changed |= visit(*static_cast<KSCAMERA_EXTENDEDPROP_HEADER*>(data), control->GetType());
            }
            control->UnlockControlData();
        }
        if (SUCCEEDED(hr) && removeIfNone && !listed &&
            SUCCEEDED(defaults->RemoveControl(KSPROPERTYSETID_ExtendedCameraControl,
                KSPROPERTY_CAMERACONTROL_EXTENDED_BACKGROUNDSEGMENTATION))) {
            changed = true;
            if (removed) *removed = true;
        }
        if (SUCCEEDED(hr) && changed) hr = manager->SaveDefaults(defaults.Get());
        if (manager) manager->Shutdown();
        MFShutdown();
    }
    if (SUCCEEDED(com)) CoUninitialize();
    return hr;
}

const wchar_t* FlagsText(uint64_t flags)
{
    if (!(flags & kBlur)) return L"off";
    return (flags & kShallowFocus) ? L"portrait blur" : L"standard blur";
}

}  // namespace

uint64_t BackgroundEffectFlags(const Settings& s)
{
    if (s.mode != 0) return KSCAMERA_EXTENDEDPROP_BACKGROUNDSEGMENTATION_OFF;
    return s.blurStyle == kBlurStandard ? kBlur : kBlur | kShallowFocus;
}

HRESULT SyncBackgroundEffectDefault(const Settings& s, std::wstring* message)
{
    // A listed default takes the new value (set to off, it drops out of the list but still applies,
    // which is what off wants). With the bokeh on and nothing listed, a hidden "off" would switch it
    // off again at the next start: removed.
    const ULONGLONG flags = BackgroundEffectFlags(s);
    bool updated = false, removed = false;
    const HRESULT hr = VisitBackgroundEffectDefaults(
        [&](KSCAMERA_EXTENDEDPROP_HEADER& h, MF_CAMERA_CONTROL_CONFIGURATION_TYPE) {
            if (h.Flags == flags) return false;
            h.Flags = flags;
            updated = true;
            return true;
        },
        flags != KSCAMERA_EXTENDEDPROP_BACKGROUNDSEGMENTATION_OFF, &removed);
    if (message)
        *message = hr == kNotConnected ? L"camera not connected"
                   : FAILED(hr)        ? L"failed"
                   : removed           ? L"removed (it would have switched the bokeh off)"
                   : updated           ? std::wstring(L"updated to ") + FlagsText(flags)
                                       : L"nothing to update";
    return FAILED(hr) ? hr : (updated || removed) ? S_OK : S_FALSE;
}

std::wstring DescribeBackgroundEffectDefault()
{
    std::wstring text;
    const HRESULT hr = VisitBackgroundEffectDefaults([&](KSCAMERA_EXTENDEDPROP_HEADER& h, MF_CAMERA_CONTROL_CONFIGURATION_TYPE t) {
        if (!text.empty()) text += L", ";
        text += FlagsText(h.Flags);
        text += t == MF_CAMERA_CONTROL_CONFIGURATION_TYPE_PRESTART ? L" (before start)" : L" (after start)";
        return false;
    });
    if (hr == kNotConnected) return L"camera not connected";
    if (FAILED(hr)) {
        wchar_t buf[48];
        swprintf_s(buf, L"unknown (0x%08lX)", static_cast<unsigned long>(hr));
        return buf;
    }
    return text.empty() ? L"none, or off (Windows does not list that one)" : text;
}

}  // namespace ps5cam
