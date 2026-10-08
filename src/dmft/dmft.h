#pragma once
// Device MFT for the PS5 camera: Frame Server loads it into every app's view of the real camera
// (registered on the camera's device interface, see ps5cam-ctl dmft), so the camera itself shows
// the effect, under its own name, without a virtual camera.
//
// Each stream of the camera (usbvideo.sys: one video pin) becomes one input and one output. The
// video stream is processed: apps see the output types of cameramodes.h (Full HD / HD, 30/60 fps,
// NV12/YUY2) and the camera runs the sensor mode the effect needs; any other stream passes through.
// Model: DTM calls SetOutputStreamState with the app's type, we ask for the matching camera format
// with METransformInputStreamStateChanged, DTM sets it (SetInputStreamState), frames come through
// ProcessInput and leave through ProcessOutput after METransformHaveOutput.
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mftransform.h>
#include <dshow.h>
#include <ks.h>
#include <ksmedia.h>
#include <ksproxy.h>
#include <wrl/implements.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

#include "processor.h"

namespace ps5cam {

// Attributes of one of our output streams. IKsControl goes to the camera's pin, as the pipeline
// sends pin properties to the output stream.
class StreamAttributes
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          IMFAttributes, IKsControl> {
public:
    HRESULT RuntimeClassInitialize(IMFAttributes* source);

#define PS5_ATTR_FORWARD(name, params, args) \
    STDMETHODIMP name params override { return m_store->name args; }
    PS5_ATTR_FORWARD(GetItem, (REFGUID k, PROPVARIANT* v), (k, v))
    PS5_ATTR_FORWARD(GetItemType, (REFGUID k, MF_ATTRIBUTE_TYPE* t), (k, t))
    PS5_ATTR_FORWARD(CompareItem, (REFGUID k, REFPROPVARIANT v, BOOL* r), (k, v, r))
    PS5_ATTR_FORWARD(Compare, (IMFAttributes* t, MF_ATTRIBUTES_MATCH_TYPE m, BOOL* r), (t, m, r))
    PS5_ATTR_FORWARD(GetUINT32, (REFGUID k, UINT32* v), (k, v))
    PS5_ATTR_FORWARD(GetUINT64, (REFGUID k, UINT64* v), (k, v))
    PS5_ATTR_FORWARD(GetDouble, (REFGUID k, double* v), (k, v))
    PS5_ATTR_FORWARD(GetGUID, (REFGUID k, GUID* v), (k, v))
    PS5_ATTR_FORWARD(GetStringLength, (REFGUID k, UINT32* v), (k, v))
    PS5_ATTR_FORWARD(GetString, (REFGUID k, LPWSTR v, UINT32 n, UINT32* l), (k, v, n, l))
    PS5_ATTR_FORWARD(GetAllocatedString, (REFGUID k, LPWSTR* v, UINT32* l), (k, v, l))
    PS5_ATTR_FORWARD(GetBlobSize, (REFGUID k, UINT32* v), (k, v))
    PS5_ATTR_FORWARD(GetBlob, (REFGUID k, UINT8* v, UINT32 n, UINT32* l), (k, v, n, l))
    PS5_ATTR_FORWARD(GetAllocatedBlob, (REFGUID k, UINT8** v, UINT32* l), (k, v, l))
    PS5_ATTR_FORWARD(GetUnknown, (REFGUID k, REFIID i, LPVOID* v), (k, i, v))
    PS5_ATTR_FORWARD(SetItem, (REFGUID k, REFPROPVARIANT v), (k, v))
    PS5_ATTR_FORWARD(DeleteItem, (REFGUID k), (k))
    PS5_ATTR_FORWARD(DeleteAllItems, (), ())
    PS5_ATTR_FORWARD(SetUINT32, (REFGUID k, UINT32 v), (k, v))
    PS5_ATTR_FORWARD(SetUINT64, (REFGUID k, UINT64 v), (k, v))
    PS5_ATTR_FORWARD(SetDouble, (REFGUID k, double v), (k, v))
    PS5_ATTR_FORWARD(SetGUID, (REFGUID k, REFGUID v), (k, v))
    PS5_ATTR_FORWARD(SetString, (REFGUID k, LPCWSTR v), (k, v))
    PS5_ATTR_FORWARD(SetBlob, (REFGUID k, const UINT8* v, UINT32 n), (k, v, n))
    PS5_ATTR_FORWARD(SetUnknown, (REFGUID k, IUnknown* v), (k, v))
    PS5_ATTR_FORWARD(LockStore, (), ())
    PS5_ATTR_FORWARD(UnlockStore, (), ())
    PS5_ATTR_FORWARD(GetCount, (UINT32* n), (n))
    PS5_ATTR_FORWARD(GetItemByIndex, (UINT32 i, GUID* k, PROPVARIANT* v), (i, k, v))
    PS5_ATTR_FORWARD(CopyAllItems, (IMFAttributes* d), (d))
#undef PS5_ATTR_FORWARD

    STDMETHODIMP KsProperty(PKSPROPERTY p, ULONG pl, LPVOID d, ULONG dl, ULONG* r) override;
    STDMETHODIMP KsMethod(PKSMETHOD m, ULONG ml, LPVOID d, ULONG dl, ULONG* r) override;
    STDMETHODIMP KsEvent(PKSEVENT e, ULONG el, LPVOID d, ULONG dl, ULONG* r) override;

private:
    Microsoft::WRL::ComPtr<IMFAttributes> m_store;
    Microsoft::WRL::ComPtr<IKsControl> m_pin;  // the camera's pin, may be null
};

class DeviceMft
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          IMFDeviceTransform, IMFShutdown, IMFMediaEventGenerator, IMFRealTimeClientEx, IKsControl> {
public:
    HRESULT RuntimeClassInitialize();
    ~DeviceMft() override;

    // IMFDeviceTransform
    STDMETHODIMP InitializeTransform(IMFAttributes* attributes) override;
    STDMETHODIMP GetInputAvailableType(DWORD stream, DWORD index, IMFMediaType** type) override;
    STDMETHODIMP GetInputCurrentType(DWORD stream, IMFMediaType** type) override;
    STDMETHODIMP GetInputStreamAttributes(DWORD stream, IMFAttributes** attributes) override;
    STDMETHODIMP GetOutputAvailableType(DWORD stream, DWORD index, IMFMediaType** type) override;
    STDMETHODIMP GetOutputCurrentType(DWORD stream, IMFMediaType** type) override;
    STDMETHODIMP GetOutputStreamAttributes(DWORD stream, IMFAttributes** attributes) override;
    STDMETHODIMP GetStreamCount(DWORD* inputs, DWORD* outputs) override;
    STDMETHODIMP GetStreamIDs(DWORD inputSize, DWORD* inputs, DWORD outputSize, DWORD* outputs) override;
    STDMETHODIMP ProcessEvent(DWORD stream, IMFMediaEvent* event) override;
    STDMETHODIMP ProcessInput(DWORD stream, IMFSample* sample, DWORD flags) override;
    STDMETHODIMP ProcessMessage(MFT_MESSAGE_TYPE message, ULONG_PTR param) override;
    STDMETHODIMP ProcessOutput(DWORD flags, DWORD count, MFT_OUTPUT_DATA_BUFFER* buffers, DWORD* status) override;
    STDMETHODIMP SetInputStreamState(DWORD stream, IMFMediaType* type, DeviceStreamState state, DWORD flags) override;
    STDMETHODIMP GetInputStreamState(DWORD stream, DeviceStreamState* state) override;
    STDMETHODIMP SetOutputStreamState(DWORD stream, IMFMediaType* type, DeviceStreamState state, DWORD flags) override;
    STDMETHODIMP GetOutputStreamState(DWORD stream, DeviceStreamState* state) override;
    STDMETHODIMP GetInputStreamPreferredState(DWORD stream, DeviceStreamState* state, IMFMediaType** type) override;
    STDMETHODIMP FlushInputStream(DWORD stream, DWORD flags) override;
    STDMETHODIMP FlushOutputStream(DWORD stream, DWORD flags) override;

    // IMFShutdown
    STDMETHODIMP Shutdown() override;
    STDMETHODIMP GetShutdownStatus(MFSHUTDOWN_STATUS* status) override;

    // IMFMediaEventGenerator
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) override;
    STDMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) override;
    STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** event) override;
    STDMETHODIMP QueueEvent(MediaEventType type, REFGUID ext, HRESULT status, const PROPVARIANT* pv) override;

    // IMFRealTimeClientEx
    STDMETHODIMP RegisterThreadsEx(DWORD* taskIndex, LPCWSTR className, LONG basePriority) override;
    STDMETHODIMP UnregisterThreads() override;
    STDMETHODIMP SetWorkQueueEx(DWORD queue, LONG basePriority) override;

    // IKsControl: everything goes to the camera (brightness, exposure, Settings app pages)
    STDMETHODIMP KsProperty(PKSPROPERTY p, ULONG pl, LPVOID d, ULONG dl, ULONG* r) override;
    STDMETHODIMP KsMethod(PKSMETHOD m, ULONG ml, LPVOID d, ULONG dl, ULONG* r) override;
    STDMETHODIMP KsEvent(PKSEVENT e, ULONG el, LPVOID d, ULONG dl, ULONG* r) override;

private:
    struct Stream {
        DWORD id = 0;
        Microsoft::WRL::ComPtr<IMFAttributes> sourceAttributes;  // the camera pin's
        Microsoft::WRL::ComPtr<StreamAttributes> outputAttributes;
        std::vector<Microsoft::WRL::ComPtr<IMFMediaType>> inputTypes;   // what the camera offers
        std::vector<Microsoft::WRL::ComPtr<IMFMediaType>> outputTypes;  // what apps get offered
        bool processed = false;  // the video stream we run the effect on (else passed through)
        Microsoft::WRL::ComPtr<IMFMediaType> inputType, outputType;
        DeviceStreamState inputState = DeviceStreamState_Stop;
        DeviceStreamState outputState = DeviceStreamState_Stop;
        Microsoft::WRL::ComPtr<IMFMediaType> preferredType;
        DeviceStreamState preferredState = DeviceStreamState_Stop;
        uint64_t inputChanges = 0;    // bumped by SetInputStreamState
        bool inputChangePending = false;  // we asked for another camera format ourselves
        ULONGLONG pendingTick = 0;
        bool discontinuity = false;   // first sample after a format change
        std::deque<Microsoft::WRL::ComPtr<IMFSample>> queue;
        std::unique_ptr<FrameProcessor> processor;
    };

    HRESULT CheckShutdown() const { return m_shutdown ? MF_E_SHUTDOWN : S_OK; }
    Stream* Find(DWORD id);
    HRESULT InitializeLocked(IMFAttributes* attributes);
    HRESULT SetOutputLocked(std::unique_lock<std::mutex>& lock, DWORD stream, IMFMediaType* type,
        DeviceStreamState state, DWORD flags);
    // The camera format for an output type and sensor mode key (kHalfKey/kMonoKey), or null.
    IMFMediaType* InputFor(Stream& s, IMFMediaType* output, const wchar_t* wantedKey, bool quiet = false);
    HRESULT RequestInput(std::unique_lock<std::mutex>& lock, Stream& s, IMFMediaType* type, DeviceStreamState state,
        bool wait);
    void QueueStreamEvent(MediaEventType type, REFGUID ext, DWORD stream);
    HRESULT CameraKs(Microsoft::WRL::ComPtr<IKsControl>& ks);
    // Windows' "Background effects" (KSPROPERTY_CAMERACONTROL_EXTENDED_BACKGROUNDSEGMENTATION), which
    // the camera itself does not have: the bokeh, on and off, as portrait or standard blur.
    HRESULT BackgroundSegmentation(PKSPROPERTY p, LPVOID d, ULONG dl, ULONG* r);

    // Guards the streams and the camera transform. Held while a frame is processed (a few ms on
    // the GPU), never while calling the event queue or the camera's IKsControl.
    std::mutex m_lock;
    std::condition_variable m_inputChanged;
    bool m_shutdown = false;
    std::vector<std::unique_ptr<Stream>> m_streams;
    Microsoft::WRL::ComPtr<IMFDeviceTransform> m_source;
    Microsoft::WRL::ComPtr<IKsControl> m_sourceKs;
    Microsoft::WRL::ComPtr<IMFMediaEventQueue> m_events;  // set once in RuntimeClassInitialize
    std::atomic<uint32_t> m_inputReleases = 0;            // diagnostics for the ProcessInput reference quirk
};

}  // namespace ps5cam
