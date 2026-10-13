// Camera-independent GPU regression for stereo-noise balancing and depth matching.
// Synthetic side-by-side YUY2 runs through StereoPipeline and the compiled GPU shaders.
#include "pipeline.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

using namespace ps5cam;

namespace {

constexpr uint32_t kWidth = 256;
constexpr uint32_t kHeight = 144;
constexpr uint32_t kDisparity = 8;
constexpr uint32_t kFrames = 14;

class Random {
public:
    explicit Random(uint64_t state) : m_state(state) {}

    uint32_t Uint()
    {
        const uint64_t old = m_state;
        m_state = old * 6364136223846793005ULL + 1442695040888963407ULL;
        const uint32_t shifted = static_cast<uint32_t>(((old >> 18u) ^ old) >> 27u);
        const uint32_t rotation = static_cast<uint32_t>(old >> 59u);
        return (shifted >> rotation) | (shifted << ((0u - rotation) & 31u));
    }

    double Gaussian()
    {
        if (m_haveSpare) {
            m_haveSpare = false;
            return m_spare;
        }
        double u1 = 0;
        do {
            u1 = (double(Uint()) + 0.5) / 4294967296.0;
        } while (u1 <= (std::numeric_limits<double>::min)());
        const double u2 = (double(Uint()) + 0.5) / 4294967296.0;
        const double magnitude = std::sqrt(-2.0 * std::log(u1));
        const double angle = 6.28318530717958647692 * u2;
        m_spare = magnitude * std::sin(angle);
        m_haveSpare = true;
        return magnitude * std::cos(angle);
    }

private:
    uint64_t m_state;
    bool m_haveSpare = false;
    double m_spare = 0;
};

struct Clip {
    std::vector<std::vector<uint8_t>> packed;
};

Clip MakeClip()
{
    Random textureRandom(0x53544552454fULL);
    std::vector<uint8_t> texture(size_t(kWidth + kDisparity) * kHeight);
    for (uint8_t& value : texture)
        value = static_cast<uint8_t>(48u + textureRandom.Uint() % 161u);

    Random mainNoise(0x4d41494eULL);
    Random secondNoise(0x5345434f4e44ULL);
    Clip clip;
    clip.packed.reserve(kFrames);
    const uint32_t pitch = kWidth * 4;
    for (uint32_t frame = 0; frame < kFrames; ++frame) {
        std::vector<uint8_t> packed(size_t(pitch) * kHeight, 128);
        const int drift = int(frame % 5) - 2;  // Median subtraction must reject exposure drift.
        for (uint32_t y = 0; y < kHeight; ++y) {
            uint8_t* second = packed.data() + size_t(y) * pitch;
            uint8_t* main = second + kWidth * 2;
            for (uint32_t x = 0; x < kWidth; ++x) {
                const int mainSample = int(texture[size_t(y) * (kWidth + kDisparity) + x]) + drift +
                    int(std::lround(mainNoise.Gaussian() * 3.0));
                const int secondSample = int(texture[size_t(y) * (kWidth + kDisparity) + x + kDisparity]) + drift +
                    int(std::lround(secondNoise.Gaussian() * 9.0));
                main[x * 2] = static_cast<uint8_t>(std::clamp(mainSample, 0, 255));
                second[x * 2] = static_cast<uint8_t>(std::clamp(secondSample, 0, 255));
            }
        }
        clip.packed.push_back(std::move(packed));
    }
    return clip;
}

struct Metrics {
    double coverage = 0;
    double meanAbsoluteError = 0;
    double noiseMain = 0;
    double noiseSecond = 0;
    double balanceMain = 0;
    double balanceSecond = 0;
    double textureThreshold = 0;
};

bool RunCase(const Clip& clip, const char* name, bool noiseBalance, bool adaptiveTexture, bool diagonals)
{
    StereoFormat stereo;
    stereo.eyeWidth = kWidth;
    stereo.eyeHeight = kHeight;
    stereo.mainIsSecondHalf = true;
    OutputFormat output;
    output.width = kWidth;
    output.height = kHeight;
    output.format = PixelFormat::NV12;
    output.cropW = float(kWidth);
    output.cropH = float(kHeight);

    StereoPipeline pipeline;
    const HRESULT initialized = pipeline.Initialize(stereo, output);
    if (FAILED(initialized)) {
        std::fprintf(stderr, "%s: Initialize failed: 0x%08X\n", name, static_cast<unsigned>(initialized));
        return false;
    }
    pipeline.SetRectification({0, 0, 0, 0});

    EffectSettings settings;
    settings.mode = ViewMode::DebugRawGrey;
    settings.autoBrightness = false;
    settings.denoise = 0.0f;
    settings.sharpen = 0.0f;
    settings.stereoNoiseBalance = noiseBalance;
    settings.adaptiveStereoTexture = adaptiveTexture;
    settings.stereoDiagonals = diagonals;

    std::vector<uint8_t> nv12(size_t(kWidth) * kHeight * 3 / 2);
    Metrics metrics;
    uint64_t valid = 0, pixels = 0;
    uint32_t measuredFrames = 0;
    constexpr uint32_t kMeasuredFrames = 4;
    for (uint32_t frame = 0; frame < kFrames; ++frame) {
        if (noiseBalance || adaptiveTexture) {
            settings.stereoNoiseBalance = noiseBalance && frame != 6;
            settings.adaptiveStereoTexture = adaptiveTexture && frame != 6;
        }
        FrameStats stats;
        const HRESULT result = pipeline.Process(clip.packed[frame].data(), kWidth * 4, settings, nv12.data(),
            nv12.data() + size_t(kWidth) * kHeight, kWidth, &stats);
        if (frame == 0) {
            if (result != S_FALSE) {
                std::fprintf(stderr, "%s: first Process returned 0x%08X instead of S_FALSE\n",
                    name, static_cast<unsigned>(result));
                return false;
            }
            continue;
        }
        if (result != S_OK) {
            std::fprintf(stderr, "%s: Process frame %u failed: 0x%08X\n",
                name, frame, static_cast<unsigned>(result));
            return false;
        }
        if ((noiseBalance || adaptiveTexture) && (frame == 7 || frame == 8) &&
            (stats.stereoBalanceMain != 0 || stats.stereoBalanceSecond != 0 ||
             stats.stereoNoiseMain != 0 || stats.stereoNoiseSecond != 0 ||
             std::abs(stats.stereoTextureThreshold - 0.012f) > 1e-6f)) {
            std::fprintf(stderr, "%s: old noise state survives a one-frame disable at call %u\n", name, frame);
            return false;
        }
        if (frame < kFrames - kMeasuredFrames) continue;
        ++measuredFrames;
        metrics.noiseMain += stats.stereoNoiseMain;
        metrics.noiseSecond += stats.stereoNoiseSecond;
        metrics.balanceMain += stats.stereoBalanceMain;
        metrics.balanceSecond += stats.stereoBalanceSecond;
        metrics.textureThreshold += stats.stereoTextureThreshold;
        for (uint32_t y = 8; y < kHeight - 8; ++y) {
            for (uint32_t x = 20; x < kWidth - 8; ++x) {
                ++pixels;
                const uint8_t level = nv12[size_t(y) * kWidth + x];
                if (level <= 16) continue;
                const double disparity = (double(level) - 16.0) * (64.0 / 219.0);
                metrics.meanAbsoluteError += std::abs(disparity - double(kDisparity));
                ++valid;
            }
        }
    }
    if (measuredFrames != kMeasuredFrames || !pixels || !valid) {
        std::fprintf(stderr, "%s: incomplete measurements\n", name);
        return false;
    }
    metrics.coverage = double(valid) / double(pixels);
    metrics.meanAbsoluteError /= double(valid);
    metrics.noiseMain /= measuredFrames;
    metrics.noiseSecond /= measuredFrames;
    metrics.balanceMain /= measuredFrames;
    metrics.balanceSecond /= measuredFrames;
    metrics.textureThreshold /= measuredFrames;

    std::printf("%s; coverage %.3f, depth MAE %.3f; noise %.5f/%.5f; balance %.3f/%.3f; texture %.5f\n",
        name, metrics.coverage, metrics.meanAbsoluteError, metrics.noiseMain, metrics.noiseSecond,
        metrics.balanceMain, metrics.balanceSecond, metrics.textureThreshold);
    bool passed = metrics.coverage >= 0.65 && metrics.meanAbsoluteError <= 1.0;
    if (noiseBalance || adaptiveTexture) {
        passed = passed && std::isfinite(metrics.noiseMain) && metrics.noiseMain > 0.0 &&
            std::isfinite(metrics.noiseSecond) && metrics.noiseSecond > metrics.noiseMain &&
            std::isfinite(metrics.balanceMain) && std::isfinite(metrics.balanceSecond) &&
            metrics.balanceSecond > metrics.balanceMain &&
            std::isfinite(metrics.textureThreshold) && metrics.textureThreshold >= 0.006 &&
            metrics.textureThreshold <= 0.024;
    }
    if (!passed) std::fprintf(stderr, "%s: stereo GPU regression limits failed\n", name);
    return passed;
}

}  // namespace

int main()
{
    const Clip clip = MakeClip();
    const bool defaults = RunCase(clip, "defaults", false, false, false);
    const bool fourDirections = RunCase(clip, "balanced-4dir", true, true, false);
    const bool eightDirections = RunCase(clip, "balanced-8dir", true, true, true);
    const bool passed = defaults && fourDirections && eightDirections;
    std::printf("stereo GPU quality: %s\n", passed ? "PASS" : "FAIL");
    return passed ? 0 : 1;
}
