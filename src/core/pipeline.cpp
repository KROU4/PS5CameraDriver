#include "pipeline.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "gpu.h"

namespace ps5cam {

namespace {

constexpr uint32_t kNumDisp = 64;
constexpr uint32_t kNoiseBins = 128;  // shaders/denoise.hlsl: bins of 1/4096 of the 3x3-mean change
constexpr float kNoiseBinWidth = 1.0f / 4096;

// Mirrors cbuffer Constants in shaders/common.hlsli.
struct GpuConstants {
    uint32_t eyeW, eyeH, workW, workH;
    uint32_t outW, outH, numDisp, pathDir;
    float rectRow0[4];
    float rectRow1[4];
    float crop[4];
    float focusDisp, blurScale, maxCoC, fgScale;
    float focusRange, temporalAlpha, p1, p2;
    uint32_t mode;
    float guidedEps;
    uint32_t secondOffsetTexels, mainOffsetTexels;
    float highlightGain;
    uint32_t outFormat;
    float lumaGain;
    uint32_t depthView;
    uint32_t secondW, secondH, secondFolded, depthMirror;
    float noiseLevel, denoiseKeep;
    uint32_t denoiseHistory;
    float denoiseSpatial;
};
static_assert(sizeof(GpuConstants) == 176, "constant buffer layout");
// kNumDisp mirrors MAX_DISP in shaders/common.hlsli.

uint32_t DivUp(uint32_t a, uint32_t b) { return (a + b - 1) / b; }

}  // namespace

struct StereoPipeline::Impl {
    GpuImage* packed = nullptr;                          // R8G8B8A8_UINT, YUY2 texels
    GpuImage *mainYuv = nullptr, *secondYuv = nullptr;   // each sensor at its own size (eyeWidth x eyeHeight, SecondWidth x SecondHeight)
    GpuImage* clean[2] = {};                             // denoised main sensor, ping-pong (this frame / history)
    GpuImage *workMain = nullptr, *workSecond = nullptr; // R32_FLOAT work res
    GpuImage *censusMain = nullptr, *censusSecond = nullptr;  // R32G32_UINT work res
    GpuImage *dispMain = nullptr, *dispSecond = nullptr;      // R32_FLOAT work res
    GpuImage* dispFill = nullptr;   // after the LR check and the fill of short gaps, -1 = still unknown
    GpuImage* dispHist[2] = {};     // filled disparity, ping-pong for temporal smoothing
    GpuImage* leftValid = nullptr;
    GpuImage *gfA = nullptr, *gfB = nullptr, *gfC = nullptr;  // R32G32B32A32_FLOAT work res
    GpuImage* bokehHalf = nullptr;                           // R16G16B16A16_FLOAT out/2
    GpuImage *outY = nullptr, *outUV = nullptr;              // R8 / R8G8 out res (read back)
    GpuImage* outYuy2 = nullptr;                             // R8G8B8A8 out/2 x out (packed YUY2, read back)
    GpuImage* depthPlane = nullptr;                          // R8 out res: the depth camera's picture (depthout.hlsl)

    GpuBuffer* sum = nullptr;  // uint16 per (pixel, disparity)
    GpuBuffer *hist = nullptr, *scoreBuf = nullptr, *lumaHist = nullptr, *noiseHist = nullptr;  // read back

    // Readback ring: the GPU fills slot N while the CPU reads slot N-1.
    struct Slot {
        bool depth = false, brightness = false, denoise = false, depthPlane = false;
    } slots[2];
    int pending = -1;  // slot holding a submitted frame that has not been read back yet
    uint32_t histIndex = 0;  // which dispHist is current
    uint32_t cleanIndex = 0;  // which clean texture holds the last denoised frame
    GpuConstants c = {};
};

StereoPipeline::StereoPipeline() = default;

StereoPipeline::~StereoPipeline()
{
    delete m_impl;
}

HRESULT StereoPipeline::Initialize(const StereoFormat& stereo, const OutputFormat& output)
{
    std::lock_guard lock(m_lock);
    if (!stereo.Valid()) return E_INVALIDARG;  // e.g. a half-size row that does not fold evenly
    m_stereo = stereo;
    m_output = output;
    // Depth works on ~640 px wide images: enough for a webcam subject, cheap at 60 fps.
    uint32_t factor = std::max(1u, (stereo.eyeWidth + 400) / 640);
    m_workW = stereo.eyeWidth / factor;
    m_workH = stereo.eyeHeight / factor;

    delete m_impl;
    m_impl = nullptr;
    m_gpu = std::make_unique<Gpu>();
    HRESULT hr = m_gpu->Initialize();
    if (SUCCEEDED(hr)) {
        m_impl = new Impl();
        hr = CreateResources();
    }
    if (FAILED(hr)) {
        delete m_impl;
        m_impl = nullptr;
        m_gpu.reset();
    }
    return hr;
}

std::string StereoPipeline::GpuName() const
{
    return m_gpu ? m_gpu->DeviceName() : std::string();
}

HRESULT StereoPipeline::CreateResources()
{
    auto& g = *m_gpu;
    auto& d = *m_impl;
    HRESULT hr = S_OK;
#define TRY(x)               \
    do {                     \
        hr = (x);            \
        if (FAILED(hr)) return hr; \
    } while (0)
    const unsigned rw = Gpu::kStorage;
    const unsigned rwBack = Gpu::kStorage | Gpu::kReadback;
    const uint32_t ew = m_stereo.eyeWidth, eh = m_stereo.eyeHeight;
    const uint32_t ww = m_workW, wh = m_workH;
    const uint32_t ow = m_output.width, oh = m_output.height;
    // One R8G8B8A8 texel per YUY2 pair: PackedWidth pixels = PackedWidth / 2 texels.
    TRY(g.CreateImage(m_stereo.PackedWidth() / 2, m_stereo.PackedHeight(), GpuFormat::RGBA8_UINT, 0, &d.packed));
    TRY(g.CreateImage(ew, eh, GpuFormat::RGBA8_UNORM, rw, &d.mainYuv));
    TRY(g.CreateImage(m_stereo.SecondWidth(), m_stereo.SecondHeight(), GpuFormat::RGBA8_UNORM, rw, &d.secondYuv));
    TRY(g.CreateImage(ew, eh, GpuFormat::RGBA8_UNORM, rw, &d.clean[0]));
    TRY(g.CreateImage(ew, eh, GpuFormat::RGBA8_UNORM, rw, &d.clean[1]));
    // Depth resources (~45 MB): a single-sensor pipeline never computes depth, so it skips them;
    // Process binds them (as none) only to kernels that do not read them in Main view.
    if (!m_stereo.mono) {
        TRY(g.CreateImage(ww, wh, GpuFormat::R32_FLOAT, rw, &d.workMain));
        TRY(g.CreateImage(ww, wh, GpuFormat::R32_FLOAT, rw, &d.workSecond));
        TRY(g.CreateImage(ww, wh, GpuFormat::RG32_UINT, rw, &d.censusMain));
        TRY(g.CreateImage(ww, wh, GpuFormat::RG32_UINT, rw, &d.censusSecond));
        TRY(g.CreateImage(ww, wh, GpuFormat::R32_FLOAT, rw, &d.dispMain));
        TRY(g.CreateImage(ww, wh, GpuFormat::R32_FLOAT, rw, &d.dispSecond));
        TRY(g.CreateImage(ww, wh, GpuFormat::R32_FLOAT, rw, &d.dispFill));
        TRY(g.CreateImage(ww, wh, GpuFormat::R32_FLOAT, rw, &d.dispHist[0]));
        TRY(g.CreateImage(ww, wh, GpuFormat::R32_FLOAT, rw, &d.dispHist[1]));
        TRY(g.CreateImage(ww, wh, GpuFormat::R32_FLOAT, rw, &d.leftValid));
        TRY(g.CreateImage(ww, wh, GpuFormat::RGBA32_FLOAT, rw, &d.gfA));
        TRY(g.CreateImage(ww, wh, GpuFormat::RGBA32_FLOAT, rw, &d.gfB));
        TRY(g.CreateImage(ww, wh, GpuFormat::RGBA32_FLOAT, rw, &d.gfC));
        TRY(g.CreateBuffer(ww * wh * kNumDisp * 2, false, &d.sum));
    }
    TRY(g.CreateImage(ow / 2, oh / 2, GpuFormat::RGBA16_FLOAT, rw, &d.bokehHalf));
    TRY(g.CreateImage(ow, oh, GpuFormat::R8_UNORM, rwBack, &d.outY));
    TRY(g.CreateImage(ow / 2, oh / 2, GpuFormat::RG8_UNORM, rwBack, &d.outUV));
    TRY(g.CreateImage(ow / 2, oh, GpuFormat::RGBA8_UNORM, rwBack, &d.outYuy2));
    if (!m_stereo.mono) TRY(g.CreateImage(ow, oh, GpuFormat::R8_UNORM, rwBack, &d.depthPlane));

    TRY(g.CreateBuffer(kNumDisp * 4, true, &d.hist));
    TRY(g.CreateBuffer(16, true, &d.scoreBuf));
    TRY(g.CreateBuffer(kNumDisp * 4, true, &d.lumaHist));
    TRY(g.CreateBuffer(kNoiseBins * 4, true, &d.noiseHist));
#undef TRY
    return S_OK;
}

void StereoPipeline::Reset()
{
    std::lock_guard lock(m_lock);
    m_haveHistory = false;
    m_haveClean = false;
    m_noise = -1;  // another stream may run at another gain
    m_focus = -1;
    m_focusPeak = -1;
    m_focusCandidate = -1;
    m_focusCandidateFrames = 0;
    if (m_impl) m_impl->pending = -1;
}

void StereoPipeline::SetRectification(const Rectification& r)
{
    std::lock_guard lock(m_lock);
    m_rect = r;
}

void StereoPipeline::Upload(const uint8_t* yuy2, uint32_t pitch)
{
    m_gpu->Upload(m_impl->packed, yuy2, pitch);
}

void StereoPipeline::UpdateConstants(const EffectSettings* s, uint32_t pathDir)
{
    auto& c = m_impl->c;
    const auto& st = m_stereo;
    const auto& o = m_output;
    c.eyeW = st.eyeWidth;
    c.eyeH = st.eyeHeight;
    c.workW = m_workW;
    c.workH = m_workH;
    c.outW = o.width;
    c.outH = o.height;
    c.numDisp = kNumDisp;
    c.pathDir = pathDir;

    // Second sensor sampling position for a main-sensor pixel: rotation about the image centre
    // plus a vertical shift. Horizontal offset is left to the disparity search.
    float a = m_rect.rotation * 3.14159265f / 180.0f;
    float cs = std::cos(a), sn = std::sin(a);
    float cx = st.eyeWidth * 0.5f, cy = st.eyeHeight * 0.5f;
    c.rectRow0[0] = cs;
    c.rectRow0[1] = -sn;
    c.rectRow0[2] = cx - cs * cx + sn * cy;
    c.rectRow1[0] = sn;
    c.rectRow1[1] = cs;
    c.rectRow1[2] = cy - sn * cx - cs * cy + m_rect.dy;

    c.crop[0] = o.cropX;
    c.crop[1] = o.cropY;
    c.crop[2] = o.cropW;
    c.crop[3] = o.cropH;
    if (st.halfSecond) {
        c.mainOffsetTexels = StereoFormat::kHalfHeaderPixels / 2;
        c.secondOffsetTexels = (StereoFormat::kHalfHeaderPixels + st.eyeWidth) / 2;
    } else {
        // Mono input: both views read the single sensor (the second one is never used for depth).
        c.mainOffsetTexels = st.MainIsRightStream() ? st.eyeWidth / 2 : 0;
        c.secondOffsetTexels = st.MainIsRightStream() || st.mono ? 0 : st.eyeWidth / 2;
    }
    c.secondW = st.SecondWidth();
    c.secondH = st.SecondHeight();
    c.secondFolded = st.halfSecond ? 1 : 0;
    // The matching passes assume the main sensor is the right stream (see downscale.hlsl).
    c.depthMirror = !st.mono && !st.MainIsRightStream() ? 1 : 0;
    c.p1 = 10;
    c.p2 = 120;
    c.guidedEps = 0.0004f;
    if (s) {
        float strength = std::clamp(s->blurStrength, 0.0f, 1.0f);
        float outScale = o.width / 1920.0f;
        // Blur grows with disparity measured in eye pixels and is expressed in output pixels.
        float workToOut = (float(st.eyeWidth) / m_workW) * (float(o.width) / o.cropW);
        c.blurScale = (0.08f + 0.5f * strength) * workToOut;
        c.maxCoC = (6.0f + 26.0f * strength) * outScale;
        c.fgScale = std::clamp(s->foregroundBlur, 0.0f, 1.0f);
        c.focusRange = std::max(0.0f, s->focusRange);
        c.temporalAlpha = m_haveHistory ? std::clamp(s->temporal, 0.05f, 1.0f) : 1.0f;
        c.highlightGain = std::max(0.0f, s->highlights);
        c.mode = static_cast<uint32_t>(s->mode);
        c.focusDisp = m_focus >= 0 ? m_focus : 16.0f;
        c.outFormat = static_cast<uint32_t>(o.format);
        c.lumaGain = s->autoBrightness ? m_gain : 1.0f;
        c.depthView = s->depthView;
        // Strongest setting: a still pixel keeps 12% of each new frame (noise std / ~4 once settled).
        const float denoise = std::clamp(s->denoise, 0.0f, 1.0f);
        c.denoiseKeep = 1.0f - 0.88f * denoise;
        c.denoiseSpatial = denoise;
        c.denoiseHistory = m_haveClean ? 1 : 0;
        c.noiseLevel = m_noise >= 0 ? m_noise : 0.0f;
    }
    m_gpu->SetConstants(&c, sizeof(c));
}

void StereoPipeline::RunDepth(const EffectSettings& s)
{
    auto& g = *m_gpu;
    auto& d = *m_impl;
    const uint32_t ww = m_workW, wh = m_workH;

    g.Dispatch(Kernel::Downscale, {d.mainYuv, d.secondYuv}, {d.workMain, d.workSecond}, DivUp(ww, 16), DivUp(wh, 8));
    g.Dispatch(Kernel::Census, {d.workMain, d.workSecond}, {d.censusMain, d.censusSecond}, DivUp(ww, 16), DivUp(wh, 8));

    for (uint32_t dir = 0; dir < 4; ++dir) {
        UpdateConstants(&s, dir);
        g.Dispatch(Kernel::Aggregate, {d.censusMain, d.censusSecond, d.workMain}, {d.sum}, dir < 2 ? wh : ww, 1);
    }

    g.Dispatch(Kernel::Wta, {d.sum, d.workMain, d.censusMain, d.censusSecond}, {d.dispMain, d.dispSecond}, DivUp(ww, 16),
        DivUp(wh, 8));

    uint32_t cur = d.histIndex ^ 1, prev = d.histIndex;
    if (!m_haveHistory) g.ClearFloat(d.dispHist[prev], -1);
    g.Dispatch(Kernel::LrFill, {d.dispMain, d.dispSecond}, {d.dispFill, d.leftValid}, DivUp(wh, 64), 1);
    g.Dispatch(Kernel::HoleFill, {d.dispFill, d.dispHist[prev], d.workMain}, {d.dispHist[cur]}, DivUp(ww, 16),
        DivUp(wh, 8));
    d.histIndex = cur;
    m_haveHistory = true;

    // Guided filter: means of (I, p, I^2, Ip) -> coefficients (a, b) -> their means.
    g.Dispatch(Kernel::GuidedPrep, {d.workMain, d.dispHist[cur]}, {d.gfA}, DivUp(ww, 16), DivUp(wh, 8));
    auto box = [&](GpuImage* src, GpuImage* tmp) {
        UpdateConstants(&s, 0);
        g.Dispatch(Kernel::GuidedBox, {src}, {tmp}, DivUp(ww, 16), DivUp(wh, 8));
        UpdateConstants(&s, 1);
        g.Dispatch(Kernel::GuidedBox, {tmp}, {src}, DivUp(ww, 16), DivUp(wh, 8));
    };
    box(d.gfA, d.gfB);
    g.Dispatch(Kernel::GuidedCoef, {d.gfA}, {d.gfC}, DivUp(ww, 16), DivUp(wh, 8));
    box(d.gfC, d.gfB);
}

void StereoPipeline::UpdateFocus(const uint32_t* histogram, const EffectSettings& s)
{
    if (!s.autoFocus) {
        m_focus = std::clamp(s.manualFocus, 0.0f, 1.0f) * (kNumDisp - 1);
        return;
    }
    // Bins 0-1 collect unmatched pixels and are ignored.
    double total = 0;
    float smooth[kNumDisp] = {};
    for (uint32_t i = 0; i < kNumDisp; ++i) {
        if (i >= 2) total += histogram[i];
        smooth[i] = 0.5f * histogram[i] + 0.25f * histogram[i > 0 ? i - 1 : i] +
                    0.25f * histogram[i + 1 < kNumDisp ? i + 1 : i];
    }
    if (total < 50) return;
    auto share = [&](int i) {
        double mass = 0;
        for (int k = std::max(2, i - 2); k <= std::min<int>(kNumDisp - 1, i + 2); ++k) mass += histogram[k];
        return mass / total;
    };
    // Nearest peak that holds a meaningful share of the central area: the subject. A subject
    // off-centre or far away fills only ~10% of the weighted area, hence the low share.
    int pick = -1, globalMax = 2;
    for (int i = kNumDisp - 2; i >= 2; --i) {
        if (smooth[i] > smooth[globalMax]) globalMax = i;
        bool peak = smooth[i] >= smooth[i - 1] && smooth[i] >= smooth[i + 1];
        if (pick < 0 && peak && share(i) >= 0.07) pick = i;
    }
    if (pick < 0) pick = globalMax;
    // While the subject being followed is still there, another peak (a hand raised towards the
    // camera, a share hovering around the threshold) takes over only after kFocusSwitchFrames.
    if (m_focusPeak >= 2 && std::abs(pick - m_focusPeak) > 2 && share(m_focusPeak) >= 0.05) {
        if (std::abs(pick - m_focusCandidate) <= 2) ++m_focusCandidateFrames;
        else m_focusCandidateFrames = 1;
        m_focusCandidate = pick;
        if (m_focusCandidateFrames < kFocusSwitchFrames) pick = m_focusPeak;
    }
    if (pick != m_focusPeak) {
        m_focusCandidate = -1;
        m_focusCandidateFrames = 0;
    }
    double wsum = 0, dsum = 0;
    for (int k = std::max(2, pick - 2); k <= std::min<int>(kNumDisp - 1, pick + 2); ++k) {
        wsum += histogram[k];
        dsum += double(histogram[k]) * k;
    }
    float target = wsum > 0 ? float(dsum / wsum) : float(pick);
    m_focusPeak = static_cast<int>(std::lround(target));  // follows the subject moving closer or away
    if (m_focus < 0)
        m_focus = target;
    else if (std::fabs(target - m_focus) > 0.6f)
        m_focus += (target - m_focus) * 0.15f;  // glide, like a lens refocusing
}

void StereoPipeline::UpdateGain(const uint32_t* h, const EffectSettings& s)
{
    double total = 0, mean = 0;
    for (uint32_t i = 0; i < kNumDisp; ++i) {
        total += h[i];
        mean += double(h[i]) * (i + 0.5) / kNumDisp;
    }
    if (total <= 0) return;
    mean /= total;
    // White point at the 97th percentile (small lamps may clip softly); average aimed at ~0.42.
    double acc = 0;
    uint32_t white = kNumDisp - 1;
    for (uint32_t i = 0; i < kNumDisp; ++i) {
        acc += h[i];
        if (acc >= total * 0.97) {
            white = i;
            break;
        }
    }
    double whiteLevel = (white + 1.0) / kNumDisp;
    double target = std::min({1.0 / whiteLevel, 0.42 / std::max(mean, 1e-3), double(std::max(1.0f, s.maxGain))});
    target = std::max(1.0, target);
    m_gain += float(target - m_gain) * 0.08f;  // like a camera AE: settle over about half a second
}

void StereoPipeline::UpdateNoise(const uint32_t* h)
{
    // The still scene's change from the lower quarter of the distribution (for the half-normal
    // distribution of noise the 25th percentile is 0.32 sigma and the median, which the shader works
    // with, 0.67), so that motion in part of the picture raises it little: a quarter of the picture
    // moving by ~1.4x, half of it by ~2x. When even the lower quarter is in the last bin, nearly
    // everything changed (a pan, an exposure step): no measurement. The level rises slowly and falls
    // quickly, so a moment of such change does not make the filter blend motion into the history.
    double total = 0;
    for (uint32_t i = 0; i < kNoiseBins; ++i) total += h[i];
    if (total < 1000) return;
    const double want = total * 0.25;
    double acc = 0;
    float quartile = -1;
    for (uint32_t i = 0; i + 1 < kNoiseBins; ++i) {
        if (acc + h[i] >= want) {
            quartile = i + float((want - acc) / h[i]);
            break;
        }
        acc += h[i];
    }
    if (quartile < 0) return;
    const float level = quartile * kNoiseBinWidth * (0.674f / 0.319f);
    m_noise = m_noise < 0 ? level : m_noise + (level - m_noise) * (level > m_noise ? 0.05f : 0.2f);
}

HRESULT StereoPipeline::Process(const uint8_t* yuy2, uint32_t yuy2Pitch, const EffectSettings& requested, uint8_t* dst,
    uint8_t* dstUV, uint32_t dstPitch, FrameStats* stats, DepthPlane* depth)
{
    std::lock_guard lock(m_lock);
    if (depth) depth->written = false;
    if (!m_impl) return E_NOT_VALID_STATE;
    EffectSettings s = requested;
    if (m_stereo.mono) s.mode = ViewMode::Main;  // no second sensor: no depth, no side-by-side
    auto& g = *m_gpu;
    auto& d = *m_impl;
    const uint32_t ow = m_output.width, oh = m_output.height;
    const int slot = (d.pending == 0) ? 1 : 0;
    // The second sensor view shows no main sensor image to clean up.
    const bool denoise = s.denoise > 0.0f && s.mode != ViewMode::Second;
    if (denoise && m_frame != m_lastDenoiseFrame + 1) m_haveClean = false;  // it was off: history is stale

    g.BeginTiming(slot);

    Upload(yuy2, yuy2Pitch);
    UpdateConstants(&s, 0);

    g.Dispatch(Kernel::Unpack, {d.packed}, {d.mainYuv, d.secondYuv}, DivUp(m_stereo.eyeWidth / 2, 16),
        DivUp(m_stereo.eyeHeight, 8));
    d.slots[slot].brightness = s.autoBrightness;
    if (s.autoBrightness) {
        g.ClearUint(d.lumaHist);
        g.Dispatch(Kernel::LumaStats, {d.mainYuv}, {d.lumaHist}, DivUp(m_stereo.eyeWidth / 8, 16),
            DivUp(m_stereo.eyeHeight / 8, 8));
        g.CopyToReadback(d.lumaHist, slot);
    }
    // What the picture is made of: the denoised main sensor image, or the raw one. Stereo matching
    // keeps the raw image (census does not mind the noise, and a history there would lag behind
    // motion); the full-resolution guide of the disparity (DisparityAt) is the picture, whose
    // edges are where the blur has to follow, and its smaller noise only steadies the disparity.
    GpuImage* image = d.mainYuv;
    d.slots[slot].denoise = denoise && m_haveClean;  // only then the shader fills the noise histogram
    if (denoise) {
        const uint32_t cur = d.cleanIndex ^ 1;
        g.ClearUint(d.noiseHist);
        g.Dispatch(Kernel::Denoise, {d.mainYuv, d.clean[d.cleanIndex]}, {d.clean[cur], d.noiseHist},
            DivUp(m_stereo.eyeWidth, 16), DivUp(m_stereo.eyeHeight, 8));
        if (d.slots[slot].denoise) g.CopyToReadback(d.noiseHist, slot);
        d.cleanIndex = cur;
        m_haveClean = true;
        m_lastDenoiseFrame = m_frame;
        image = d.clean[cur];
    }

    const bool needDepth = s.mode == ViewMode::Bokeh || s.mode == ViewMode::Depth || s.mode == ViewMode::DebugRaw ||
                           s.mode == ViewMode::DebugFilled || s.mode == ViewMode::DebugHoles ||
                           s.mode == ViewMode::DebugGrey || (s.depthPlane && !m_stereo.mono);
    d.slots[slot].depth = needDepth;
    if (needDepth) {
        if (m_frame != m_lastDepthFrame + 1) m_haveHistory = false;  // depth was off: history is stale
        m_lastDepthFrame = m_frame;
        RunDepth(s);
        UpdateConstants(&s, 0);
        g.ClearUint(d.hist);
        g.Dispatch(Kernel::Histogram, {image, d.gfC}, {d.hist}, DivUp(ow / 4, 16), DivUp(oh / 4, 8));
        g.CopyToReadback(d.hist, slot);
    }
    if (s.mode == ViewMode::Bokeh)
        g.Dispatch(Kernel::Bokeh, {image, d.gfC}, {d.bokehHalf}, DivUp(ow / 2, 16), DivUp(oh / 2, 8));
    const bool packedOut = m_output.format == PixelFormat::YUY2;
    g.Dispatch(Kernel::Composite,
        {image, d.gfC, d.bokehHalf, d.secondYuv, d.dispMain, d.dispFill, d.dispHist[d.histIndex]},
        {d.outY, d.outUV, d.outYuy2}, DivUp(ow / 2, 16), DivUp(oh / 2, 8));
    d.slots[slot].depthPlane = s.depthPlane && !m_stereo.mono;
    if (d.slots[slot].depthPlane) {
        g.Dispatch(Kernel::DepthOut, {image, d.gfC}, {d.depthPlane}, DivUp(ow, 16), DivUp(oh, 8));
        g.CopyToReadback(d.depthPlane, slot);
    }
    g.Unbind();

    if (packedOut) {
        g.CopyToReadback(d.outYuy2, slot);
    } else {
        g.CopyToReadback(d.outY, slot);
        g.CopyToReadback(d.outUV, slot);
    }
    g.EndTiming(slot);
    g.Flush();  // start the GPU now; it runs while we read back the previous frame

    // Deliver the previous frame (normally finished long ago). The very first frame only primes
    // the ring, so the caller gets S_FALSE once.
    const int read = d.pending;
    d.pending = slot;
    ++m_frame;
    if (read < 0) return S_FALSE;

    auto readPlane = [&](GpuImage* image, uint8_t* out, uint32_t rows, uint32_t rowBytes, uint32_t pitch) {
        GpuMapped m;
        HRESULT hr = g.Map(image, read, &m);
        if (FAILED(hr)) return hr;
        for (uint32_t y = 0; y < rows; ++y)
            memcpy(out + size_t(y) * pitch, m.data + size_t(y) * m.rowPitch, rowBytes);
        g.Unmap(image, read);
        return S_OK;
    };
    HRESULT hr = packedOut ? readPlane(d.outYuy2, dst, oh, ow * 2, dstPitch) : readPlane(d.outY, dst, oh, ow, dstPitch);
    if (SUCCEEDED(hr) && !packedOut) hr = readPlane(d.outUV, dstUV, oh / 2, ow, dstPitch);
    if (FAILED(hr)) return hr;
    if (depth && depth->data && depth->pitch >= ow && d.slots[read].depthPlane &&
        SUCCEEDED(readPlane(d.depthPlane, depth->data, oh, ow, depth->pitch)))
        depth->written = true;

    auto withHistogram = [&](GpuBuffer* buffer, auto use) {
        GpuMapped m;
        if (SUCCEEDED(g.Map(buffer, read, &m))) {
            use(reinterpret_cast<const uint32_t*>(m.data));
            g.Unmap(buffer, read);
        }
    };
    if (d.slots[read].brightness) withHistogram(d.lumaHist, [&](const uint32_t* h) { UpdateGain(h, s); });
    if (d.slots[read].depth) withHistogram(d.hist, [&](const uint32_t* h) { UpdateFocus(h, s); });
    if (d.slots[read].denoise) withHistogram(d.noiseHist, [&](const uint32_t* h) { UpdateNoise(h); });

    if (stats) {
        float ms = 0;
        if (g.TimingMs(read, &ms)) stats->gpuMs = ms;
        stats->focusDisparity = m_focus;
        stats->gain = s.autoBrightness ? m_gain : 1.0f;
        stats->noise = std::max(m_noise, 0.0f);
    }
    return S_OK;
}

float StereoPipeline::ScoreAlignment(float dy, float rotationDeg)
{
    auto& g = *m_gpu;
    auto& d = *m_impl;
    m_rect.dy = dy;
    m_rect.rotation = rotationDeg;
    UpdateConstants(nullptr, 0);
    g.Dispatch(Kernel::Downscale, {d.mainYuv, d.secondYuv}, {d.workMain, d.workSecond}, DivUp(m_workW, 16),
        DivUp(m_workH, 8));
    g.Dispatch(Kernel::Census, {d.workMain, d.workSecond}, {d.censusMain, d.censusSecond}, DivUp(m_workW, 16),
        DivUp(m_workH, 8));
    g.ClearUint(d.scoreBuf);
    g.Dispatch(Kernel::Score, {d.censusMain, d.censusSecond}, {d.scoreBuf}, DivUp(m_workW, 16), DivUp(m_workH, 8));
    g.Unbind();
    g.CopyToReadback(d.scoreBuf, 0);
    GpuMapped m;
    if (FAILED(g.Map(d.scoreBuf, 0, &m))) return 1e9f;
    const uint32_t* v = reinterpret_cast<const uint32_t*>(m.data);
    float score = v[1] > 500 ? float(v[0]) / v[1] : 1e9f;
    g.Unmap(d.scoreBuf, 0);
    return score;
}

HRESULT StereoPipeline::Calibrate(const uint8_t* yuy2, uint32_t yuy2Pitch, Rectification* result)
{
    std::lock_guard lock(m_lock);
    if (!m_impl || m_stereo.mono) return E_NOT_VALID_STATE;
    auto& d = *m_impl;
    Rectification saved = m_rect;
    Upload(yuy2, yuy2Pitch);
    UpdateConstants(nullptr, 0);
    m_gpu->Dispatch(Kernel::Unpack, {d.packed}, {d.mainYuv, d.secondYuv}, DivUp(m_stereo.eyeWidth / 2, 16),
        DivUp(m_stereo.eyeHeight, 8));

    // Bail out cheaply on dark or featureless frames.
    if (ScoreAlignment(0, 0) >= 1e8f) {
        m_rect = saved;
        return E_FAIL;
    }
    // Coarse vertical sweep, then a joint refinement of offset and roll around the best value.
    float bestDy = 0, bestRot = 0, best = 1e9f;
    std::vector<float> sweep;
    for (float dy = -12; dy <= 12; dy += 1.0f) {
        float sc = ScoreAlignment(dy, 0);
        sweep.push_back(sc);
        if (sc < best) best = sc, bestDy = dy;
    }
    // Noise-only frames give a flat curve; require a clear minimum before trusting the result.
    std::nth_element(sweep.begin(), sweep.begin() + sweep.size() / 2, sweep.end());
    float median = sweep[sweep.size() / 2];
    if (median <= 0 || (median - best) / median < 0.12f) {
        m_rect = saved;
        return E_FAIL;
    }
    float coarseDy = bestDy;
    for (float dy = coarseDy - 1; dy <= coarseDy + 1.001f; dy += 0.25f)
        for (float rot = -1.0f; rot <= 1.001f; rot += 0.25f) {
            float sc = ScoreAlignment(dy, rot);
            if (sc < best) best = sc, bestDy = dy, bestRot = rot;
        }
    if (best >= 1e8f) {
        m_rect = saved;
        return E_FAIL;  // not enough texture to decide
    }
    m_rect = {bestDy, bestRot, best};
    m_haveHistory = false;
    if (result) *result = m_rect;
    return S_OK;
}

}  // namespace ps5cam
