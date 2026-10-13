// Camera-independent GPU regression for stereo-noise balancing and depth matching.
// Synthetic side-by-side YUY2 runs through StereoPipeline and the compiled GPU shaders.
#include "pipeline.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
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

constexpr uint32_t kLayeredFrames = 40;
constexpr uint32_t kBackgroundDisparity = 4;
constexpr uint32_t kForegroundDisparity = 16;
constexpr uint32_t kMoveFrame = 24;

Clip MakeLayeredClip()
{
    Random textureRandom(0x4c4159455253ULL);
    std::vector<uint8_t> background(size_t(kWidth + kBackgroundDisparity) * kHeight);
    std::vector<uint8_t> foreground(size_t(96) * 84);
    for (uint8_t& value : background)
        value = static_cast<uint8_t>(63u + textureRandom.Uint() % 45u);  // mean about 85
    for (uint8_t& value : foreground)
        value = static_cast<uint8_t>(133u + textureRandom.Uint() % 45u); // mean about 155

    Random mainNoise(0x4c4d41494eULL);
    Random secondNoise(0x4c5345434f4e44ULL);
    Clip clip;
    clip.packed.reserve(kLayeredFrames);
    const uint32_t pitch = kWidth * 4;
    for (uint32_t frame = 0; frame < kLayeredFrames; ++frame) {
        std::vector<uint8_t> main(size_t(kWidth) * kHeight);
        std::vector<uint8_t> second(size_t(kWidth) * kHeight);
        for (uint32_t y = 0; y < kHeight; ++y) {
            for (uint32_t x = 0; x < kWidth; ++x) {
                main[size_t(y) * kWidth + x] = background[size_t(y) * (kWidth + kBackgroundDisparity) + x];
                second[size_t(y) * kWidth + x] =
                    background[size_t(y) * (kWidth + kBackgroundDisparity) + x + kBackgroundDisparity];
            }
        }

        const uint32_t foregroundX = frame < kMoveFrame ? 80 : 120;
        for (uint32_t y = 32; y < 116; ++y) {
            for (uint32_t localX = 0; localX < 96; ++localX) {
                const uint32_t mainX = foregroundX + localX;
                const int secondX = int(mainX) - int(kForegroundDisparity);
                const uint8_t value = foreground[size_t(y - 32) * 96 + localX];
                main[size_t(y) * kWidth + mainX] = value;
                if (secondX >= 0 && secondX < int(kWidth))
                    second[size_t(y) * kWidth + uint32_t(secondX)] = value;
            }
        }

        std::vector<uint8_t> packed(size_t(pitch) * kHeight, 128);
        for (uint32_t y = 0; y < kHeight; ++y) {
            uint8_t* packedSecond = packed.data() + size_t(y) * pitch;
            uint8_t* packedMain = packedSecond + kWidth * 2;
            for (uint32_t x = 0; x < kWidth; ++x) {
                const int mainSample = int(main[size_t(y) * kWidth + x]) +
                    int(std::lround(mainNoise.Gaussian() * 3.0));
                const int secondSample = int(second[size_t(y) * kWidth + x]) +
                    int(std::lround(secondNoise.Gaussian() * 9.0));
                packedMain[x * 2] = static_cast<uint8_t>(std::clamp(mainSample, 0, 255));
                packedSecond[x * 2] = static_cast<uint8_t>(std::clamp(secondSample, 0, 255));
            }
        }
        clip.packed.push_back(std::move(packed));
    }
    return clip;
}

struct LayeredResult {
    std::vector<std::vector<uint8_t>> output;
};

bool ProcessLayered(const Clip& clip, const char* name, ViewMode mode,
    bool subjectEdges, bool backgroundSteady, LayeredResult& result)
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
    settings.mode = mode;
    settings.autoBrightness = false;
    settings.denoise = 1.0f;
    settings.sharpen = 0.0f;
    settings.subjectRange = 2.5f;
    settings.subjectEdges = subjectEdges;
    settings.backgroundSteady = backgroundSteady;

    result.output.resize(kLayeredFrames);
    std::vector<uint8_t> nv12(size_t(kWidth) * kHeight * 3 / 2);
    for (uint32_t call = 0; call < kLayeredFrames; ++call) {
        const HRESULT processed = pipeline.Process(clip.packed[call].data(), kWidth * 4, settings, nv12.data(),
            nv12.data() + size_t(kWidth) * kHeight, kWidth);
        if (call == 0) {
            if (processed != S_FALSE) {
                std::fprintf(stderr, "%s: first Process returned 0x%08X instead of S_FALSE\n",
                    name, static_cast<unsigned>(processed));
                return false;
            }
            continue;
        }
        if (processed != S_OK) {
            std::fprintf(stderr, "%s: Process frame %u failed: 0x%08X\n",
                name, call, static_cast<unsigned>(processed));
            return false;
        }
        // Process is one frame deep: call n delivers the depth for input n - 1.
        result.output[call - 1].assign(nv12.begin(), nv12.begin() + size_t(kWidth) * kHeight);
    }
    return true;
}

double DecodeDisparity(uint8_t level)
{
    return (double(level) - 16.0) * (64.0 / 219.0);
}

double RegionMae(const LayeredResult& result, uint32_t firstFrame, uint32_t lastFrame,
    uint32_t left, uint32_t right, uint32_t top, uint32_t bottom, double expected)
{
    double error = 0;
    uint64_t count = 0;
    for (uint32_t frame = firstFrame; frame <= lastFrame; ++frame) {
        for (uint32_t y = top; y < bottom; ++y) {
            for (uint32_t x = left; x < right; ++x) {
                error += std::abs(DecodeDisparity(result.output[frame][size_t(y) * kWidth + x]) - expected);
                ++count;
            }
        }
    }
    return error / double(count);
}

double BackgroundTemporalMad(const LayeredResult& result)
{
    double change = 0;
    uint64_t count = 0;
    for (uint32_t frame = 13; frame <= 22; ++frame) {
        for (uint32_t y = 20; y < 123; ++y) {
            for (uint32_t x = 24; x < 56; ++x) {
                const size_t pixel = size_t(y) * kWidth + x;
                change += std::abs(DecodeDisparity(result.output[frame][pixel]) -
                                   DecodeDisparity(result.output[frame - 1][pixel]));
                ++count;
            }
            for (uint32_t x = 224; x < 248; ++x) {
                const size_t pixel = size_t(y) * kWidth + x;
                change += std::abs(DecodeDisparity(result.output[frame][pixel]) -
                                   DecodeDisparity(result.output[frame - 1][pixel]));
                ++count;
            }
        }
    }
    return change / double(count);
}

double DecodeBlend(uint8_t level)
{
    return std::clamp((double(level) - 16.0) / 219.0, 0.0, 1.0);
}

struct BlendEdges {
    double erosion = 0; // Blur weight inside the foreground edge; zero is an intact sharp subject.
    double halo = 0;    // Sharp weight outside the edge; zero is a fully blurred background.
};

BlendEdges MeasureBlendEdges(const LayeredResult& result, uint32_t firstFrame, uint32_t lastFrame,
    uint32_t left, uint32_t right, uint32_t top, uint32_t bottom)
{
    double erosion = 0, halo = 0;
    uint64_t erosionCount = 0, haloCount = 0;
    auto add = [&](uint32_t frame, uint32_t x0, uint32_t x1, uint32_t y0, uint32_t y1,
                   bool foreground) {
        for (uint32_t y = y0; y < y1; ++y)
            for (uint32_t x = x0; x < x1; ++x) {
                const double blend = DecodeBlend(result.output[frame][size_t(y) * kWidth + x]);
                if (foreground) {
                    erosion += blend;
                    ++erosionCount;
                } else {
                    halo += 1.0 - blend;
                    ++haloCount;
                }
            }
    };
    for (uint32_t frame = firstFrame; frame <= lastFrame; ++frame) {
        // Four-to-eight-pixel bands stay clear of the guided-filter transition itself.
        add(frame, left + 4, left + 8, top + 8, bottom - 8, true);
        add(frame, right - 8, right - 4, top + 8, bottom - 8, true);
        add(frame, left + 8, right - 8, top + 4, top + 8, true);
        add(frame, left + 8, right - 8, bottom - 8, bottom - 4, true);
        add(frame, left - 8, left - 4, top, bottom, false);
        add(frame, right + 4, right + 8, top, bottom, false);
        add(frame, left, right, top - 8, top - 4, false);
        add(frame, left, right, bottom + 4, bottom + 8, false);
    }
    return {erosion / double(erosionCount), halo / double(haloCount)};
}

double BlendRegionError(const LayeredResult& result, uint32_t firstFrame, uint32_t lastFrame,
    uint32_t left, uint32_t right, uint32_t top, uint32_t bottom, double expected)
{
    double error = 0;
    uint64_t count = 0;
    for (uint32_t frame = firstFrame; frame <= lastFrame; ++frame)
        for (uint32_t y = top; y < bottom; ++y)
            for (uint32_t x = left; x < right; ++x) {
                error += std::abs(DecodeBlend(result.output[frame][size_t(y) * kWidth + x]) - expected);
                ++count;
            }
    return error / double(count);
}

bool LayeredRegression(bool baselineAsCandidate)
{
    const Clip clip = MakeLayeredClip();
    LayeredResult depthBaseline, depthCandidate, blendBaseline, blendCandidate;
    if (!ProcessLayered(clip, "layered-depth-baseline", ViewMode::DebugGrey, false, false, depthBaseline) ||
        !ProcessLayered(clip, "layered-depth-candidate", ViewMode::DebugGrey, false,
            !baselineAsCandidate, depthCandidate) ||
        !ProcessLayered(clip, "layered-blend-baseline", ViewMode::DebugBlend, false, false, blendBaseline) ||
        !ProcessLayered(clip, "layered-blend-candidate", ViewMode::DebugBlend,
            !baselineAsCandidate, false, blendCandidate))
        return false;

    const double baselineBackgroundMad = BackgroundTemporalMad(depthBaseline);
    const double candidateBackgroundMad = BackgroundTemporalMad(depthCandidate);
    const double baselineForegroundMae = RegionMae(depthBaseline, 12, 22, 88, 168, 40, 108, kForegroundDisparity);
    const double candidateForegroundMae = RegionMae(depthCandidate, 12, 22, 88, 168, 40, 108, kForegroundDisparity);
    const double clearedOldMae = RegionMae(depthCandidate, 28, 38, 88, 112, 40, 108, kBackgroundDisparity);
    const double movedForegroundMae = RegionMae(depthCandidate, 28, 38, 128, 208, 40, 108, kForegroundDisparity);
    const BlendEdges baselineEdges = MeasureBlendEdges(blendBaseline, 12, 22, 80, 176, 32, 116);
    const BlendEdges candidateEdges = MeasureBlendEdges(blendCandidate, 12, 22, 80, 176, 32, 116);
    const double baselineClearedBlend = BlendRegionError(blendBaseline, 28, 38, 88, 112, 40, 108, 1.0);
    const double candidateClearedBlend = BlendRegionError(blendCandidate, 28, 38, 88, 112, 40, 108, 1.0);
    const double baselineMovedBlend = BlendRegionError(blendBaseline, 28, 38, 128, 208, 40, 108, 0.0);
    const double candidateMovedBlend = BlendRegionError(blendCandidate, 28, 38, 128, 208, 40, 108, 0.0);
    std::printf("layered; background MAD %.3f -> %.3f; foreground MAE %.3f -> %.3f; "
                "after move old/new MAE %.3f/%.3f; blend erosion %.4f -> %.4f, halo %.4f -> %.4f; "
                "move blend old %.4f -> %.4f, new %.4f -> %.4f\n",
        baselineBackgroundMad, candidateBackgroundMad, baselineForegroundMae, candidateForegroundMae,
        clearedOldMae, movedForegroundMae, baselineEdges.erosion, candidateEdges.erosion,
        baselineEdges.halo, candidateEdges.halo, baselineClearedBlend, candidateClearedBlend,
        baselineMovedBlend, candidateMovedBlend);

    const double temporalLimit = baselineAsCandidate ? baselineBackgroundMad * 0.95 : baselineBackgroundMad * 1.05;
    const bool temporalPassed = baselineAsCandidate ? candidateBackgroundMad < temporalLimit :
        candidateBackgroundMad <= temporalLimit;
    constexpr double kBlendQuantization = 1.0 / 219.0;
    const bool edgePassed = candidateEdges.erosion <= (std::max)(baselineEdges.erosion * 1.05, kBlendQuantization) &&
        candidateEdges.halo <= (std::max)(baselineEdges.halo * 1.05, kBlendQuantization) &&
        candidateClearedBlend <= (std::max)(baselineClearedBlend * 1.05, kBlendQuantization) &&
        candidateMovedBlend <= (std::max)(baselineMovedBlend * 1.05, kBlendQuantization);
    const bool passed = std::isfinite(baselineBackgroundMad) && std::isfinite(candidateBackgroundMad) &&
        baselineForegroundMae <= 1.0 && candidateForegroundMae <= 1.0 &&
        temporalPassed && clearedOldMae <= 1.0 && movedForegroundMae <= 1.0 && edgePassed;
    if (!passed) {
        std::fprintf(stderr, "layered: depth stability or large-motion regression limit failed%s\n",
            baselineAsCandidate ? " (expected red-gate mode)" : "");
    }
    return passed;
}

}  // namespace

int main(int argc, char** argv)
{
    const bool baselineAsCandidate = argc == 2 && std::strcmp(argv[1], "--baseline-as-candidate") == 0;
    const Clip clip = MakeClip();
    const bool defaults = RunCase(clip, "defaults", false, false, false);
    const bool fourDirections = RunCase(clip, "balanced-4dir", true, true, false);
    const bool eightDirections = RunCase(clip, "balanced-8dir", true, true, true);
    const bool layered = LayeredRegression(baselineAsCandidate);
    const bool passed = defaults && fourDirections && eightDirections && layered;
    std::printf("stereo GPU quality: %s\n", passed ? "PASS" : "FAIL");
    return passed ? 0 : 1;
}
