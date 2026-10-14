#include "../../common/frameclock.h"
#include "../../core/frametiming.h"
#include "../dmft.h"
#include "../processor.h"
#include "cameramodes.h"

#include <mfapi.h>
#include <mferror.h>
#include <wrl/client.h>

#include <cstdio>

using Microsoft::WRL::ComPtr;
using namespace ps5cam;

namespace ps5cam {

struct DeviceMftTestAccess {
    static void AddRunningPassThrough(DeviceMft* transform, DWORD id)
    {
        auto stream = std::make_unique<DeviceMft::Stream>();
        stream->id = id;
        stream->inputState = DeviceStreamState_Run;
        stream->outputState = DeviceStreamState_Run;
        transform->m_streams.push_back(std::move(stream));
    }
};

}  // namespace ps5cam

namespace {

int checks = 0, failures = 0;
void Check(bool ok, const char* expression, int line)
{
    ++checks;
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "frametiming_tests.cpp:%d: %s\n", line, expression);
    }
}
#define CHECK(expression) Check((expression), #expression, __LINE__)

ComPtr<IMFSample> Sample()
{
    ComPtr<IMFSample> sample;
    MFCreateSample(&sample);
    return sample;
}

ComPtr<IMFSample> VideoSample(uint64_t deviceTime, uint64_t ingressTime, uint64_t sequence, LONGLONG sampleTime)
{
    ComPtr<IMFMediaBuffer> buffer;
    ComPtr<IMFSample> sample;
    if (FAILED(MFCreate2DMediaBuffer(1920, 1080, MFVideoFormat_YUY2.Data1, FALSE, &buffer)) ||
        FAILED(MFCreateSample(&sample)) || FAILED(sample->AddBuffer(buffer.Get())))
        return nullptr;
    ComPtr<IMF2DBuffer2> twoD;
    BYTE* scan0 = nullptr;
    BYTE* start = nullptr;
    LONG pitch = 0;
    DWORD length = 0;
    if (FAILED(buffer.As(&twoD)) ||
        FAILED(twoD->Lock2DSize(MF2DBuffer_LockFlags_Write, &scan0, &pitch, &start, &length)))
        return nullptr;
    for (uint32_t y = 0; y < 1080; ++y) {
        BYTE* row = scan0 + size_t(y) * pitch;
        for (uint32_t x = 0; x < 1920; x += 2) {
            row[x * 2] = static_cast<BYTE>(32 + (x + y) % 160);
            row[x * 2 + 1] = 128;
            row[x * 2 + 2] = static_cast<BYTE>(32 + (x + y + 1) % 160);
            row[x * 2 + 3] = 128;
        }
    }
    twoD->Unlock2D();
    sample->SetSampleTime(sampleTime);
    sample->SetUINT64(MFSampleExtension_DeviceTimestamp, deviceTime);
    StampIngress(sample.Get(), ingressTime, sequence);
    return sample;
}

void DelayedMetadataFollowsTheDeliveredFrame()
{
    DelayedFrameTiming history;
    FrameTiming first = {.deviceTime = 111, .ingressTime = 222, .sequence = 7, .hasDeviceTime = true,
        .hasIngressTime = true, .hasSequence = true};
    FrameTiming second = {.deviceTime = 333, .ingressTime = 444, .sequence = 8, .hasDeviceTime = true,
        .hasIngressTime = true, .hasSequence = true};

    CHECK(!history.Advance(first).has_value());
    const auto delivered = history.Advance(second);
    CHECK(delivered.has_value());
    CHECK(delivered->deviceTime == 111);
    CHECK(delivered->ingressTime == 222);
    CHECK(delivered->sequence == 7);

    history.Reset();
    CHECK(!history.Advance(second).has_value());
}

void MissingDeviceTimestampStaysMissing()
{
    ComPtr<IMFSample> source = Sample();
    ComPtr<IMFSample> output = Sample();
    source->SetUINT64(kFrameIngressTime, 500);
    source->SetUINT64(kFrameSequence, 9);

    const FrameTiming timing = ReadFrameTiming(source.Get());
    CHECK(!timing.hasDeviceTime);
    CHECK(timing.hasIngressTime && timing.ingressTime == 500);
    ApplyProcessedTiming(output.Get(), timing, 800);

    UINT64 value = 0;
    CHECK(output->GetUINT64(MFSampleExtension_DeviceTimestamp, &value) == MF_E_ATTRIBUTENOTFOUND);
    CHECK(SUCCEEDED(output->GetUINT64(kFrameIngressTime, &value)) && value == 500);
    CHECK(SUCCEEDED(output->GetUINT64(kFrameProcessedTime, &value)) && value == 800);
    CHECK(SUCCEEDED(output->GetUINT64(kFrameSequence, &value)) && value == 9);
}

void DeviceTimestampAndDeliveryMetadataArePreserved()
{
    ComPtr<IMFSample> source = Sample();
    ComPtr<IMFSample> output = Sample();
    source->SetUINT64(MFSampleExtension_DeviceTimestamp, 1'234'567);
    StampIngress(source.Get(), 2'000'000, 42);

    ApplyProcessedTiming(output.Get(), ReadFrameTiming(source.Get()), 2'300'000);
    StampDelivery(output.Get(), 2'500'000, 3);

    UINT64 value = 0;
    CHECK(SUCCEEDED(output->GetUINT64(MFSampleExtension_DeviceTimestamp, &value)) && value == 1'234'567);
    CHECK(SUCCEEDED(output->GetUINT64(kFrameIngressTime, &value)) && value == 2'000'000);
    CHECK(SUCCEEDED(output->GetUINT64(kFrameProcessedTime, &value)) && value == 2'300'000);
    CHECK(SUCCEEDED(output->GetUINT64(kFrameDeliveredTime, &value)) && value == 2'500'000);
    CHECK(SUCCEEDED(output->GetUINT64(kFrameSequence, &value)) && value == 42);
    CHECK(SUCCEEDED(output->GetUINT64(kDroppedFrames, &value)) && value == 3);
}

void AuthoritativeIngressReplacesAttributesLeftOnAReusedSample()
{
    ComPtr<IMFSample> sample = Sample();
    sample->SetUINT64(kFrameIngressTime, 100);
    sample->SetUINT64(kFrameSequence, 5);
    StampIngress(sample.Get(), 900, 12);

    UINT64 value = 0;
    CHECK(SUCCEEDED(sample->GetUINT64(kFrameIngressTime, &value)) && value == 900);
    CHECK(SUCCEEDED(sample->GetUINT64(kFrameSequence, &value)) && value == 12);
}

void LatestFrameStateCountsOnlyReplacedFrames()
{
    LatestFrameState state;
    CHECK(!state.Replace(false));
    CHECK(state.Replace(true));
    CHECK(state.Replace(true));
    CHECK(state.TakeDropped() == 2);
    CHECK(state.TakeDropped() == 0);
    state.Replace(true);
    state.Reset();
    CHECK(state.TakeDropped() == 0);
}

void DeviceMftQueuesOnlyTheNewestFrameAndOneAvailabilityEvent()
{
    constexpr DWORD streamId = 7;
    ComPtr<DeviceMft> transform;
    CHECK(SUCCEEDED(Microsoft::WRL::MakeAndInitialize<DeviceMft>(&transform)));
    if (!transform) return;
    DeviceMftTestAccess::AddRunningPassThrough(transform.Get(), streamId);

    ComPtr<IMFSample> first = Sample();
    ComPtr<IMFSample> second = Sample();
    first->SetUINT64(MFSampleExtension_DeviceTimestamp, 111);
    second->SetUINT64(MFSampleExtension_DeviceTimestamp, 222);
    CHECK(SUCCEEDED(transform->ProcessInput(streamId, first.Get(), 0)));
    CHECK(SUCCEEDED(transform->ProcessInput(streamId, second.Get(), 0)));

    ComPtr<IMFMediaEvent> event;
    CHECK(SUCCEEDED(transform->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event)));
    MediaEventType eventType = MEUnknown;
    CHECK(event && SUCCEEDED(event->GetType(&eventType)) && eventType == METransformHaveOutput);
    event.Reset();
    CHECK(transform->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event) == MF_E_NO_EVENTS_AVAILABLE);

    MFT_OUTPUT_DATA_BUFFER output = {};
    output.dwStreamID = streamId;
    DWORD status = 0;
    CHECK(SUCCEEDED(transform->ProcessOutput(0, 1, &output, &status)));
    ComPtr<IMFSample> delivered;
    delivered.Attach(output.pSample);
    CHECK(delivered.Get() == second.Get());
    UINT64 value = 0;
    UINT32 flag = FALSE;
    CHECK(SUCCEEDED(delivered->GetUINT64(MFSampleExtension_DeviceTimestamp, &value)) && value == 222);
    CHECK(SUCCEEDED(delivered->GetUINT64(kFrameSequence, &value)) && value == 2);
    CHECK(SUCCEEDED(delivered->GetUINT64(kDroppedFrames, &value)) && value == 1);
    CHECK(SUCCEEDED(delivered->GetUINT64(kFrameDeliveredTime, &value)) && value > 0);
    CHECK(SUCCEEDED(delivered->GetUINT32(MFSampleExtension_Discontinuity, &flag)) && flag == TRUE);
    output.pSample = nullptr;
    CHECK(transform->ProcessOutput(0, 1, &output, &status) == MF_E_TRANSFORM_NEED_MORE_INPUT);

    ComPtr<IMFSample> beforeFlush = Sample();
    CHECK(SUCCEEDED(transform->ProcessInput(streamId, beforeFlush.Get(), 0)));
    CHECK(SUCCEEDED(transform->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event)));
    CHECK(SUCCEEDED(transform->FlushOutputStream(streamId, 0)));
    CHECK(transform->ProcessOutput(0, 1, &output, &status) == MF_E_TRANSFORM_NEED_MORE_INPUT);

    ComPtr<IMFSample> afterFlush = Sample();
    CHECK(SUCCEEDED(transform->ProcessInput(streamId, afterFlush.Get(), 0)));
    CHECK(SUCCEEDED(transform->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event)));
    CHECK(SUCCEEDED(transform->ProcessOutput(0, 1, &output, &status)));
    delivered.Reset();
    delivered.Attach(output.pSample);
    CHECK(SUCCEEDED(delivered->GetUINT64(kFrameSequence, &value)) && value == 1);
    CHECK(SUCCEEDED(delivered->GetUINT64(kDroppedFrames, &value)) && value == 0);
    flag = FALSE;
    CHECK(SUCCEEDED(delivered->GetUINT32(MFSampleExtension_Discontinuity, &flag)) && flag == TRUE);
    CHECK(SUCCEEDED(transform->Shutdown()));
}

void FrameProcessorReturnsMetadataFromTheImageItActuallyDelivers()
{
    ComPtr<IMFMediaType> input;
    ComPtr<IMFMediaType> output;
    CHECK(SUCCEEDED(MakeVideoType({1920, 1080, 30, PixelFormat::YUY2}, &input)));
    CHECK(SUCCEEDED(MakeVideoType({1280, 720, 30, PixelFormat::NV12}, &output)));
    FrameProcessor processor(false);
    CHECK(processor.Configure(input.Get(), output.Get()));

    ComPtr<IMFSample> first = VideoSample(111, 222, 7, 1'000);
    ComPtr<IMFSample> second = VideoSample(333, 444, 8, 2'000);
    CHECK(first && second);
    CHECK(!processor.Process(first.Get()));
    ComPtr<IMFSample> delivered = processor.Process(second.Get());
    CHECK(delivered);
    if (!delivered) return;

    UINT64 value = 0;
    LONGLONG sampleTime = 0;
    CHECK(SUCCEEDED(delivered->GetUINT64(MFSampleExtension_DeviceTimestamp, &value)) && value == 111);
    CHECK(SUCCEEDED(delivered->GetUINT64(kFrameIngressTime, &value)) && value == 222);
    CHECK(SUCCEEDED(delivered->GetUINT64(kFrameSequence, &value)) && value == 7);
    CHECK(SUCCEEDED(delivered->GetUINT64(kFrameProcessedTime, &value)) && value >= 222);
    CHECK(SUCCEEDED(delivered->GetUINT64(kFrameGpuTimeUs, &value)) && value > 0);
    UINT32 flag = FALSE;
    CHECK(SUCCEEDED(delivered->GetUINT32(MFSampleExtension_Discontinuity, &flag)) && flag == TRUE);
    CHECK(SUCCEEDED(delivered->GetSampleTime(&sampleTime)) && sampleTime == 1'000);
}

void FrameProcessorFlushDropsThePrimedFrameAndItsMetadata()
{
    ComPtr<IMFMediaType> input;
    ComPtr<IMFMediaType> output;
    CHECK(SUCCEEDED(MakeVideoType({1920, 1080, 30, PixelFormat::YUY2}, &input)));
    CHECK(SUCCEEDED(MakeVideoType({1280, 720, 30, PixelFormat::NV12}, &output)));
    FrameProcessor processor(false);
    CHECK(processor.Configure(input.Get(), output.Get()));
    CHECK(!processor.Process(VideoSample(111, 222, 7, 1'000).Get()));

    processor.Flush();
    CHECK(!processor.Process(VideoSample(333, 444, 8, 2'000).Get()));
    ComPtr<IMFSample> delivered = processor.Process(VideoSample(555, 666, 9, 3'000).Get());
    CHECK(delivered);
    if (!delivered) return;
    UINT64 value = 0;
    CHECK(SUCCEEDED(delivered->GetUINT64(MFSampleExtension_DeviceTimestamp, &value)) && value == 333);
    CHECK(SUCCEEDED(delivered->GetUINT64(kFrameSequence, &value)) && value == 8);
    UINT32 flag = FALSE;
    CHECK(SUCCEEDED(delivered->GetUINT32(MFSampleExtension_Discontinuity, &flag)) && flag == TRUE);
}

}  // namespace

int main()
{
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) return 2;
    DelayedMetadataFollowsTheDeliveredFrame();
    MissingDeviceTimestampStaysMissing();
    DeviceTimestampAndDeliveryMetadataArePreserved();
    AuthoritativeIngressReplacesAttributesLeftOnAReusedSample();
    LatestFrameStateCountsOnlyReplacedFrames();
    DeviceMftQueuesOnlyTheNewestFrameAndOneAvailabilityEvent();
    FrameProcessorReturnsMetadataFromTheImageItActuallyDelivers();
    FrameProcessorFlushDropsThePrimedFrameAndItsMetadata();
    MFShutdown();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
