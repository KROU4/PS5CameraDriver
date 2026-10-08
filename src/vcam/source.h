#pragma once
// Frame Server custom media source for the "PS5 Camera" virtual camera.
//   Ps5Activate - registered COM class (IMFActivate); Frame Server calls ActivateObject.
//   Ps5Source   - IMFMediaSourceEx with a single video stream.
//   Ps5Stream   - IMFMediaStream2; answers RequestSample tokens with frames from CaptureEngine.
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <dshow.h>
#include <ks.h>
#include <ksmedia.h>
#include <ksproxy.h>
#include <wrl/implements.h>

#include <deque>
#include <mutex>
#include <vector>

#include "capture.h"

namespace ps5cam {

class Ps5Source;

class Ps5Stream
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          Microsoft::WRL::ChainInterfaces<IMFMediaStream2, IMFMediaStream, IMFMediaEventGenerator>> {
public:
    HRESULT RuntimeClassInitialize(Ps5Source* parent, bool prefer60, bool fullHdOnly);
    HRESULT Start();  // StartEngine + MEStreamStarted
    HRESULT Stop();
    void Shutdown();
    IMFStreamDescriptor* Descriptor() { return m_descriptor.Get(); }
    IMFAttributes* Attributes() { return m_attributes.Get(); }
    Microsoft::WRL::ComPtr<IUnknown> PhysicalSource() { return m_engine.PhysicalSource(); }

    // IMFMediaEventGenerator
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* cb, IUnknown* state) override;
    STDMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** ev) override;
    STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** ev) override;
    STDMETHODIMP QueueEvent(MediaEventType type, REFGUID ext, HRESULT status, const PROPVARIANT* pv) override;
    // IMFMediaStream
    STDMETHODIMP GetMediaSource(IMFMediaSource** source) override;
    STDMETHODIMP GetStreamDescriptor(IMFStreamDescriptor** sd) override;
    STDMETHODIMP RequestSample(IUnknown* token) override;
    // IMFMediaStream2
    STDMETHODIMP SetStreamState(MF_STREAM_STATE state) override;
    STDMETHODIMP GetStreamState(MF_STREAM_STATE* state) override;

private:
    HRESULT StartEngine();  // (re)starts capture for the current media type in a new epoch, state running
    void StopEngine();      // state stopped, new epoch, capture stopped
    void OnFrame(IMFSample* sample, uint64_t epoch);
    void DeliverLocked();
    HRESULT CheckShutdown() const { return m_shutdown ? MF_E_SHUTDOWN : S_OK; }

    // Serialises Start/Stop/Shutdown/SetStreamState so the stream state and the engine change
    // together; taken before m_lock and never on the frame path (OnFrame, RequestSample).
    std::mutex m_control;
    std::recursive_mutex m_lock;
    Ps5Source* m_parent = nullptr;  // weak: the source owns the stream
    Microsoft::WRL::ComPtr<IMFMediaEventQueue> m_queue;
    Microsoft::WRL::ComPtr<IMFStreamDescriptor> m_descriptor;
    Microsoft::WRL::ComPtr<IMFAttributes> m_attributes;
    MF_STREAM_STATE m_state = MF_STREAM_STATE_STOPPED;
    bool m_shutdown = false;
    std::deque<Microsoft::WRL::ComPtr<IUnknown>> m_tokens;
    Microsoft::WRL::ComPtr<IMFSample> m_pending;  // newest undelivered frame
    uint64_t m_epoch = 0;  // bumped on every start/stop; frames from older sessions are dropped
    CaptureEngine m_engine;
};

class Ps5Source
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          Microsoft::WRL::ChainInterfaces<IMFMediaSourceEx, IMFMediaSource, IMFMediaEventGenerator>, IMFGetService,
          IKsControl> {
public:
    HRESULT RuntimeClassInitialize(IMFAttributes* activateAttributes);
    bool IsShutdown()
    {
        std::lock_guard lock(m_lock);
        return m_shutdown;
    }

    // IMFMediaEventGenerator
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* cb, IUnknown* state) override;
    STDMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** ev) override;
    STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** ev) override;
    STDMETHODIMP QueueEvent(MediaEventType type, REFGUID ext, HRESULT status, const PROPVARIANT* pv) override;
    // IMFMediaSource
    STDMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor** pd) override;
    STDMETHODIMP GetCharacteristics(DWORD* characteristics) override;
    STDMETHODIMP Pause() override;
    STDMETHODIMP Shutdown() override;
    STDMETHODIMP Start(IMFPresentationDescriptor* pd, const GUID* timeFormat, const PROPVARIANT* startPosition) override;
    STDMETHODIMP Stop() override;
    // IMFMediaSourceEx
    STDMETHODIMP GetSourceAttributes(IMFAttributes** attributes) override;
    STDMETHODIMP GetStreamAttributes(DWORD streamId, IMFAttributes** attributes) override;
    STDMETHODIMP SetD3DManager(IUnknown* manager) override;
    // IMFGetService
    STDMETHODIMP GetService(REFGUID service, REFIID riid, LPVOID* object) override;
    // IKsControl
    STDMETHODIMP KsProperty(PKSPROPERTY property, ULONG propertyLength, LPVOID data, ULONG dataLength,
        ULONG* bytesReturned) override;
    STDMETHODIMP KsMethod(PKSMETHOD method, ULONG methodLength, LPVOID data, ULONG dataLength,
        ULONG* bytesReturned) override;
    STDMETHODIMP KsEvent(PKSEVENT event, ULONG eventLength, LPVOID data, ULONG dataLength,
        ULONG* bytesReturned) override;

private:
    HRESULT CheckShutdown() const { return m_shutdown ? MF_E_SHUTDOWN : S_OK; }

    std::recursive_mutex m_lock;
    bool m_shutdown = false;
    Microsoft::WRL::ComPtr<IMFMediaEventQueue> m_queue;
    Microsoft::WRL::ComPtr<IMFAttributes> m_attributes;
    Microsoft::WRL::ComPtr<IMFPresentationDescriptor> m_pd;
    Microsoft::WRL::ComPtr<Ps5Stream> m_stream;
    bool m_streamActive = false;
};

// IMFActivate that creates Ps5Source. All IMFAttributes calls go to an inner attribute store.
class Ps5Activate
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          Microsoft::WRL::ChainInterfaces<IMFActivate, IMFAttributes>> {
public:
    HRESULT RuntimeClassInitialize();

    STDMETHODIMP ActivateObject(REFIID riid, void** ppv) override;
    STDMETHODIMP ShutdownObject() override;
    STDMETHODIMP DetachObject() override;

#define FWD(name, params, args) \
    STDMETHODIMP name params override { return m_attr->name args; }
    FWD(GetItem, (REFGUID k, PROPVARIANT* v), (k, v))
    FWD(GetItemType, (REFGUID k, MF_ATTRIBUTE_TYPE* t), (k, t))
    FWD(CompareItem, (REFGUID k, REFPROPVARIANT v, BOOL* r), (k, v, r))
    FWD(Compare, (IMFAttributes* a, MF_ATTRIBUTES_MATCH_TYPE m, BOOL* r), (a, m, r))
    FWD(GetUINT32, (REFGUID k, UINT32* v), (k, v))
    FWD(GetUINT64, (REFGUID k, UINT64* v), (k, v))
    FWD(GetDouble, (REFGUID k, double* v), (k, v))
    FWD(GetGUID, (REFGUID k, GUID* v), (k, v))
    FWD(GetStringLength, (REFGUID k, UINT32* l), (k, l))
    FWD(GetString, (REFGUID k, LPWSTR v, UINT32 n, UINT32* l), (k, v, n, l))
    FWD(GetAllocatedString, (REFGUID k, LPWSTR* v, UINT32* l), (k, v, l))
    FWD(GetBlobSize, (REFGUID k, UINT32* s), (k, s))
    FWD(GetBlob, (REFGUID k, UINT8* b, UINT32 n, UINT32* s), (k, b, n, s))
    FWD(GetAllocatedBlob, (REFGUID k, UINT8** b, UINT32* s), (k, b, s))
    FWD(GetUnknown, (REFGUID k, REFIID r, LPVOID* p), (k, r, p))
    FWD(SetItem, (REFGUID k, REFPROPVARIANT v), (k, v))
    FWD(DeleteItem, (REFGUID k), (k))
    FWD(DeleteAllItems, (), ())
    FWD(SetUINT32, (REFGUID k, UINT32 v), (k, v))
    FWD(SetUINT64, (REFGUID k, UINT64 v), (k, v))
    FWD(SetDouble, (REFGUID k, double v), (k, v))
    FWD(SetGUID, (REFGUID k, REFGUID v), (k, v))
    FWD(SetString, (REFGUID k, LPCWSTR v), (k, v))
    FWD(SetBlob, (REFGUID k, const UINT8* b, UINT32 s), (k, b, s))
    FWD(SetUnknown, (REFGUID k, IUnknown* u), (k, u))
    FWD(LockStore, (), ())
    FWD(UnlockStore, (), ())
    FWD(GetCount, (UINT32* c), (c))
    FWD(GetItemByIndex, (UINT32 i, GUID* k, PROPVARIANT* v), (i, k, v))
    FWD(CopyAllItems, (IMFAttributes* d), (d))
#undef FWD

private:
    Microsoft::WRL::ComPtr<IMFAttributes> m_attr;
    Microsoft::WRL::ComPtr<Ps5Source> m_source;
};

}  // namespace ps5cam
