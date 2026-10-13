// Camera-independent GPU regression for temporal denoise quality.  The synthetic clips run
// through StereoPipeline itself, so this exercises the compiled D3D11/Vulkan shader rather than
// a CPU copy of its algorithm.
#include "pipeline.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <tuple>
#include <utility>
#include <vector>

using namespace ps5cam;

namespace {

constexpr uint32_t kWidth = 320;
constexpr uint32_t kHeight = 192;
constexpr uint32_t kFrames = 48;
constexpr uint32_t kCrop = 24;

enum class Scene { Flat, Texture, Edge, Correlated, HeterogeneousNoise };

// A fixed generator and Box-Muller transform keep the fixture reproducible without depending on
// the implementation-defined sequence of std::normal_distribution.
class GaussianNoise {
public:
    explicit GaussianNoise(uint64_t seed) : m_state(seed) {}

    double Next()
    {
        if (m_haveSpare) {
            m_haveSpare = false;
            return m_spare;
        }
        double u1 = 0;
        do {
            u1 = Uniform();
        } while (u1 <= std::numeric_limits<double>::min());
        const double u2 = Uniform();
        const double magnitude = std::sqrt(-2.0 * std::log(u1));
        const double angle = 6.28318530717958647692 * u2;
        m_spare = magnitude * std::sin(angle);
        m_haveSpare = true;
        return magnitude * std::cos(angle);
    }

private:
    uint32_t NextUint()
    {
        const uint64_t old = m_state;
        m_state = old * 6364136223846793005ULL + 1442695040888963407ULL;
        const uint32_t shifted = static_cast<uint32_t>(((old >> 18u) ^ old) >> 27u);
        const uint32_t rotation = static_cast<uint32_t>(old >> 59u);
        return (shifted >> rotation) | (shifted << ((0u - rotation) & 31u));
    }

    double Uniform()
    {
        const uint64_t bits = (uint64_t(NextUint()) << 21u) ^ (NextUint() & 0x1FFFFFu);
        return (double(bits) + 0.5) / 9007199254740992.0;
    }

    uint64_t m_state;
    bool m_haveSpare = false;
    double m_spare = 0;
};

float Studio(float y)
{
    return 16.0f + y * (219.0f / 255.0f);
}

struct Clip {
    std::vector<std::vector<uint8_t>> packed;
    std::vector<std::vector<float>> truth;
};

uint64_t SceneSeed(Scene scene)
{
    return 0x50534341ULL + static_cast<uint64_t>(scene);
}

Clip MakeClip(Scene scene)
{
    Clip clip;
    clip.packed.reserve(kFrames);
    clip.truth.reserve(kFrames);
    GaussianNoise noise(SceneSeed(scene));
    const uint32_t pitch = kWidth * 4;  // Two side-by-side YUY2 eyes.

    for (uint32_t frameIndex = 0; frameIndex < kFrames; ++frameIndex) {
        std::vector<double> correlated;
        if (scene == Scene::Correlated || scene == Scene::HeterogeneousNoise) {
            correlated.resize(size_t(kWidth + 4) * (kHeight + 4));
            for (double& value : correlated) value = noise.Next();
        }
        std::vector<uint8_t> luma(size_t(kWidth) * kHeight);
        std::vector<float> truth(size_t(kWidth) * kHeight);
        for (uint32_t y = 0; y < kHeight; ++y) {
            for (uint32_t x = 0; x < kWidth; ++x) {
                float level = 112.0f;
                if (scene == Scene::Texture && x >= kWidth / 3 && x < 2 * kWidth / 3) {
                    const int bar = (((x + frameIndex) / 2) & 1) ? 1 : -1;
                    level += 15.0f * float(bar);
                } else if (scene == Scene::Edge) {
                    const uint32_t edge = kWidth / 2 + frameIndex % 12 - 6;
                    level = x < edge ? 78.0f : 154.0f;
                }
                const size_t index = size_t(y) * kWidth + x;
                truth[index] = Studio(level);
                double sampleNoise = 0;
                if (scene == Scene::HeterogeneousNoise && (x < kWidth / 3 || x >= 2 * kWidth / 3)) {
                    sampleNoise = noise.Next() * 3.0;
                } else if (scene == Scene::Correlated || scene == Scene::HeterogeneousNoise) {
                    // Binomial 5x5 correlation, normalized to the same pixel sigma=10 as white
                    // noise. Independent frames reproduce ISP-sized blobs without real motion.
                    constexpr int kernel[5] = {1, 4, 6, 4, 1};
                    for (uint32_t dy = 0; dy < 5; ++dy)
                        for (uint32_t dx = 0; dx < 5; ++dx)
                            sampleNoise += kernel[dy] * kernel[dx] *
                                correlated[size_t(y + dy) * (kWidth + 4) + x + dx] / 7.0;
                } else {
                    sampleNoise = noise.Next() * 10.0;
                }
                const long sample = std::lround(level + sampleNoise);
                luma[index] = static_cast<uint8_t>(std::clamp(sample, 0L, 255L));
            }
        }

        std::vector<uint8_t> packed(size_t(pitch) * kHeight);
        for (uint32_t y = 0; y < kHeight; ++y) {
            for (uint32_t eye = 0; eye < 2; ++eye) {
                uint8_t* row = packed.data() + size_t(y) * pitch + eye * kWidth * 2;
                const uint8_t* source = luma.data() + size_t(y) * kWidth;
                for (uint32_t x = 0; x < kWidth; x += 2) {
                    row[x * 2] = source[x];
                    row[x * 2 + 1] = 128;
                    row[x * 2 + 2] = source[x + 1];
                    row[x * 2 + 3] = 128;
                }
            }
        }
        clip.packed.push_back(std::move(packed));
        clip.truth.push_back(std::move(truth));
    }
    return clip;
}

struct ProcessedClip {
    std::vector<std::vector<uint8_t>> output;
    std::vector<std::vector<float>> truth;
};

bool ProcessClip(const Clip& clip, bool motionCompensation, ProcessedClip& processed,
                 bool replayAfterReset = false, bool fineDetail = true)
{
    StereoFormat stereo;
    stereo.eyeWidth = kWidth;
    stereo.eyeHeight = kHeight;
    OutputFormat output;
    output.width = kWidth;
    output.height = kHeight;
    output.format = PixelFormat::NV12;
    output.cropX = output.cropY = 0;
    output.cropW = float(kWidth);
    output.cropH = float(kHeight);

    StereoPipeline pipeline;
    const HRESULT initialized = pipeline.Initialize(stereo, output);
    if (FAILED(initialized)) {
        std::fprintf(stderr, "Initialize failed: 0x%08X\n", static_cast<unsigned>(initialized));
        return false;
    }
    pipeline.SetRectification({0, 0, 0, 0});
    std::printf("GPU: %s; motion %d\n", pipeline.GpuName().c_str(), motionCompensation ? 1 : 0);

    EffectSettings settings;
    settings.mode = ViewMode::Main;
    settings.autoBrightness = false;
    settings.denoise = 1.0f;
    settings.sharpen = 0.0f;
    settings.motionCompensation = motionCompensation;
    settings.fineDetail = fineDetail;
    std::vector<uint8_t> nv12(size_t(kWidth) * kHeight * 3 / 2);

    for (uint32_t call = 0; call < kFrames * (replayAfterReset ? 2u : 1u); ++call) {
        const uint32_t frameIndex = call % kFrames;
        if (call == kFrames) pipeline.Reset();
        const HRESULT result = pipeline.Process(clip.packed[frameIndex].data(), kWidth * 4, settings, nv12.data(),
            nv12.data() + size_t(kWidth) * kHeight, kWidth);
        if (frameIndex == 0) {
            if (result != S_FALSE) {
                std::fprintf(stderr, "first Process returned 0x%08X instead of S_FALSE\n", static_cast<unsigned>(result));
                return false;
            }
            continue;
        }
        if (result != S_OK) {
            std::fprintf(stderr, "Process frame %u failed: 0x%08X\n", frameIndex, static_cast<unsigned>(result));
            return false;
        }
        if (call >= kFrames) {
            // Reset must discard temporal luma and fine-motion confidence even though GPU images
            // retain old contents. Replaying a seeded clip must reproduce a fresh stream exactly.
            const auto& expected = processed.output[frameIndex - 1];
            if (!std::equal(expected.begin(), expected.end(), nv12.begin())) {
                std::fprintf(stderr, "Reset changed luma at frame %u (motion %d, fine detail %d)\n",
                    frameIndex, motionCompensation ? 1 : 0, fineDetail ? 1 : 0);
                return false;
            }
            continue;
        }
        processed.output.emplace_back(nv12.begin(), nv12.begin() + size_t(kWidth) * kHeight);
        // Process is explicitly one frame deep: output from call n belongs to input n - 1.
        processed.truth.push_back(clip.truth[frameIndex - 1]);
    }
    return processed.output.size() == kFrames - 1 && processed.truth.size() == processed.output.size();
}

struct Metrics {
    double flatRms = std::numeric_limits<double>::quiet_NaN();
    double textureGain = std::numeric_limits<double>::quiet_NaN();
    double textureResidual = std::numeric_limits<double>::quiet_NaN();
    double edgeGain = std::numeric_limits<double>::quiet_NaN();
    double edgeOvershoot = std::numeric_limits<double>::quiet_NaN();
};

double FlatRms(const ProcessedClip& clip)
{
    double squared = 0;
    uint64_t count = 0;
    for (size_t frame = 16; frame < clip.output.size(); ++frame) {
        double mean = 0;
        uint64_t frameCount = 0;
        for (uint32_t y = kCrop; y < kHeight - kCrop; ++y) {
            for (uint32_t x = kCrop; x < kWidth - kCrop; ++x) {
                const size_t pixel = size_t(y) * kWidth + x;
                mean += double(clip.output[frame][pixel]) - clip.truth[frame][pixel];
                ++frameCount;
            }
        }
        mean /= double(frameCount);
        for (uint32_t y = kCrop; y < kHeight - kCrop; ++y) {
            for (uint32_t x = kCrop; x < kWidth - kCrop; ++x) {
                const size_t pixel = size_t(y) * kWidth + x;
                const double delta = double(clip.output[frame][pixel]) - clip.truth[frame][pixel] - mean;
                squared += delta * delta;
                ++count;
            }
        }
    }
    return std::sqrt(squared / double(count));
}

std::pair<double, double> TextureMetrics(const ProcessedClip& clip)
{
    double covariance = 0;
    double referencePower = 0;
    for (size_t frame = 12; frame < clip.output.size(); ++frame) {
        for (uint32_t y = kCrop; y < kHeight - kCrop; ++y) {
            double outputMean = 0;
            double truthMean = 0;
            for (uint32_t x = kCrop; x < kWidth - kCrop; ++x) {
                const size_t pixel = size_t(y) * kWidth + x;
                outputMean += clip.output[frame][pixel];
                truthMean += clip.truth[frame][pixel];
            }
            const double width = kWidth - 2 * kCrop;
            outputMean /= width;
            truthMean /= width;
            for (uint32_t x = kCrop; x < kWidth - kCrop; ++x) {
                const size_t pixel = size_t(y) * kWidth + x;
                const double out = double(clip.output[frame][pixel]) - outputMean;
                const double reference = clip.truth[frame][pixel] - truthMean;
                covariance += out * reference;
                referencePower += reference * reference;
            }
        }
    }
    const double gain = covariance / referencePower;
    double residualPower = 0;
    uint64_t count = 0;
    for (size_t frame = 12; frame < clip.output.size(); ++frame) {
        for (uint32_t y = kCrop; y < kHeight - kCrop; ++y) {
            double outputMean = 0;
            double truthMean = 0;
            for (uint32_t x = kCrop; x < kWidth - kCrop; ++x) {
                const size_t pixel = size_t(y) * kWidth + x;
                outputMean += clip.output[frame][pixel];
                truthMean += clip.truth[frame][pixel];
            }
            const double width = kWidth - 2 * kCrop;
            outputMean /= width;
            truthMean /= width;
            for (uint32_t x = kCrop; x < kWidth - kCrop; ++x) {
                const size_t pixel = size_t(y) * kWidth + x;
                const double out = double(clip.output[frame][pixel]) - outputMean;
                const double reference = clip.truth[frame][pixel] - truthMean;
                const double residual = out - gain * reference;
                residualPower += residual * residual;
                ++count;
            }
        }
    }
    return {gain, std::sqrt(residualPower / double(count))};
}

std::pair<double, double> EdgeMetrics(const ProcessedClip& clip)
{
    double outputLow = 0, outputHigh = 0, expectedLow = 0, expectedHigh = 0;
    uint64_t lowCount = 0, highCount = 0;
    double profileMinimum = std::numeric_limits<double>::infinity();
    double profileMaximum = -std::numeric_limits<double>::infinity();
    for (size_t frame = 12; frame < clip.output.size(); ++frame) {
        for (uint32_t y = kCrop; y < kHeight - kCrop; ++y) {
            for (uint32_t x = kCrop; x < kWidth - kCrop; ++x) {
                const size_t pixel = size_t(y) * kWidth + x;
                if (clip.truth[frame][pixel] < 100.0f) {
                    outputLow += clip.output[frame][pixel];
                    expectedLow += clip.truth[frame][pixel];
                    ++lowCount;
                } else if (clip.truth[frame][pixel] > 130.0f) {
                    outputHigh += clip.output[frame][pixel];
                    expectedHigh += clip.truth[frame][pixel];
                    ++highCount;
                }
            }
        }
        for (uint32_t x = kCrop; x < kWidth - kCrop; ++x) {
            double profile = 0;
            for (uint32_t y = kCrop; y < kHeight - kCrop; ++y)
                profile += clip.output[frame][size_t(y) * kWidth + x];
            profile /= kHeight - 2 * kCrop;
            profileMinimum = std::min(profileMinimum, profile);
            profileMaximum = std::max(profileMaximum, profile);
        }
    }
    outputLow /= double(lowCount);
    outputHigh /= double(highCount);
    expectedLow /= double(lowCount);
    expectedHigh /= double(highCount);
    const double gain = (outputHigh - outputLow) / (expectedHigh - expectedLow);
    const double overshoot = std::max({0.0, expectedLow - profileMinimum, profileMaximum - expectedHigh});
    return {gain, overshoot};
}

bool Run(bool motionCompensation)
{
    ProcessedClip flat, texture, edge;
    if (!ProcessClip(MakeClip(Scene::Flat), motionCompensation, flat) ||
        !ProcessClip(MakeClip(Scene::Texture), motionCompensation, texture) ||
        !ProcessClip(MakeClip(Scene::Edge), motionCompensation, edge)) {
        return false;
    }

    Metrics metrics;
    metrics.flatRms = FlatRms(flat);
    std::tie(metrics.textureGain, metrics.textureResidual) = TextureMetrics(texture);
    std::tie(metrics.edgeGain, metrics.edgeOvershoot) = EdgeMetrics(edge);
    std::printf("motion %d; flat RMS %.3f; texture gain %.3f, residual %.3f; edge gain %.3f, overshoot %.3f\n",
        motionCompensation ? 1 : 0, metrics.flatRms, metrics.textureGain, metrics.textureResidual,
        metrics.edgeGain, metrics.edgeOvershoot);

    bool passed = true;
    auto require = [&](bool condition, const char* message, double value, double limit) {
        if (!condition) {
            std::fprintf(stderr, "motion %d: ", motionCompensation ? 1 : 0);
            std::fprintf(stderr, message, value, limit);
            std::fputc('\n', stderr);
            passed = false;
        }
    };
    require(std::isfinite(metrics.flatRms), "flat RMS %.3f is not finite (limit %.2f)", metrics.flatRms, 2.65);
    require(std::isfinite(metrics.textureGain), "texture gain %.3f is not finite (limit %.2f)", metrics.textureGain, 0.80);
    require(std::isfinite(metrics.textureResidual), "texture residual %.3f is not finite (limit %.2f)", metrics.textureResidual, 7.0);
    require(std::isfinite(metrics.edgeGain), "edge gain %.3f is not finite (limit %.2f)", metrics.edgeGain, 0.94);
    require(std::isfinite(metrics.edgeOvershoot), "edge overshoot %.3f is not finite (limit %.2f)", metrics.edgeOvershoot, 2.0);
    if (!passed) return false;
    require(metrics.flatRms <= 2.65, "settled flat-field RMS %.3f exceeds %.2f studio levels", metrics.flatRms, 2.65);
    require(metrics.textureGain >= 0.80, "moving fine-texture gain %.3f is below %.2f", metrics.textureGain, 0.80);
    require(metrics.textureResidual <= 7.0, "moving texture residual %.3f exceeds %.2f studio levels",
        metrics.textureResidual, 7.0);
    require(metrics.edgeGain >= 0.94, "moving edge gain %.3f is below %.2f", metrics.edgeGain, 0.94);
    require(metrics.edgeOvershoot <= 2.0, "moving edge overshoot %.3f exceeds %.2f studio levels",
        metrics.edgeOvershoot, 2.0);
    return passed;
}

double FlatFlicker(const ProcessedClip& clip)
{
    double sum = 0;
    uint64_t count = 0;
    for (size_t frame = 17; frame < clip.output.size(); ++frame)
        for (uint32_t y = kCrop; y < kHeight - kCrop; ++y)
            for (uint32_t x = kCrop; x < kWidth - kCrop; ++x) {
                const size_t p = size_t(y) * kWidth + x;
                sum += std::abs(int(clip.output[frame][p]) - int(clip.output[frame - 1][p]));
                ++count;
            }
    return sum / double(count);
}

bool CorrelatedNoise(bool motion, Scene scene)
{
    const Clip fixture = MakeClip(scene);
    ProcessedClip normal, detail;
    if (!ProcessClip(fixture, motion, normal, false, false) ||
        !ProcessClip(fixture, motion, detail)) return false;
    const double normalRms = FlatRms(normal), detailRms = FlatRms(detail);
    const double normalFlicker = FlatFlicker(normal), detailFlicker = FlatFlicker(detail);
    std::printf("motion %d; %s noise RMS %.3f -> %.3f; flicker %.3f -> %.3f\n",
        motion ? 1 : 0, scene == Scene::Correlated ? "correlated" : "heterogeneous",
        normalRms, detailRms, normalFlicker, detailFlicker);
    const bool passed = std::isfinite(detailRms) && std::isfinite(detailFlicker) &&
        detailRms <= normalRms * 1.05 && detailFlicker <= normalFlicker * 1.05;
    if (!passed) std::fprintf(stderr, "motion %d: correlated noise exceeds 5%% regression limit\n", motion ? 1 : 0);
    return passed;
}

}  // namespace

int main()
{
    const bool withoutMotion = Run(false);
    const bool withMotion = Run(true);
    const bool correlatedWithoutMotion = CorrelatedNoise(false, Scene::Correlated);
    const bool correlatedWithMotion = CorrelatedNoise(true, Scene::Correlated);
    const bool heterogeneousWithoutMotion = CorrelatedNoise(false, Scene::HeterogeneousNoise);
    const bool heterogeneousWithMotion = CorrelatedNoise(true, Scene::HeterogeneousNoise);
    bool resetPassed = true;
    const Clip resetClip = MakeClip(Scene::Texture);
    for (bool motion : {false, true}) {
        for (bool fineDetail : {false, true}) {
            ProcessedClip replay;
            if (!ProcessClip(resetClip, motion, replay, true, fineDetail)) resetPassed = false;
        }
    }
    std::printf("denoise history reset: %s\n", resetPassed ? "PASS" : "FAIL");
    const bool passed = withoutMotion && withMotion && correlatedWithoutMotion && correlatedWithMotion &&
        heterogeneousWithoutMotion && heterogeneousWithMotion && resetPassed;
    std::printf("denoise GPU quality: %s\n", passed ? "PASS" : "FAIL");
    return passed ? 0 : 1;
}
