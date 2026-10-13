#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace ps5cam::stereonoise {

constexpr size_t kHistogramBins = 512;
constexpr size_t kHistogramCenter = 256;
constexpr float kHistogramBinWidth = 1.0f / 4096.0f;
constexpr uint64_t kMinimumSamples = 64;
constexpr float kUnknownNoise = -1.0f;

struct NoiseEstimate {
    float median = kUnknownNoise;
    float mad = kUnknownNoise;
    float sigma = kUnknownNoise;
    uint64_t samples = 0;
};

// Bins signed normalized-luma differences at 1/4096. Values outside the
// represented range are accumulated in the two edge bins.
inline size_t DifferenceBin(float delta)
{
    if (!std::isfinite(delta)) return kHistogramCenter;
    const long offset = std::lround(delta / kHistogramBinWidth);
    return size_t(std::clamp<long>(long(kHistogramCenter) + offset, 0, long(kHistogramBins - 1)));
}

inline void AddDifference(float current, float previous, uint32_t (&histogram)[kHistogramBins])
{
    if (!std::isfinite(current) || !std::isfinite(previous)) return;
    uint32_t& count = histogram[DifferenceBin(current - previous)];
    if (count != std::numeric_limits<uint32_t>::max()) ++count;
}

// Both inputs must be consecutive raw work-resolution images. In particular,
// the main-camera estimate must not be replaced with the temporal denoiser's
// m_noise value: that quantity has different filtering and calibration.
inline void AddStereoDifferences(const float* mainCurrent, const float* mainPrevious,
    const float* secondCurrent, const float* secondPrevious, size_t count,
    uint32_t (&mainHistogram)[kHistogramBins], uint32_t (&secondHistogram)[kHistogramBins])
{
    if (!mainCurrent || !mainPrevious || !secondCurrent || !secondPrevious) return;
    for (size_t i = 0; i < count; ++i) {
        AddDifference(mainCurrent[i], mainPrevious[i], mainHistogram);
        AddDifference(secondCurrent[i], secondPrevious[i], secondHistogram);
    }
}

inline size_t WeightedMedianBin(const uint64_t* counts, size_t count, uint64_t total)
{
    const uint64_t target = (total - 1) / 2;
    uint64_t cumulative = 0;
    for (size_t i = 0; i < count; ++i) {
        cumulative += counts[i];
        if (cumulative > target) return i;
    }
    return count - 1;
}

// Converts the temporal-difference MAD to one-frame Gaussian sigma. This
// assumes independent white noise in consecutive frames. Spatial correlation,
// fixed-pattern noise, motion, and exposure changes can bias the estimate, so
// the median rejects signed drift and the MAD rejects sparse outliers. A
// histogram with at least 25% of samples clipped into its edge bins is invalid.
inline NoiseEstimate NoiseFromHistogram(const uint32_t* histogram)
{
    NoiseEstimate result;
    if (!histogram) return result;

    uint64_t counts[kHistogramBins] = {};
    for (size_t i = 0; i < kHistogramBins; ++i) {
        counts[i] = histogram[i];
        result.samples += counts[i];
    }
    if (result.samples < kMinimumSamples) return result;
    const uint64_t clipped = counts[0] + counts[kHistogramBins - 1];
    if (clipped * 4 >= result.samples) return result;

    const size_t medianBin = WeightedMedianBin(counts, kHistogramBins, result.samples);
    result.median = (float(medianBin) - float(kHistogramCenter)) * kHistogramBinWidth;

    uint64_t deviations[kHistogramBins] = {};
    for (size_t i = 0; i < kHistogramBins; ++i) {
        const size_t distance = i > medianBin ? i - medianBin : medianBin - i;
        deviations[distance] += counts[i];
    }
    const size_t madBins = WeightedMedianBin(deviations, kHistogramBins, result.samples);
    result.mad = float(madBins) * kHistogramBinWidth;

    constexpr float kGaussianMad = 0.67448975f;
    constexpr float kSqrtTwo = 1.41421356237f;
    constexpr float kSqrtTwelve = 3.46410161514f;
    const float robustSigma = result.mad / (kGaussianMad * kSqrtTwo);
    const float quantizationSigma = kHistogramBinWidth / kSqrtTwelve;
    result.sigma = std::max(robustSigma, quantizationSigma);
    return result;
}

// A separable [0.25, 0.5, 0.25] 3x3 blur blended by alpha with the original
// has this white-noise variance ratio. Real sensor noise can be spatially
// correlated, so the ratio is a calibration estimate rather than a guarantee.
inline float BlurBlendVarianceRatio(float alpha)
{
    alpha = std::clamp(alpha, 0.0f, 1.0f);
    return 1.0f - 1.5f * alpha + 0.640625f * alpha * alpha;
}

inline float EqualizationAlpha(float sensorSigma, float targetSigma)
{
    if (!std::isfinite(sensorSigma) || !std::isfinite(targetSigma) ||
        sensorSigma <= 0.0f || targetSigma <= 0.0f || sensorSigma <= targetSigma)
        return 0.0f;
    const float ratio = targetSigma / sensorSigma;
    const float discriminant = 2.25f - 2.5625f * (1.0f - ratio * ratio);
    if (discriminant <= 0.0f) return 1.0f;
    return std::clamp((1.5f - std::sqrt(discriminant)) / 1.28125f, 0.0f, 1.0f);
}

inline float BalancedSigma(float sensorSigma, float alpha)
{
    if (!std::isfinite(sensorSigma) || sensorSigma < 0.0f || !std::isfinite(alpha))
        return kUnknownNoise;
    return sensorSigma * std::sqrt(BlurBlendVarianceRatio(alpha));
}

// Inputs are the post-equalization per-frame sigmas of the two raw images.
inline float TextureThreshold(float balancedMainSigma, float balancedSecondSigma)
{
    if (!std::isfinite(balancedMainSigma) || !std::isfinite(balancedSecondSigma) ||
        balancedMainSigma < 0.0f || balancedSecondSigma < 0.0f)
        return 0.012f;
    // Do not admit lower-contrast matches than the validated fixed threshold: a low global
    // noise median can hide a noisy patch or periodic background and create near-depth islands.
    return std::clamp(1.5f * std::max(balancedMainSigma, balancedSecondSigma), 0.012f, 0.024f);
}

}  // namespace ps5cam::stereonoise
