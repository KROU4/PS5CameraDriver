#pragma once
#include <mfidl.h>
#include <optional>
#include "ids.h"
#include "../core/latencystats.h"

namespace ps5cam {
struct SampleLatency {
    std::optional<uint64_t> deviceTime, ingressTime, processedTime, deliveredTime, sequence, dropped;
    std::optional<double> deviceToApp, driverToApp, processingSpan, queue, transport, gpuMs;
};

inline SampleLatency ReadLatency(IMFSample* sample, uint64_t arrival)
{
    SampleLatency result;
    if (!sample) return result;
    auto read = [&](const GUID& key) -> std::optional<uint64_t> {
        UINT64 value = 0;
        if (FAILED(sample->GetUINT64(key, &value))) return std::nullopt;
        return value;
    };
    result.deviceTime = read(MFSampleExtension_DeviceTimestamp);
    result.ingressTime = read(kFrameIngressTime);
    result.processedTime = read(kFrameProcessedTime);
    result.deliveredTime = read(kFrameDeliveredTime);
    result.sequence = read(kFrameSequence);
    result.dropped = read(kDroppedFrames);
    auto elapsed = [&](std::optional<uint64_t> begin, std::optional<uint64_t> end) {
        if (!begin || !end || *end > arrival) return std::optional<double>{};
        return latency::ElapsedMs(*begin, *end);
    };
    result.deviceToApp = elapsed(result.deviceTime, arrival);
    result.driverToApp = elapsed(result.ingressTime, arrival);
    result.processingSpan = elapsed(result.ingressTime, result.processedTime);
    result.queue = elapsed(result.processedTime, result.deliveredTime);
    result.transport = elapsed(result.deliveredTime, arrival);
    if (auto gpu = read(kFrameGpuTimeUs)) result.gpuMs = double(*gpu) / 1000.0;
    return result;
}
}  // namespace ps5cam
