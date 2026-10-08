#include "camctl.h"

#include <dshow.h>
#include <ks.h>
#include <ksmedia.h>
#include <ksproxy.h>
#include <wrl/client.h>

namespace ps5cam {

HRESULT SetPowerLineFrequency(IUnknown* camera, int value)
{
    Microsoft::WRL::ComPtr<IKsControl> ks;
    if (!camera) return E_POINTER;
    HRESULT hr = camera->QueryInterface(IID_PPV_ARGS(&ks));
    if (FAILED(hr)) return hr;
    KSPROPERTY_VIDEOPROCAMP_S p = {};
    p.Property.Set = PROPSETID_VIDCAP_VIDEOPROCAMP;
    p.Property.Id = KSPROPERTY_VIDEOPROCAMP_POWERLINE_FREQUENCY;
    p.Property.Flags = KSPROPERTY_TYPE_SET;
    p.Value = value;
    p.Flags = KSPROPERTY_VIDEOPROCAMP_FLAGS_MANUAL;
    ULONG returned = 0;
    return ks->KsProperty(&p.Property, sizeof(p), &p, sizeof(p), &returned);
}

}  // namespace ps5cam
