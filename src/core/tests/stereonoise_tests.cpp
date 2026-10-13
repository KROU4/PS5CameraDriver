#include "stereonoise.h"

#include <cmath>
#include <cstdio>
#include <cstdint>

using namespace ps5cam::stereonoise;
namespace {
int checks = 0, failures = 0;
void Check(bool ok, const char* expression, int line)
{
    ++checks;
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "stereonoise_tests.cpp:%d: %s\n", line, expression);
    }
}
#define CHECK(expression) Check((expression), #expression, __LINE__)
bool Near(float a, float b, float tolerance = 0.000001f) { return std::fabs(a - b) <= tolerance; }

void DifferenceHistogram()
{
    uint32_t histogram[kHistogramBins] = {};
    AddDifference(0.25f + 0.6f * kHistogramBinWidth, 0.25f, histogram);
    AddDifference(0.25f - 0.6f * kHistogramBinWidth, 0.25f, histogram);
    AddDifference(1.0f, 0.0f, histogram);
    AddDifference(0.0f, 1.0f, histogram);
    CHECK(histogram[kHistogramCenter + 1] == 1);
    CHECK(histogram[kHistogramCenter - 1] == 1);
    CHECK(histogram[kHistogramBins - 1] == 1);
    CHECK(histogram[0] == 1);

    const float mainCurrent[] = {0.1f + kHistogramBinWidth, 0.2f};
    const float mainPrevious[] = {0.1f, 0.2f + kHistogramBinWidth};
    const float secondCurrent[] = {0.3f, 0.4f + 2.0f * kHistogramBinWidth};
    const float secondPrevious[] = {0.3f, 0.4f};
    uint32_t mainHistogram[kHistogramBins] = {};
    uint32_t secondHistogram[kHistogramBins] = {};
    AddStereoDifferences(mainCurrent, mainPrevious, secondCurrent, secondPrevious, 2,
        mainHistogram, secondHistogram);
    CHECK(mainHistogram[kHistogramCenter + 1] == 1);
    CHECK(mainHistogram[kHistogramCenter - 1] == 1);
    CHECK(secondHistogram[kHistogramCenter] == 1);
    CHECK(secondHistogram[kHistogramCenter + 2] == 1);
}

void RobustNoiseEstimate()
{
    uint32_t histogram[kHistogramBins] = {};
    histogram[kHistogramCenter + 2] = 33;
    histogram[kHistogramCenter + 3] = 31;
    histogram[kHistogramCenter + 4] = 1;
    histogram[kHistogramCenter + 5] = 31;
    histogram[kHistogramCenter + 6] = 33;
    const NoiseEstimate estimate = NoiseFromHistogram(histogram);
    CHECK(estimate.samples == 129);
    CHECK(Near(estimate.median, 4.0f / 4096.0f));
    CHECK(Near(estimate.mad, 2.0f / 4096.0f));
    CHECK(Near(estimate.sigma, 0.00051189f, 0.00000002f));

    uint32_t quantized[kHistogramBins] = {};
    quantized[kHistogramCenter] = kMinimumSamples;
    const NoiseEstimate floor = NoiseFromHistogram(quantized);
    CHECK(Near(floor.sigma, 0.00007048f, 0.00000002f));
}

void InvalidHistograms()
{
    uint32_t empty[kHistogramBins] = {};
    CHECK(NoiseFromHistogram(empty).sigma < 0.0f);
    empty[kHistogramCenter] = kMinimumSamples - 1;
    CHECK(NoiseFromHistogram(empty).sigma < 0.0f);

    uint32_t clipped[kHistogramBins] = {};
    clipped[0] = 16;
    clipped[kHistogramCenter] = 48;
    CHECK(NoiseFromHistogram(clipped).sigma < 0.0f);

    uint32_t sparseOutliers[kHistogramBins] = {};
    sparseOutliers[0] = 15;
    sparseOutliers[kHistogramCenter] = 49;
    CHECK(NoiseFromHistogram(sparseOutliers).sigma >= 0.0f);
}

void NoiseEqualization()
{
    CHECK(Near(BlurBlendVarianceRatio(0.0f), 1.0f));
    CHECK(Near(BlurBlendVarianceRatio(1.0f), 0.140625f));
    CHECK(Near(EqualizationAlpha(0.02f, 0.02f), 0.0f));
    CHECK(Near(EqualizationAlpha(0.02f, 0.01f), 0.72365f, 0.00001f));
    CHECK(Near(EqualizationAlpha(0.02f, 0.001f), 1.0f));
    CHECK(Near(EqualizationAlpha(-1.0f, 0.01f), 0.0f));
    CHECK(Near(BalancedSigma(0.02f, 1.0f), 0.0075f));
}

void TextureThresholds()
{
    CHECK(Near(TextureThreshold(0.001f, 0.002f), 0.012f));
    CHECK(Near(TextureThreshold(0.006f, 0.008f), 0.012f));
    CHECK(Near(TextureThreshold(0.020f, 0.010f), 0.024f));
    CHECK(Near(TextureThreshold(-1.0f, 0.010f), 0.012f));
}
}  // namespace

int main()
{
    DifferenceHistogram();
    RobustNoiseEstimate();
    InvalidHistograms();
    NoiseEqualization();
    TextureThresholds();
    std::printf("stereo noise: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
