// Mode selection and Media Foundation type contracts, without a camera or GPU.
#include "cameramodes.h"
#include <wrl/client.h>
#include <cmath>
#include <cstdio>

using namespace ps5cam;
using Microsoft::WRL::ComPtr;
namespace {
int checks = 0, failures = 0;
void Check(bool ok, const char* expression, int line)
{
    ++checks;
    if (!ok) { ++failures; std::fprintf(stderr, "cameramodes_tests.cpp:%d: %s\n", line, expression); }
}
#define CHECK(expression) Check((expression), #expression, __LINE__)
bool Near(float a, float b) { return std::fabs(a - b) < 0.001f; }

void Modes()
{
    for (uint32_t fps : {30u, 60u}) {
        const auto mono = SensorModes(fps, ViewMode::Main);
        CHECK(mono.size() == 3 && mono[0].layout == Layout::Mono && mono[0].fps == fps);
        for (ViewMode view : {ViewMode::Bokeh, ViewMode::Second, ViewMode::Depth, ViewMode::SideBySide}) {
            const auto stereo = SensorModes(fps, view);
            CHECK(stereo.size() == 2 && stereo[0].layout == Layout::HalfSecond && stereo[0].fps == fps);
            CHECK(stereo[1].packedW == (fps == 60 ? 2560u : 3840u));
            CHECK(stereo[1].packedH == (fps == 60 ? 800u : 1080u));
        }
        for (const auto& mode : mono) {
            SensorMode decoded{};
            CHECK(SensorModeOf(mode.packedW, mode.packedH, fps, &decoded));
            const auto format = FormatOf(decoded);
            CHECK(format.Valid() && format.PackedWidth() == mode.packedW && format.PackedHeight() == mode.packedH);
            if (decoded.layout == Layout::HalfSecond)
                CHECK(format.eyeWidth == 1920 && format.eyeHeight == 1080 && format.SecondWidth() == 960 && format.SecondHeight() == 540);
        }
    }
    SensorMode untouched{1, 2, 3, Layout::Mono, L"unchanged"};
    CHECK(!SensorModeOf(1234, 567, 60, &untouched));
    CHECK(untouched.packedW == 1 && untouched.packedH == 2);

    StereoFormat wide;
    wide.eyeWidth = 1280; wide.eyeHeight = 800;
    const auto cropped = OutputFor(wide, 1920, 1080, PixelFormat::NV12);
    CHECK(Near(cropped.cropW, 1280) && Near(cropped.cropH, 720));
    CHECK(Near(cropped.cropX, 0) && Near(cropped.cropY, 40));
    StereoFormat full;
    const auto square = OutputFor(full, 720, 720, PixelFormat::YUY2);
    CHECK(Near(square.cropX, 420) && Near(square.cropY, 0) && Near(square.cropW, 1080) && Near(square.cropH, 1080));
}

void Types()
{
    for (bool prefer60 : {false, true}) {
        const auto restricted = OutputTypes(prefer60, true);
        CHECK(restricted.size() == 2);
        CHECK(restricted[0].fmt == PixelFormat::NV12 && restricted[1].fmt == PixelFormat::YUY2);
        for (const auto& type : restricted) CHECK(type.w == 1920 && type.h == 1080 && type.fps == 60);
        const auto all = OutputTypes(prefer60, false);
        CHECK(all.size() == 8 && all[0].fps == (prefer60 ? 60u : 30u));
        for (const auto& spec : all) {
            ComPtr<IMFMediaType> media;
            const HRESULT hr = MakeVideoType(spec, &media);
            CHECK(SUCCEEDED(hr));
            if (FAILED(hr)) continue;
            UINT32 w = 0, h = 0, numerator = 0, denominator = 0, size = 0, stride = 0;
            GUID subtype{};
            CHECK(SUCCEEDED(MFGetAttributeSize(media.Get(), MF_MT_FRAME_SIZE, &w, &h)) && w == spec.w && h == spec.h);
            CHECK(SUCCEEDED(MFGetAttributeRatio(media.Get(), MF_MT_FRAME_RATE, &numerator, &denominator)));
            CHECK(numerator == 10000000 && denominator == (spec.fps == 60 ? 166666u : 333333u));
            CHECK(SUCCEEDED(media->GetGUID(MF_MT_SUBTYPE, &subtype)) && subtype == (spec.fmt == PixelFormat::YUY2 ? MFVideoFormat_YUY2 : MFVideoFormat_NV12));
            CHECK(SUCCEEDED(media->GetUINT32(MF_MT_SAMPLE_SIZE, &size)) && size == (spec.fmt == PixelFormat::YUY2 ? spec.w * spec.h * 2 : spec.w * spec.h * 3 / 2));
            CHECK(SUCCEEDED(media->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) && stride == (spec.fmt == PixelFormat::YUY2 ? spec.w * 2 : spec.w));
        }
    }
}
}
int main()
{
    const HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) { std::fprintf(stderr, "MFStartup failed: %08lx\n", static_cast<unsigned long>(hr)); return 1; }
    Modes(); Types();
    MFShutdown();
    std::printf("camera modes: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
