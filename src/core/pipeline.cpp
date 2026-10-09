#include "pipeline.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "gpu.h"

namespace ps5cam {

namespace {

constexpr uint32_t kNumDisp = 64;
constexpr float kRangeDisparity = 24;  // EffectSettings::focusRange and subjectRange hold at this disparity
constexpr float kDefaultFocus = 16;    // the focus plane until the autofocus has one
constexpr uint32_t kNoiseBins = 128;  // shaders/denoise.hlsl: bins of 1/4096 of the 3x3-mean change
constexpr float kNoiseBinWidth = 1.0f / 4096;
constexpr uint32_t kMotionBlock = 32;  // shaders/motion.hlsl: pixels per motion vector, each way
// shaders/meter.hlsl: cells across and down the picture, samples per cell, three floats per cell.
constexpr uint32_t kMeterCellsX = 32, kMeterCellsY = 18, kMeterSamples = 64;
// Mean luma the auto brightness aims at: of the picture, and with depth of the subject's head.
constexpr double kFrameTarget = 0.42;
constexpr double kSubjectTarget = 0.42;
// Auto brightness's stable zone (UpdateGain): the gain follows once the target is this far off...
constexpr float kGainStart = 0.04f;
// ...and stops once it is this close.
constexpr float kGainStop = 0.01f;
// A measurement this far from the smoothed one replaces it at once.
constexpr float kGainJump = 0.3f;
// Stereo self-calibration (Calibrate): the well textured pixels (score.hlsl) a score needs, and how
// far the best vertical offset must stand out below the sweep's median.
constexpr uint32_t kCalibrationPixels = 2000;
constexpr float kCalibrationContrast = 0.08f;
// The head's light (MeasureSubject): this share of its cells, by subject weight, is darker...
constexpr double kHeadPercentile = 0.75;
// ...counting the cells at least this much on the subject; its columns are where the top rows are
// at least kHeadColumn on it.
constexpr float kHeadCell = 0.5f;
constexpr float kHeadColumn = 0.25f;

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
    float sharpen, sharpenCore;
    float subjectRange;
    float pad;
};
static_assert(sizeof(GpuConstants) == 192, "constant buffer layout");
// kNumDisp mirrors MAX_DISP in shaders/common.hlsli.

uint32_t DivUp(uint32_t a, uint32_t b) { return (a + b - 1) / b; }

}  // namespace

struct StereoPipeline::Impl {
    GpuImage* packed = nullptr;                          // R8G8B8A8_UINT, YUY2 texels
    GpuImage *mainYuv = nullptr, *secondYuv = nullptr;   // each sensor at its own size (eyeWidth x eyeHeight, SecondWidth x SecondHeight)
    GpuImage* clean[2] = {};                             // denoised main sensor, ping-pong (this frame / history)
    // Motion of the picture against the history (motion.hlsl): 4x-downscaled luma of this frame and
    // of the history (R32_FLOAT), coarse and refined vectors per 32x32 block (R32G32_UINT; the
    // refined ones ping-pong, as the noise reduction checks them against the previous frame's).
    GpuImage *smallCur = nullptr, *smallPrev = nullptr, *motionCoarse = nullptr, *motion[2] = {};
    uint32_t motionIndex = 0;  // which motion holds the latest vectors
    GpuImage *workMain = nullptr, *workSecond = nullptr; // R32_FLOAT work res
    GpuImage* workPrev = nullptr;   // the previous depth frame's workMain (they swap): a still picture for holefill.hlsl
    GpuImage *censusMain = nullptr, *censusSecond = nullptr;  // R32G32_UINT work res
    GpuImage *dispMain = nullptr, *dispSecond = nullptr;      // R32_FLOAT work res
    GpuImage* dispFill = nullptr;   // after the LR check and the fill of short gaps, -1 = still unknown
    GpuImage* dispHist[2] = {};     // filled disparity, ping-pong for temporal smoothing
    GpuImage* dispFrame[2] = {};    // the same before the smoothing (holefill.hlsl confirms jumps with it), ping-pong alike
    GpuImage* leftValid = nullptr;
    GpuImage *gfA = nullptr, *gfB = nullptr, *gfC = nullptr;  // R32G32B32A32_FLOAT work res
    // The subject's silhouette (subject.hlsl), R32_FLOAT work res: the guided disparity, the cost of
    // reaching each pixel from the focus plane (ping-pong), the subject's share (R8_UNORM) the blur passes read.
    GpuImage *subjDisp = nullptr, *subjCost[2] = {}, *subjShare[2] = {};
    uint32_t shareIndex = 0;  // which subjShare is this frame's
    GpuImage* Share() const { return subjShare[shareIndex]; }
    GpuImage* bokehHalf = nullptr;                           // R16G16B16A16_FLOAT out/2
    GpuImage *outY = nullptr, *outUV = nullptr;              // R8 / R8G8 out res (read back)
    GpuImage* outYuy2 = nullptr;                             // R8G8B8A8 out/2 x out (packed YUY2, read back)
    GpuImage* depthPlane = nullptr;                          // R8 out res: the depth camera's picture (depthout.hlsl)

    GpuBuffer* sum = nullptr;  // uint16 per (pixel, disparity)
    GpuBuffer *hist = nullptr, *scoreBuf = nullptr, *lumaHist = nullptr, *noiseHist = nullptr;  // read back
    GpuBuffer* meter = nullptr;  // the subject's light per cell (meter.hlsl), read back

    // Readback ring: the GPU fills slot N while the CPU reads slot N-1.
    struct Slot {
        bool depth = false, brightness = false, denoise = false, depthPlane = false, meter = false;
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
    TRY(g.CreateImage(ew / 4, eh / 4, GpuFormat::R32_FLOAT, rw, &d.smallCur));
    TRY(g.CreateImage(ew / 4, eh / 4, GpuFormat::R32_FLOAT, rw, &d.smallPrev));
    TRY(g.CreateImage(DivUp(ew, kMotionBlock), DivUp(eh, kMotionBlock), GpuFormat::RG32_UINT, rw, &d.motionCoarse));
    TRY(g.CreateImage(DivUp(ew, kMotionBlock), DivUp(eh, kMotionBlock), GpuFormat::RG32_UINT, rw, &d.motion[0]));
    TRY(g.CreateImage(DivUp(ew, kMotionBlock), DivUp(eh, kMotionBlock), GpuFormat::RG32_UINT, rw, &d.motion[1]));
    // Depth resources (~45 MB): a single-sensor pipeline never computes depth, so it skips them;
    // Process binds them (as none) only to kernels that do not read them in Main view.
    if (!m_stereo.mono) {
        TRY(g.CreateImage(ww, wh, GpuFormat::R32_FLOAT, rw, &d.workMain));
        TRY(g.CreateImage(ww, wh, GpuFormat::R32_FLOAT, rw, &d.workPrev));
        TRY(g.CreateImage(ww, wh, GpuFormat::R32_FLOAT, rw, &d.workSecond));
        TRY(g.CreateImage(ww, wh, GpuFormat::RG32_UINT, rw, &d.censusMain));
        TRY(g.CreateImage(ww, wh, GpuFormat::RG32_UINT, rw, &d.censusSecond));
        TRY(g.CreateImage(ww, wh, GpuFormat::R32_FLOAT, rw, &d.dispMain));
        TRY(g.CreateImage(ww, wh, GpuFormat::R32_FLOAT, rw, &d.dispSecond));
        TRY(g.CreateImage(ww, wh, GpuFormat::R32_FLOAT, rw, &d.dispFill));
        TRY(g.CreateImage(ww, wh, GpuFormat::R32_FLOAT, rw, &d.dispHist[0]));
        TRY(g.CreateImage(ww, wh, GpuFormat::R32_FLOAT, rw, &d.dispHist[1]));
        TRY(g.CreateImage(ww, wh, GpuFormat::R32_FLOAT, rw, &d.dispFrame[0]));
        TRY(g.CreateImage(ww, wh, GpuFormat::R32_FLOAT, rw, &d.dispFrame[1]));
        TRY(g.CreateImage(ww, wh, GpuFormat::R32_FLOAT, rw, &d.leftValid));
        TRY(g.CreateImage(ww, wh, GpuFormat::RGBA32_FLOAT, rw, &d.gfA));
        TRY(g.CreateImage(ww, wh, GpuFormat::RGBA32_FLOAT, rw, &d.gfB));
        TRY(g.CreateImage(ww, wh, GpuFormat::RGBA32_FLOAT, rw, &d.gfC));
        TRY(g.CreateImage(ww, wh, GpuFormat::R32_FLOAT, rw, &d.subjDisp));
        TRY(g.CreateImage(ww, wh, GpuFormat::R32_FLOAT, rw, &d.subjCost[0]));
        TRY(g.CreateImage(ww, wh, GpuFormat::R32_FLOAT, rw, &d.subjCost[1]));
        TRY(g.CreateImage(ww, wh, GpuFormat::R8_UNORM, rw, &d.subjShare[0]));
        TRY(g.CreateImage(ww, wh, GpuFormat::R8_UNORM, rw, &d.subjShare[1]));
        TRY(g.CreateBuffer(ww * wh * kNumDisp * 2, false, &d.sum));
        TRY(g.CreateBuffer(kMeterCellsX * kMeterCellsY * 12, true, &d.meter));
    }
    TRY(g.CreateImage(ow / 2, oh / 2, GpuFormat::RGBA16_FLOAT, rw, &d.bokehHalf));
    TRY(g.CreateImage(ow, oh, GpuFormat::R8_UNORM, rwBack, &d.outY));
    TRY(g.CreateImage(ow / 2, oh / 2, GpuFormat::RG8_UNORM, rwBack, &d.outUV));
    TRY(g.CreateImage(ow / 2, oh, GpuFormat::RGBA8_UNORM, rwBack, &d.outYuy2));
    if (!m_stereo.mono) TRY(g.CreateImage(ow, oh, GpuFormat::R8_UNORM, rwBack, &d.depthPlane));

    TRY(g.CreateBuffer(2 * kNumDisp * 4, true, &d.hist));  // measured points, then all (histogram.hlsl)
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
    RestartFocus();
    // The auto brightness measures afresh (another stream, other light); the gain itself stays as
    // where it starts from.
    m_gainHistoryNext = m_gainHistoryCount = 0;
    m_gainTarget = -1;
    m_gainSettling = false;
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
        // The sharp zone is a depth in centimetres, which in disparity grows with the square of the
        // subject's disparity: the ranges are set for a subject at kRangeDisparity (~70 cm away).
        // (Within 0.3..2.5 of that: disparities ~13..38, ~1.3 m to ~45 cm.)
        const float focus = m_focus >= 0 ? m_focus : kDefaultFocus;
        const float depthScale = std::clamp(focus * focus / (kRangeDisparity * kRangeDisparity), 0.3f, 2.5f);
        c.focusRange = std::max(0.0f, s->focusRange) * depthScale;
        c.subjectRange = std::max(0.0f, s->subjectRange) * depthScale;
        c.temporalAlpha = m_haveHistory ? std::clamp(s->temporal, 0.05f, 1.0f) : 1.0f;
        c.highlightGain = std::max(0.0f, s->highlights);
        c.mode = static_cast<uint32_t>(s->mode);
        c.focusDisp = focus;
        c.outFormat = static_cast<uint32_t>(o.format);
        c.lumaGain = s->autoBrightness ? m_gain : 1.0f;
        c.depthView = s->depthView;
        // Strongest setting: a still pixel keeps 12% of each new frame (noise std / ~4 once settled).
        const float denoise = std::clamp(s->denoise, 0.0f, 1.0f);
        c.denoiseKeep = 1.0f - 0.88f * denoise;
        c.denoiseSpatial = denoise;
        c.denoiseHistory = !m_haveClean ? 0 : m_motionReady ? 2 : 1;
        c.noiseLevel = m_noise >= 0 ? m_noise : 0.0f;
        // Sharpening leaves alone detail no larger than the noise left in the picture, so that it
        // brings out edges rather than grain: ~2.5 noiseLevel after a full noise reduction (~2
        // levels in daylight, ~9 in a dim room, where it all but stops), up to 5 without one. While
        // the noise is not known (the noise reduction off or just started), so much that it hardly
        // sharpens.
        c.sharpen = std::clamp(s->sharpen, 0.0f, 1.0f);
        c.sharpenCore = m_noise >= 0 ? m_noise * (5.0f - 2.5f * denoise) : 0.03f;
    }
    m_gpu->SetConstants(&c, sizeof(c));
}

void StereoPipeline::RunDepth(const EffectSettings& s)
{
    auto& g = *m_gpu;
    auto& d = *m_impl;
    const uint32_t ww = m_workW, wh = m_workH;

    // The last depth frame's work luma stays for the hole fill (it matters only where there is history).
    std::swap(d.workMain, d.workPrev);
    g.Dispatch(Kernel::Downscale, {d.mainYuv, d.secondYuv}, {d.workMain, d.workSecond}, DivUp(ww, 16), DivUp(wh, 8));
    g.Dispatch(Kernel::Census, {d.workMain, d.workSecond}, {d.censusMain, d.censusSecond}, DivUp(ww, 16), DivUp(wh, 8));

    for (uint32_t dir = 0; dir < 4; ++dir) {
        UpdateConstants(&s, dir);
        g.Dispatch(Kernel::Aggregate, {d.censusMain, d.censusSecond, d.workMain}, {d.sum}, dir < 2 ? wh : ww, 1);
    }

    g.Dispatch(Kernel::Wta, {d.sum, d.workMain, d.censusMain, d.censusSecond}, {d.dispMain, d.dispSecond}, DivUp(ww, 16),
        DivUp(wh, 8));

    uint32_t cur = d.histIndex ^ 1, prev = d.histIndex;
    const bool hadHistory = m_haveHistory;
    if (!m_haveHistory) g.ClearFloat(d.dispHist[prev], -1);
    // The hole fill reads the previous frame's silhouette (where a drop is the subject's own); one
    // that is not from the frame just before (none made, or the bokeh just switched on) reads as none.
    if (!(s.subjectRange > 0 && m_frame == m_lastShareFrame + 1)) g.ClearFloat(d.Share(), 0);
    g.Dispatch(Kernel::LrFill, {d.dispMain, d.dispSecond}, {d.dispFill, d.leftValid}, DivUp(wh, 64), 1);
    g.Dispatch(Kernel::HoleFill, {d.dispFill, d.dispHist[prev], d.workMain, d.workPrev, d.dispFrame[prev], d.Share()},
        {d.dispHist[cur], d.dispFrame[cur]}, DivUp(ww, 16), DivUp(wh, 8));
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

    // The subject's silhouette: two rounds of the four scan directions (each from one cost image
    // into the other; an even count ends in subjCost[0]), then its share, blended with the previous
    // frame's while the depth history goes on.
    if (s.subjectRange > 0) {
        UpdateConstants(&s, 0);
        g.Dispatch(Kernel::SubjectSeed, {d.workMain, d.gfC}, {d.subjDisp, d.subjCost[0]}, DivUp(ww, 16), DivUp(wh, 8));
        for (uint32_t sweep = 0; sweep < 8; ++sweep) {
            const uint32_t dir = sweep % 4;
            UpdateConstants(&s, dir);
            g.Dispatch(Kernel::SubjectSweep, {d.subjDisp, d.subjCost[sweep % 2]}, {d.subjCost[(sweep + 1) % 2]},
                DivUp(dir < 2 ? wh : ww, 64), 1);
        }
        const bool blend = hadHistory && m_frame == m_lastShareFrame + 1;
        UpdateConstants(&s, blend ? 1 : 0);
        const uint32_t share = d.shareIndex ^ 1;
        g.Dispatch(Kernel::SubjectShare, {d.subjCost[0], d.subjShare[d.shareIndex]}, {d.subjShare[share]},
            DivUp(ww, 16), DivUp(wh, 8));
        d.shareIndex = share;
        m_lastShareFrame = m_frame;
        UpdateConstants(&s, 0);
    }
}

void StereoPipeline::RestartFocus()
{
    m_focusFrames = 0;
    m_focusPeak = -1;
    m_focusCandidate = -1;
    m_focusCandidateFrames = 0;
    m_focusTarget = -1;
    m_focusAcquired = false;
}

void StereoPipeline::UpdateFocus(const uint32_t* histogram, const EffectSettings& s)
{
    if (!s.autoFocus) {
        m_focus = std::clamp(s.manualFocus, 0.0f, 1.0f) * (kNumDisp - 1);
        RestartFocus();  // autofocus switched back on acquires the subject anew
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
    // While a stream starts, the depth of a person (less texture than a wall, noisier in a dim
    // room) settles over the first frames, and the first pick may be the wall behind: during
    // acquisition the focus follows the pick at once, without the hold and the glide below.
    const bool acquiring = m_focusFrames < kFocusAcquireFrames;
    if (acquiring) ++m_focusFrames;
    auto share = [&](int i) {
        double mass = 0;
        for (int k = std::max(2, i - 2); k <= std::min<int>(kNumDisp - 1, i + 2); ++k) mass += histogram[k];
        return mass / total;
    };
    auto isPeak = [&](int i) { return smooth[i] >= smooth[i - 1] && smooth[i] >= smooth[i + 1]; };
    // A subject stands out of what lies around it in depth: its peak rises kFocusProminence times
    // above the valleys that part it from anything as high on either side (or from the ends of the
    // range), however broad the subject's own peak is. A desk, a shelf or a wall at many depths gives
    // a comb of small peaks with shallow valleys between them, which swap places from frame to frame.
    auto prominent = [&](int i) {
        float left = smooth[i], right = smooth[i];
        for (int k = i - 1; k >= 2 && smooth[k] <= smooth[i]; --k) left = std::min(left, smooth[k]);
        for (int k = i + 1; k < int(kNumDisp) && smooth[k] <= smooth[i]; ++k) right = std::min(right, smooth[k]);
        return smooth[i] >= kFocusProminence * std::max(left, right);
    };
    // Nearest prominent peak that holds a meaningful share of the central area: the subject. A
    // subject off-centre or far away fills only ~10% of the weighted area, hence the low share.
    // While acquiring, the nearest peak with that share even if it does not stand out: a person a
    // few steps away stands close to the wall, and their peak is a shoulder on the wall's.
    int pick = -1, nearest = -1, globalMax = 2;
    for (int i = kNumDisp - 2; i >= 2; --i) {
        if (smooth[i] > smooth[globalMax]) globalMax = i;
        if (!isPeak(i) || share(i) < 0.07) continue;
        if (nearest < 0) nearest = i;
        if (pick < 0 && prominent(i)) pick = i;
    }
    if (acquiring && nearest >= 0) pick = nearest;
    // The peak being followed, if it still is one: the prominent local maximum next to it, which
    // moves with the subject. Without one there the focus keeps to the bin it follows while that
    // still holds a share (a subject that does not stand out, sitting against a sofa); a plain local
    // maximum is not followed, since on a slope or a comb of small peaks the mean around it moved up
    // a little every frame and the focus crept along. -1: the subject is gone.
    int followed = -1;
    if (!acquiring && m_focusPeak >= 2) {
        for (int k = std::max(2, m_focusPeak - 1); k <= std::min<int>(kNumDisp - 2, m_focusPeak + 1); ++k)
            if (isPeak(k) && prominent(k) && (followed < 0 || smooth[k] > smooth[followed])) followed = k;
        if (followed < 0) followed = m_focusPeak;
        if (share(followed) < 0.05) followed = -1;
    }
    // No prominent peak (nobody in front of the camera, a desk or a wall sloping away): the focus
    // stays with the subject it follows, or where it is (finishing a glide under way), rather than
    // chasing whatever is largest, which wandered from frame to frame.
    if (pick < 0) {
        if (acquiring || m_focus < 0) {
            pick = globalMax;
        } else if (followed >= 0) {
            pick = followed;
        } else {
            m_focusCandidate = -1;
            m_focusCandidateFrames = 0;
            GlideFocus(m_focusTarget, false);
            return;
        }
    }
    if (followed >= 0) m_focusPeak = followed;
    // While the subject being followed is still there, another peak (a hand raised towards the
    // camera, a share hovering around the threshold) takes over only after kFocusSwitchFrames. A
    // farther one (the wall behind, standing out more than a person in front of it) does not take
    // over at all while a local peak next to the followed bin still holds kFocusHoldShare of the
    // centre and is the subject acquired at the start: a person, not the tail of a wall's peak or a
    // tooth of a desk, nor a hand that took over later and rests on the desk, which give way as before.
    bool subjectHolds = false;
    if (followed >= 0 && m_focusAcquired)
        for (int k = std::max(2, followed - 1); k <= std::min<int>(kNumDisp - 2, followed + 1); ++k)
            subjectHolds = subjectHolds || (isPeak(k) && share(k) >= kFocusHoldShare);
    if (!acquiring && subjectHolds && pick < m_focusPeak - 2) {
        m_focusCandidate = -1;
        m_focusCandidateFrames = 0;
        pick = m_focusPeak;
    } else if (!acquiring && m_focusPeak >= 2 && std::abs(pick - m_focusPeak) > 2 && share(m_focusPeak) >= 0.05) {
        if (std::abs(pick - m_focusCandidate) <= 2) ++m_focusCandidateFrames;
        else m_focusCandidateFrames = 1;
        m_focusCandidate = pick;
        if (m_focusCandidateFrames < kFocusSwitchFrames) pick = m_focusPeak;
    }
    if (pick != m_focusPeak) {
        m_focusCandidate = -1;
        m_focusCandidateFrames = 0;
    }
    if (acquiring) m_focusAcquired = true;
    else if (std::abs(pick - m_focusPeak) > 2) m_focusAcquired = false;  // another subject took over
    // The focus value from every point around the subject's peak (the second histogram), in the
    // disparity the blur compares with.
    const uint32_t* all = histogram + kNumDisp;
    double wsum = 0, dsum = 0;
    for (int k = std::max(2, pick - 2); k <= std::min<int>(kNumDisp - 1, pick + 2); ++k) {
        wsum += all[k];
        dsum += double(all[k]) * k;
    }
    m_focusPeak = pick;  // a bin of the measured histogram, where the next frame looks for it
    m_focusTarget = wsum > 0 ? float(dsum / wsum) : float(pick);
    GlideFocus(m_focusTarget, acquiring);
}

void StereoPipeline::GlideFocus(float target, bool atOnce)
{
    if (target < 0) return;
    if (m_focus < 0 || atOnce)
        m_focus = target;
    else if (std::fabs(target - m_focus) > 0.6f)
        m_focus += (target - m_focus) * 0.15f;  // glide, like a lens refocusing
}

StereoPipeline::SubjectLight StereoPipeline::MeasureSubject(const float* cells)
{
    // The subject is what the bokeh keeps sharp; its head is the top of it: the columns of its top
    // two rows (one cell more either side, the crown being narrower than the face) and, from its
    // first row, as many rows as those columns are wide (a head is about that tall in square cells),
    // at most 40% of the way down to its last (head and shoulders, as a webcam frames a person), at
    // least two. Shoulders and a light shirt beside or below the head stay out.
    SubjectLight light;
    float rowWeight[kMeterCellsY] = {};
    float total = 0;
    for (uint32_t y = 0; y < kMeterCellsY; ++y) {
        for (uint32_t x = 0; x < kMeterCellsX; ++x) rowWeight[y] += cells[(y * kMeterCellsX + x) * 3];
        total += rowWeight[y];
    }
    const float coverage = total / float(kMeterCellsX * kMeterCellsY * kMeterSamples);
    const float rowMin = 0.08f * kMeterCellsX * kMeterSamples;  // ~2.5 cells of a row
    int top = -1, bottom = -1;
    for (uint32_t y = 0; y < kMeterCellsY; ++y) {
        if (rowWeight[y] < rowMin) continue;
        if (top < 0) top = int(y);
        bottom = int(y);
    }
    if (top < 0 || coverage < 0.02f) return light;
    const auto weightAt = [&](int x, int y) { return cells[(y * kMeterCellsX + x) * 3]; };
    int colMin = int(kMeterCellsX), colMax = -1;
    for (int y = top; y <= std::min(top + 1, bottom); ++y) {
        for (int x = 0; x < int(kMeterCellsX); ++x) {
            if (weightAt(x, y) < kHeadColumn * kMeterSamples) continue;
            colMin = std::min(colMin, x);
            colMax = std::max(colMax, x);
        }
    }
    if (colMax < 0) return light;  // only slivers of subject on top: no head to meter
    colMin = std::max(0, colMin - 1);
    colMax = std::min(int(kMeterCellsX) - 1, colMax + 1);
    const int maxRows = std::max(2, int(std::lround(0.4 * (bottom - top + 1))));
    const int headRows = std::clamp(colMax - colMin + 1, 2, maxRows);
    // The head's light is its face's: the brighter side (kHeadPercentile by subject weight) of its
    // cells that lie wholly on the subject, not their mean, which hair and headphones, darker and
    // counted in or out by the row, pull down, nor the edge cells, which take in some of what lies
    // behind (a window behind a backlit face). Without whole cells the edge ones count.
    std::array<std::pair<float, float>, kMeterCellsX * kMeterCellsY> head;  // (cell luma, subject weight)
    for (const float minWeight : {kHeadCell * kMeterSamples, 0.0f}) {
        size_t count = 0;
        double weight = 0;
        for (int y = top; y < std::min<int>(top + headRows, kMeterCellsY); ++y) {
            for (int x = colMin; x <= colMax; ++x) {
                const float w = weightAt(x, y), luma = cells[(y * kMeterCellsX + x) * 3 + 1] / w;
                if (!(w > minWeight) || !std::isfinite(luma)) continue;
                head[count++] = {luma, w};
                weight += w;
            }
        }
        if (count == 0) continue;
        std::sort(head.begin(), head.begin() + count);
        // Where the cumulative weight reaches the percentile, between that cell's luma and the one
        // before it, so that a small shift of the weights moves the measurement a little.
        const double want = weight * kHeadPercentile;
        double acc = 0;
        float before = head[0].first;
        for (size_t i = 0; i < count; ++i) {
            const auto [luma, w] = head[i];
            if (acc + w >= want) {
                light.headLuma = before + (luma - before) * float((want - acc) / w);
                break;
            }
            acc += w;
            before = luma;
        }
        light.valid = true;
        break;
    }
    if (!light.valid) return light;
    // A small subject (far away, or the focus on a patch of wall) counts less, and so does one
    // filling most of the picture (an empty room or a wall in focus, not a person).
    auto smooth = [](float a, float b, float x) {
        const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
        return t * t * (3 - 2 * t);
    };
    light.confidence = smooth(0.02f, 0.06f, coverage) * (1 - smooth(0.5f, 0.8f, coverage));
    return light;
}

void StereoPipeline::UpdateGain(const uint32_t* h, const EffectSettings& s, const SubjectLight& subject)
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
    const double maxGain = std::max(1.0f, s.maxGain);
    double target = std::min({1.0 / whiteLevel, kFrameTarget / std::max(mean, 1e-3), maxGain});
    // With depth the exposure is the subject's: its head at kSubjectTarget whatever the rest of the
    // picture does, so a face against a bright window is not left dark (the window goes white under
    // the soft shoulder of composite.hlsl) and a face lit by a screen in a dark room is not blown out.
    if (subject.valid) {
        const double onSubject = std::min(kSubjectTarget / std::max<double>(subject.headLuma, 1e-3), maxGain);
        target += (onSubject - target) * subject.confidence;
    }
    target = std::max(1.0, target);
    // Like a camera's AE, steady: the measurement swings from frame to frame (the white point steps
    // between two bins, the head's cells shift), so it is the middle of the last kGainHistory
    // measurements that counts (the mean of the middle three, the median until there are that
    // many), smoothed further, and the gain moves only once that is off by more than kGainStart, then
    // settles over about half a second to within kGainStop. A change that persists for half the
    // history and is large (a start, the light switched) is taken at once; a lowered maxGain holds at once.
    m_gainHistory[m_gainHistoryNext] = float(target);
    m_gainHistoryNext = (m_gainHistoryNext + 1) % kGainHistory;
    m_gainHistoryCount = std::min(m_gainHistoryCount + 1, kGainHistory);
    std::array<float, kGainHistory> recent = m_gainHistory;
    std::sort(recent.begin(), recent.begin() + m_gainHistoryCount);
    const uint32_t mid = m_gainHistoryCount / 2;
    const float measured =
        m_gainHistoryCount == kGainHistory ? (recent[mid - 1] + recent[mid] + recent[mid + 1]) / 3.0f : recent[mid];
    if (m_gainTarget < 0 || std::fabs(measured / m_gainTarget - 1.0f) > kGainJump) m_gainTarget = measured;
    else m_gainTarget += (measured - m_gainTarget) * 0.15f;
    m_gainTarget = std::min(m_gainTarget, float(maxGain));
    if (std::fabs(m_gainTarget / m_gain - 1.0f) > kGainStart) m_gainSettling = true;
    if (m_gainSettling) {
        m_gain += (m_gainTarget - m_gain) * 0.08f;
        if (std::fabs(m_gainTarget / m_gain - 1.0f) < kGainStop) m_gainSettling = false;
    }
    m_gain = std::clamp(m_gain, 1.0f, float(maxGain));
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
    // Motion compensation needs this frame's vectors and the previous frame's to check them against.
    m_motionReady = denoise && m_haveClean && s.motionCompensation && m_frame == m_lastMotionFrame + 1;
    // The noise is measured by the noise reduction only: without it a level measured earlier, in
    // other light, would mislead the sharpening.
    if (!denoise) m_noise = -1;

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
        const uint32_t ew = m_stereo.eyeWidth, eh = m_stereo.eyeHeight;
        const uint32_t motionCur = d.motionIndex ^ 1;
        if (m_haveClean && s.motionCompensation) {
            g.Dispatch(Kernel::MotionDown, {d.mainYuv, d.clean[d.cleanIndex]}, {d.smallCur, d.smallPrev},
                DivUp(ew / 4, 16), DivUp(eh / 4, 8));
            g.Dispatch(Kernel::MotionSearch, {d.smallCur, d.smallPrev}, {d.motionCoarse}, DivUp(ew, kMotionBlock),
                DivUp(eh, kMotionBlock));
            g.Dispatch(Kernel::MotionRefine, {d.mainYuv, d.clean[d.cleanIndex], d.motionCoarse},
                {d.motion[motionCur]}, DivUp(ew, kMotionBlock), DivUp(eh, kMotionBlock));
            d.motionIndex = motionCur;
            m_lastMotionFrame = m_frame;
        }
        g.ClearUint(d.noiseHist);
        g.Dispatch(Kernel::Denoise, {d.mainYuv, d.clean[d.cleanIndex], d.motion[motionCur], d.motion[motionCur ^ 1]},
            {d.clean[cur], d.noiseHist}, DivUp(ew, 16), DivUp(eh, 8));
        if (d.slots[slot].denoise) g.CopyToReadback(d.noiseHist, slot);
        d.cleanIndex = cur;
        m_haveClean = true;
        m_lastDenoiseFrame = m_frame;
        image = d.clean[cur];
    }

    const bool needDepth = s.mode == ViewMode::Bokeh || s.mode == ViewMode::Depth || s.mode == ViewMode::DebugRaw ||
                           s.mode == ViewMode::DebugFilled || s.mode == ViewMode::DebugHoles ||
                           s.mode == ViewMode::DebugGrey || s.mode == ViewMode::DebugBlend ||
                           (s.depthPlane && !m_stereo.mono);
    d.slots[slot].depth = needDepth;
    if (needDepth) {
        if (m_frame != m_lastDepthFrame + 1) {  // depth was off: its history and the focus are stale
            m_haveHistory = false;
            RestartFocus();
        }
        m_lastDepthFrame = m_frame;
        RunDepth(s);
        UpdateConstants(&s, 0);
        g.ClearUint(d.hist);
        g.Dispatch(Kernel::Histogram, {image, d.gfC, d.dispFill}, {d.hist}, DivUp(ow / 4, 16), DivUp(oh / 4, 8));
        g.CopyToReadback(d.hist, slot);
    }
    // With depth the auto brightness meters the subject (meter.hlsl: 32x18 cells, 8x8 per group).
    d.slots[slot].meter = needDepth && s.autoBrightness;
    if (d.slots[slot].meter) {
        g.Dispatch(Kernel::Meter, {image, d.gfC, d.Share()}, {d.meter}, DivUp(kMeterCellsX, 8),
            DivUp(kMeterCellsY, 8));
        g.CopyToReadback(d.meter, slot);
    }
    if (s.mode == ViewMode::Bokeh)
        g.Dispatch(Kernel::Bokeh, {image, d.gfC, d.Share()}, {d.bokehHalf}, DivUp(ow / 2, 16), DivUp(oh / 2, 8));
    const bool packedOut = m_output.format == PixelFormat::YUY2;
    g.Dispatch(Kernel::Composite,
        {image, d.gfC, d.bokehHalf, d.secondYuv, d.dispMain, d.dispFill, d.dispHist[d.histIndex], d.Share()},
        {d.outY, d.outUV, d.outYuy2}, DivUp(ow / 2, 16), DivUp(oh / 2, 8));
    d.slots[slot].depthPlane = s.depthPlane && !m_stereo.mono;
    if (d.slots[slot].depthPlane) {
        g.Dispatch(Kernel::DepthOut, {image, d.gfC, d.Share()}, {d.depthPlane}, DivUp(ow, 16), DivUp(oh, 8));
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
        if (m.rowPitch < rowBytes) {
            g.Unmap(image, read);
            return E_FAIL;
        }
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
    SubjectLight subject;
    if (d.slots[read].meter) {
        GpuMapped m;
        if (SUCCEEDED(g.Map(d.meter, read, &m))) {
            subject = MeasureSubject(reinterpret_cast<const float*>(m.data));
            g.Unmap(d.meter, read);
        }
    }
    m_subjectLuma = subject.valid && subject.confidence > 0 ? subject.headLuma : 0.0f;
    if (d.slots[read].brightness) withHistogram(d.lumaHist, [&](const uint32_t* h) { UpdateGain(h, s, subject); });
    if (d.slots[read].depth) withHistogram(d.hist, [&](const uint32_t* h) { UpdateFocus(h, s); });
    if (d.slots[read].denoise) withHistogram(d.noiseHist, [&](const uint32_t* h) { UpdateNoise(h); });

    if (stats) {
        float ms = 0;
        if (g.TimingMs(read, &ms)) stats->gpuMs = ms;
        stats->focusDisparity = m_focus;
        stats->gain = s.autoBrightness ? m_gain : 1.0f;
        stats->noise = std::max(m_noise, 0.0f);
        stats->subjectLuma = m_subjectLuma;
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
    // In workPrev, which the next RunDepth swaps in and overwrites: workMain keeps the last depth
    // frame's luma, which the hole fill compares the next frame with.
    g.Dispatch(Kernel::Downscale, {d.mainYuv, d.secondYuv}, {d.workPrev, d.workSecond}, DivUp(m_workW, 16),
        DivUp(m_workH, 8));
    g.Dispatch(Kernel::Census, {d.workPrev, d.workSecond}, {d.censusMain, d.censusSecond}, DivUp(m_workW, 16),
        DivUp(m_workH, 8));
    g.ClearUint(d.scoreBuf);
    g.Dispatch(Kernel::Score, {d.censusMain, d.censusSecond, d.workPrev}, {d.scoreBuf}, DivUp(m_workW, 16),
        DivUp(m_workH, 8));
    g.Unbind();
    g.CopyToReadback(d.scoreBuf, 0);
    GpuMapped m;
    if (FAILED(g.Map(d.scoreBuf, 0, &m))) return 1e9f;
    const uint32_t* v = reinterpret_cast<const uint32_t*>(m.data);
    float score = v[1] >= kCalibrationPixels ? float(v[0]) / v[1] : 1e9f;
    // The curve the calibration decides on, for tools/calcurve.py (bench).
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996)  // getenv: read only, nothing to free
#endif
    static const char* traceEnv = std::getenv("PS5CAM_DEBUGCAL");
    static const bool trace = traceEnv && *traceEnv && *traceEnv != '0';
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    if (trace)
        std::fprintf(stderr, "CAL dy %.2f rot %.2f mean %.4f n %u decisive %u\n", dy, rotationDeg,
            v[1] ? float(v[0]) / v[1] : 0.0f, v[1], v[2]);
    g.Unmap(d.scoreBuf, 0);
    return score;
}

HRESULT StereoPipeline::Calibrate(const uint8_t* yuy2, uint32_t yuy2Pitch, Rectification* result)
{
    std::lock_guard lock(m_lock);
    if (result) result->contrast = -1;
    if (!m_impl || m_stereo.mono) return E_NOT_VALID_STATE;
    auto& d = *m_impl;
    Rectification saved = m_rect;
    Upload(yuy2, yuy2Pitch);
    UpdateConstants(nullptr, 0);
    m_gpu->Dispatch(Kernel::Unpack, {d.packed}, {d.mainYuv, d.secondYuv}, DivUp(m_stereo.eyeWidth / 2, 16),
        DivUp(m_stereo.eyeHeight, 8));

    // Bail out cheaply on dark or featureless frames (the pixels counted come from the main sensor
    // alone, so their number is the same at every offset).
    const float atZero = ScoreAlignment(0, 0);
    if (atZero >= 1e8f) {
        m_rect = saved;
        return E_FAIL;
    }
    // Coarse vertical sweep, then a joint refinement of offset and roll around the best value.
    float bestDy = 0, bestRot = 0, best = 1e9f;
    std::vector<float> sweep;
    for (float dy = -12; dy <= 12; dy += 1.0f) {
        float sc = dy == 0 ? atZero : ScoreAlignment(dy, 0);
        sweep.push_back(sc);
        if (sc < best) best = sc, bestDy = dy;
    }
    // Noise-only frames give a flat curve; require a clear minimum, inside the sweep, before trusting
    // the result. (Real frames, counting only well textured pixels: 13-15% below the median at the
    // right offset; a desk right in front of the camera, nearer than the disparity range: 2.5%.)
    std::nth_element(sweep.begin(), sweep.begin() + sweep.size() / 2, sweep.end());
    const float median = sweep[sweep.size() / 2];
    const float contrast = median > 0 && median < 1e8f ? (median - best) / median : 0.0f;
    if (result) result->contrast = std::fabs(bestDy) >= 12 ? -2 : contrast;
    if (contrast < kCalibrationContrast || std::fabs(bestDy) >= 12) {
        m_rect = saved;
        return E_FAIL;
    }
    float coarseDy = bestDy;
    for (float dy = coarseDy - 1; dy <= coarseDy + 1.001f; dy += 0.25f)
        for (float rot = -1.0f; rot <= 1.001f; rot += 0.25f) {
            float sc = ScoreAlignment(dy, rot);
            if (sc < best) best = sc, bestDy = dy, bestRot = rot;
        }
    m_rect = {bestDy, bestRot, best, contrast};
    m_haveHistory = false;
    RestartFocus();  // the disparities shift with the new alignment
    if (result) *result = m_rect;
    return S_OK;
}

}  // namespace ps5cam
