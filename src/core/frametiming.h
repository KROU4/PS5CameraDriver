#pragma once

#include <cstdint>
#include <optional>

namespace ps5cam {

struct FrameTiming {
    uint64_t deviceTime = 0;
    uint64_t ingressTime = 0;
    uint64_t sequence = 0;
    bool hasDeviceTime = false;
    bool hasIngressTime = false;
    bool hasSequence = false;
};

// The compute pipeline is one frame deep. Associate each completed image with the metadata of the
// input that produced it, rather than the input that happened to trigger its readback.
class DelayedFrameTiming {
public:
    std::optional<FrameTiming> Advance(const FrameTiming& current)
    {
        std::optional<FrameTiming> completed = m_previous;
        m_previous = current;
        return completed;
    }
    void Reset() { m_previous.reset(); }

private:
    std::optional<FrameTiming> m_previous;
};

// Counts ready frames replaced while no consumer was available. The count is attached to the next
// sample actually delivered, then cleared.
class LatestFrameState {
public:
    bool Replace(bool hadPending)
    {
        if (hadPending) ++m_dropped;
        return hadPending;
    }
    uint64_t TakeDropped()
    {
        const uint64_t dropped = m_dropped;
        m_dropped = 0;
        return dropped;
    }
    void Reset() { m_dropped = 0; }

private:
    uint64_t m_dropped = 0;
};

}  // namespace ps5cam
