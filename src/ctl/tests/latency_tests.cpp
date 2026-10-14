#include "../../common/latencyreader.h"
#include <mfapi.h>
#include <wrl/client.h>
#include <cstdio>

int main()
{
    using namespace ps5cam;
    MFStartup(MF_VERSION);
    Microsoft::WRL::ComPtr<IMFSample> sample;
    if (FAILED(MFCreateSample(&sample))) return 1;
    int failures = 0;
    auto check = [&](bool result, const char* text) {
        if (!result) { ++failures; std::fprintf(stderr, "%s\n", text); }
    };
    auto missing = ReadLatency(sample.Get(), 100000);
    check(!missing.deviceToApp && !missing.driverToApp && !missing.gpuMs,
        "missing timestamps must not appear as zero latency");
    sample->SetUINT64(MFSampleExtension_DeviceTimestamp, 80000);
    sample->SetUINT64(kFrameIngressTime, 85000);
    sample->SetUINT64(kFrameProcessedTime, 91000);
    sample->SetUINT64(kFrameDeliveredTime, 93000);
    sample->SetUINT64(kFrameSequence, 42);
    sample->SetUINT64(kDroppedFrames, 5);
    sample->SetUINT64(kFrameGpuTimeUs, 250);
    auto measured = ReadLatency(sample.Get(), 100000);
    check(measured.deviceToApp == 2 && measured.driverToApp == 1.5 &&
        measured.processingSpan == 0.6 && measured.queue == 0.2 && measured.transport == 0.7 &&
        measured.gpuMs == 0.25 && measured.sequence == 42 && measured.dropped == 5,
        "known actual IMF attributes were decoded with wrong units or boundaries");
    sample->SetUINT64(MFSampleExtension_DeviceTimestamp, 110000);
    sample->SetUINT64(kFrameProcessedTime, 101000);
    auto invalid = ReadLatency(sample.Get(), 100000);
    check(!invalid.deviceToApp && !invalid.processingSpan && !invalid.queue,
        "future timestamps must not produce apparently valid measurements");
    sample.Reset();
    MFShutdown();
    std::printf("latency reader: %d failures\n", failures);
    return failures ? 1 : 0;
}
