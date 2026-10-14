#pragma once

#include <mfapi.h>
#include <mfidl.h>

#include "ids.h"
#include "../core/frametiming.h"

namespace ps5cam {

inline uint64_t FrameClockNow() { return static_cast<uint64_t>(MFGetSystemTime()); }

inline FrameTiming ReadFrameTiming(IMFSample* sample)
{
    FrameTiming result;
    if (!sample) return result;
    result.hasDeviceTime = SUCCEEDED(sample->GetUINT64(MFSampleExtension_DeviceTimestamp, &result.deviceTime));
    result.hasIngressTime = SUCCEEDED(sample->GetUINT64(kFrameIngressTime, &result.ingressTime));
    result.hasSequence = SUCCEEDED(sample->GetUINT64(kFrameSequence, &result.sequence));
    return result;
}

inline void StampIngress(IMFSample* sample, uint64_t time, uint64_t sequence)
{
    if (!sample) return;
    sample->SetUINT64(kFrameIngressTime, time);
    sample->SetUINT64(kFrameSequence, sequence);
}

inline void ApplyProcessedTiming(IMFSample* sample, const FrameTiming& timing, uint64_t processedTime)
{
    if (!sample) return;
    if (timing.hasDeviceTime) sample->SetUINT64(MFSampleExtension_DeviceTimestamp, timing.deviceTime);
    if (timing.hasIngressTime) sample->SetUINT64(kFrameIngressTime, timing.ingressTime);
    if (timing.hasSequence) sample->SetUINT64(kFrameSequence, timing.sequence);
    sample->SetUINT64(kFrameProcessedTime, processedTime);
}

inline void StampDelivery(IMFSample* sample, uint64_t time, uint64_t dropped)
{
    if (!sample) return;
    sample->SetUINT64(kFrameDeliveredTime, time);
    sample->SetUINT64(kDroppedFrames, dropped);
}

}  // namespace ps5cam
