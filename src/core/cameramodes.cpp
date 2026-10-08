#include "cameramodes.h"

#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace ps5cam {

const wchar_t* WantedKey(ViewMode view)
{
    return view == ViewMode::Main ? kMonoKey : kHalfKey;
}

std::vector<SensorMode> SensorModes(uint32_t outputFps, ViewMode view)
{
    const uint32_t fps = outputFps > 30 ? 60u : 30u;
    std::vector<SensorMode> modes;
    // Without a depth effect the image comes from one sensor at full 1920x1080 (60 fps since e7).
    if (view == ViewMode::Main) modes.push_back({1920, 1080, fps, Layout::Mono, kMonoKey});
    // Depth, firmware e9: the same sensor at full 1920x1080 plus the second one at 960x540, at 30 or
    // 60 fps (two full sensors at 60 fps would exceed the USB bandwidth).
    modes.push_back({2448, 1088, fps, Layout::HalfSecond, kHalfKey});
    // Older images: both full sensors up to 30 fps, the 1280x800 crop of each at 60.
    if (fps > 30) modes.push_back({2560, 800, 60, Layout::SideBySide, L"800"});
    else modes.push_back({3840, 1080, 30, Layout::SideBySide, L"1080"});
    return modes;
}

bool SensorModeOf(uint32_t packedW, uint32_t packedH, uint32_t fps, SensorMode* mode)
{
    const SensorMode known[] = {
        {1920, 1080, fps, Layout::Mono, kMonoKey},
        {2448, 1088, fps, Layout::HalfSecond, kHalfKey},
        {2560, 800, fps, Layout::SideBySide, L"800"},
        {3840, 1080, fps, Layout::SideBySide, L"1080"},
    };
    for (const SensorMode& m : known) {
        if (m.packedW == packedW && m.packedH == packedH) {
            *mode = m;
            return true;
        }
    }
    return false;
}

StereoFormat FormatOf(const SensorMode& m)
{
    StereoFormat sf;
    switch (m.layout) {
    case Layout::Mono:
        sf.mono = true;
        sf.eyeWidth = m.packedW;
        sf.eyeHeight = m.packedH;
        break;
    case Layout::SideBySide:
        sf.eyeWidth = m.packedW / 2;
        sf.eyeHeight = m.packedH;
        break;
    case Layout::HalfSecond:
        sf.halfSecond = true;
        sf.eyeWidth = (m.packedW - StereoFormat::kHalfHeaderPixels) * 4 / 5;  // main + main / 4
        sf.eyeHeight = m.packedH - StereoFormat::kHalfExtraLines;
        break;
    }
    return sf;
}

OutputFormat OutputFor(const StereoFormat& sf, uint32_t width, uint32_t height, PixelFormat format)
{
    OutputFormat of;
    of.width = width;
    of.height = height;
    of.format = format;
    // Keep the output aspect: crop the sensor image vertically or horizontally as needed.
    float outAspect = float(of.width) / of.height, eyeAspect = float(sf.eyeWidth) / sf.eyeHeight;
    of.cropW = eyeAspect > outAspect ? sf.eyeHeight * outAspect : float(sf.eyeWidth);
    of.cropH = eyeAspect > outAspect ? float(sf.eyeHeight) : sf.eyeWidth / outAspect;
    of.cropX = (sf.eyeWidth - of.cropW) * 0.5f;
    of.cropY = (sf.eyeHeight - of.cropH) * 0.5f;
    return of;
}

std::vector<TypeSpec> OutputTypes(bool prefer60, bool fullHdOnly)
{
    // Both rates show one full 1920x1080 sensor; with depth (firmware e9) the second sensor comes
    // along at half size. Older images fall back to the 1280x800 stereo crop at 60 fps and both full
    // sensors at 30 (see SensorModes).
    std::vector<TypeSpec> types;
    if (fullHdOnly) {
        for (PixelFormat fmt : {PixelFormat::NV12, PixelFormat::YUY2}) types.push_back({1920, 1080, 60, fmt});
        return types;
    }
    const uint32_t sizes[][2] = {{1920, 1080}, {1280, 720}};  // Full HD and HD; apps scale anything smaller
    const uint32_t rates[2] = {prefer60 ? 60u : 30u, prefer60 ? 30u : 60u};
    for (PixelFormat fmt : {PixelFormat::NV12, PixelFormat::YUY2})
        for (const auto& s : sizes)
            for (uint32_t fps : rates) types.push_back({s[0], s[1], fps, fmt});
    return types;
}

HRESULT MakeVideoType(const TypeSpec& t, IMFMediaType** out)
{
    ComPtr<IMFMediaType> mt;
    HRESULT hr = MFCreateMediaType(&mt);
    if (FAILED(hr)) return hr;
    const bool yuy2 = t.fmt == PixelFormat::YUY2;
    mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    mt->SetGUID(MF_MT_SUBTYPE, yuy2 ? MFVideoFormat_YUY2 : MFVideoFormat_NV12);
    MFSetAttributeSize(mt.Get(), MF_MT_FRAME_SIZE, t.w, t.h);
    // Express the rate as a whole 100 ns frame interval (60 fps -> 166666, like the camera's own UVC
    // descriptors): the DirectShow bridge truncates intervals, and 60/1 would otherwise surface as
    // 59.9999 fps and reject a request for exactly 60.
    UINT32 interval = 10'000'000 / t.fps;
    MFSetAttributeRatio(mt.Get(), MF_MT_FRAME_RATE, 10'000'000, interval);
    MFSetAttributeRatio(mt.Get(), MF_MT_FRAME_RATE_RANGE_MAX, 10'000'000, interval);
    MFSetAttributeRatio(mt.Get(), MF_MT_FRAME_RATE_RANGE_MIN, 10'000'000, interval);
    MFSetAttributeRatio(mt.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    mt->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    mt->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    mt->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
    mt->SetUINT32(MF_MT_DEFAULT_STRIDE, yuy2 ? t.w * 2 : t.w);  // positive: top-down
    UINT32 sampleSize = yuy2 ? t.w * t.h * 2 : t.w * t.h * 3 / 2;
    mt->SetUINT32(MF_MT_SAMPLE_SIZE, sampleSize);
    mt->SetUINT32(MF_MT_AVG_BITRATE, sampleSize * 8 * t.fps);
    mt->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT601);
    mt->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    mt->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
    mt->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
    *out = mt.Detach();
    return S_OK;
}

}  // namespace ps5cam
