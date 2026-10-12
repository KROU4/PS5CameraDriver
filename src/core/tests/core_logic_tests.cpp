// Camera-independent contracts shared by the Windows and Linux pipelines.
#include "calibschedule.h"
#include "flicker.h"
#include "picture.h"

#include <cmath>
#include <cstdio>
#include <limits>

using namespace ps5cam;
namespace {
int checks = 0, failures = 0;
void Check(bool ok, const char* expression, int line)
{
    ++checks;
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "core_logic_tests.cpp:%d: %s\n", line, expression);
    }
}
#define CHECK(expression) Check((expression), #expression, __LINE__)
bool Near(float a, float b) { return std::fabs(a - b) < 0.00001f; }

void Calibration()
{
    for (uint32_t fps : {30u, 60u}) {
        CalibrationSchedule schedule;
        schedule.Start(false, 100);
        CHECK(!schedule.Pending() && !schedule.Due(10000));
        schedule.Request(100);
        CHECK(schedule.Pending() && !schedule.Due(109) && schedule.Due(110));
        schedule.Request(108);  // Repeated requests must not postpone an existing attempt.
        CHECK(schedule.Due(110));
        uint32_t frame = 110;
        for (uint32_t seconds : {1u, 2u, 4u, 8u, 16u, 16u}) {
            schedule.Failed(frame, fps);
            frame += seconds * fps;
            CHECK(!schedule.Due(frame - 1) && schedule.Due(frame));
        }
        CHECK(schedule.Failures() == 5);
        schedule.Succeeded();
        CHECK(!schedule.Pending() && schedule.Failures() == 0 && !schedule.Due(frame));
        schedule.Start(true, 200);
        CHECK(!schedule.Due(209) && schedule.Due(210));
    }
}

void Picture()
{
    PictureSettings picture;
    EffectSettings effect;
    ApplyPicture(picture, effect);
    CHECK(effect.mode == ViewMode::Bokeh && Near(effect.blurStrength, 0.25f));
    CHECK(Near(effect.brightness, 0) && Near(effect.contrast, 1) && Near(effect.saturation, 1));
    CHECK(Near(effect.denoise, 0.9f) && Near(effect.maxGain, 6));

    const uint32_t tooLarge = std::numeric_limits<uint32_t>::max();
    picture.mode = picture.blur = picture.focus = picture.highlights = picture.temporal = tooLarge;
    picture.maxGain = picture.denoise = picture.antiFlicker = picture.sharpen = tooLarge;
    picture.brightness = picture.contrast = picture.saturation = tooLarge;
    ClampPicture(picture);
    CHECK(picture.mode == 0 && picture.antiFlicker == 0);
    ApplyPicture(picture, effect);
    CHECK(Near(effect.blurStrength, 1) && Near(effect.manualFocus, 1));
    CHECK(Near(effect.highlights, 4) && Near(effect.temporal, 1) && Near(effect.maxGain, 16));
    CHECK(Near(effect.denoise, 1) && Near(effect.sharpen, 1));
    CHECK(Near(effect.brightness, 1) && Near(effect.contrast, 1.5f) && Near(effect.saturation, 2));

    picture.temporal = picture.maxGain = picture.brightness = picture.contrast = picture.saturation = 0;
    picture.mode = 4;
    picture.autoFocus = picture.autoBrightness = false;
    ClampPicture(picture);
    ApplyPicture(picture, effect);
    CHECK(effect.mode == ViewMode::SideBySide && !effect.autoFocus && !effect.autoBrightness);
    CHECK(Near(effect.temporal, 0.05f) && Near(effect.maxGain, 1));
    CHECK(Near(effect.brightness, -1) && Near(effect.contrast, 0.5f) && Near(effect.saturation, 0));
}

void RowMeans()
{
    for (int layout = 0; layout != 3; ++layout) {
        StereoFormat format;
        format.eyeWidth = format.eyeHeight = 64;
        format.mono = layout == 0;
        format.halfSecond = layout == 2;
        const uint32_t pitch = format.PackedWidth() * 2 + 16;
        std::vector<uint8_t> frame(size_t(pitch) * format.PackedHeight(), 0);
        const uint32_t mainOffset = layout == 0 ? 0 : layout == 1 ? 128 : 96;
        for (uint32_t row = 0; row < 64; row += 2)
            for (uint32_t x = 0; x < 64; ++x) frame[size_t(row) * pitch + mainOffset + x * 2] = 255;
        std::vector<float> means;
        MainRowMeans(frame.data(), pitch, format, means);
        CHECK(means.size() == 32);
        for (float value : means) CHECK(Near(value, 1));
    }
}

void Flicker()
{
    const std::vector<float> dark(32, 0.1f);
    for (AntiFlicker setting : {AntiFlicker::Off, AntiFlicker::Hz50, AntiFlicker::Hz60}) {
        FlickerGuard guard;
        const int expected = setting == AntiFlicker::Off ? kPowerLineOff :
            setting == AntiFlicker::Hz50 ? kPowerLine50 : kPowerLine60;
        CHECK(guard.Start(setting, false) == expected);
        for (uint32_t i = 0; i < FlickerGuard::kDarkFrames + 1; ++i) CHECK(guard.Update(dark) == -1);
        CHECK(guard.Value() == expected);
    }
    FlickerGuard mains60;
    CHECK(mains60.Start(AntiFlicker::Auto, true) == kPowerLine60);
    for (uint32_t i = 0; i < FlickerGuard::kDarkFrames + 1; ++i) CHECK(mains60.Update(dark) == -1);

    FlickerGuard automatic;
    CHECK(automatic.Start(AntiFlicker::Auto, false) == kPowerLine50);
    for (uint32_t i = 1; i < FlickerGuard::kDarkFrames; ++i) CHECK(automatic.Update(dark) == -1);
    CHECK(automatic.Update(dark) == kPowerLineOff);
    for (int i = 0; i < 100; ++i) CHECK(automatic.Update(std::vector<float>(32, 0.3f + i * 0.002f)) == -1);
    CHECK(!automatic.FlickerSeen());  // A slow exposure change is not periodic lamp flicker.
    automatic.Hold();
    for (int i = 0; i < 100; ++i) CHECK(automatic.Update(dark) == -1);

    FlickerGuard bands;
    bands.WatchAsIfOff();
    const float levels[] = {0.2f, 0.3f, 0.15f};
    for (int i = 0; i < 100 && !bands.FlickerSeen(); ++i)
        bands.Update(std::vector<float>(32, levels[i % 3]));
    CHECK(bands.FlickerSeen() && bands.Value() == kPowerLine50);
    CHECK(bands.Score() > FlickerGuard::kFlickerScore);
    FlickerGuard nextStream;
    CHECK(nextStream.Start(AntiFlicker::Auto, false) == kPowerLine50);
    for (uint32_t i = 0; i < FlickerGuard::kDarkFrames + 1; ++i) CHECK(nextStream.Update(dark) == -1);
}
}

int main()
{
    Calibration();
    Picture();
    RowMeans();
    Flicker();
    std::printf("core logic: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
