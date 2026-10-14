#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

namespace ps5cam::latency {

// Both boundaries are QPC-correlated timestamps expressed in 100ns units.
// Stream-relative sample presentation times must not be passed here.
inline std::optional<double> ElapsedMs(uint64_t begin, uint64_t end)
{
    if (end < begin) return std::nullopt;
    return double(end - begin) / 10000.0;
}

struct Summary {
    size_t count;
    double mean, p50, p95, maximum;
};

class Series {
public:
    bool Add(double value)
    {
        if (!std::isfinite(value) || value < 0) return false;
        m_values.push_back(value);
        return true;
    }
    std::optional<Summary> Summarize() const
    {
        if (m_values.empty()) return std::nullopt;
        auto sorted = m_values;
        std::sort(sorted.begin(), sorted.end());
        double total = 0;
        for (double value : sorted) total += value;
        // Nearest-rank percentiles; an absent metric stays absent rather than
        // being silently included as a zero in a percentile or mean.
        const auto rank = [&](double fraction) {
            return sorted[size_t(std::ceil(sorted.size() * fraction)) - 1];
        };
        return Summary{sorted.size(), total / sorted.size(), rank(0.5), rank(0.95), sorted.back()};
    }
private:
    std::vector<double> m_values;
};
}  // namespace ps5cam::latency
