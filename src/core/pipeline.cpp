#include "pipeline.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "shaders/aggregate.h"
#include "shaders/bokeh.h"
#include "shaders/census.h"
#include "shaders/composite.h"
#include "shaders/downscale.h"
#include "shaders/guided_box.h"
#include "shaders/guided_coef.h"
#include "shaders/guided_prep.h"
#include "shaders/histogram.h"
#include "shaders/lrfill.h"
#include "shaders/lumastats.h"
#include "shaders/score.h"
#include "shaders/unpack.h"
#include "shaders/wta.h"

using Microsoft::WRL::ComPtr;

namespace ps5cam {

namespace {

constexpr uint32_t kNumDisp = 64;

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
    float pad;
    uint32_t secondW, secondH, secondFolded, depthMirror;
};
static_assert(sizeof(GpuConstants) == 160, "constant buffer layout");
// kNumDisp mirrors MAX_DISP in shaders/common.hlsli.

uint32_t DivUp(uint32_t a, uint32_t b) { return (a + b - 1) / b; }

struct Tex {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
};

}  // namespace

struct StereoPipeline::Impl {
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11SamplerState> linearClamp;
    ComPtr<ID3D11ComputeShader> unpack, downscale, census, aggregate, wta, lrfill, guidedPrep, guidedBox, guidedCoef,
        histogram, bokeh, composite, score, lumastats;

    Tex packed;                    // R8G8B8A8_UINT, YUY2 texels
    Tex mainYuv, secondYuv;        // each sensor at its own size (eyeWidth x eyeHeight, SecondWidth x SecondHeight)
    Tex workMain, workSecond;      // R32_FLOAT work res
    Tex censusMain, censusSecond;  // R32G32_UINT work res
    Tex dispMain, dispSecond;      // R32_FLOAT work res
    Tex dispHist[2];               // filtered disparity, ping-pong for temporal smoothing
    Tex leftValid;
    Tex gfA, gfB, gfC;             // R32G32B32A32_FLOAT work res
    Tex bokehHalf;                 // R16G16B16A16_FLOAT out/2
    Tex outY, outUV;               // R8 / R8G8 out res
    Tex outYuy2;                   // R8G8B8A8 out/2 x out (packed YUY2)
    // Readback ring: the GPU fills slot N while the CPU reads slot N-1.
    ComPtr<ID3D11Texture2D> outYStaging[2], outUVStaging[2], outYuy2Staging[2];

    ComPtr<ID3D11Buffer> sum;  // raw, uint16 per (pixel, disparity)
    ComPtr<ID3D11UnorderedAccessView> sumUav;
    ComPtr<ID3D11ShaderResourceView> sumSrv;
    ComPtr<ID3D11Buffer> hist, histStaging[2], scoreBuf, scoreStaging, lumaHist, lumaHistStaging[2];
    ComPtr<ID3D11UnorderedAccessView> histUav, scoreUav, lumaHistUav;

    ComPtr<ID3D11Query> disjoint[2], tsBegin[2], tsEnd[2];
    struct Slot {
        bool depth = false, brightness = false;
    } slots[2];
    int pending = -1;  // slot holding a submitted frame that has not been read back yet
    uint32_t histIndex = 0;  // which dispHist is current
    GpuConstants c = {};
};

StereoPipeline::StereoPipeline() = default;

StereoPipeline::~StereoPipeline()
{
    delete m_impl;
}

namespace {

HRESULT MakeTex(ID3D11Device* dev, uint32_t w, uint32_t h, DXGI_FORMAT fmt, bool uav, Tex& out)
{
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | (uav ? D3D11_BIND_UNORDERED_ACCESS : 0);
    HRESULT hr = dev->CreateTexture2D(&d, nullptr, &out.tex);
    if (FAILED(hr)) return hr;
    hr = dev->CreateShaderResourceView(out.tex.Get(), nullptr, &out.srv);
    if (FAILED(hr)) return hr;
    if (uav) hr = dev->CreateUnorderedAccessView(out.tex.Get(), nullptr, &out.uav);
    return hr;
}

HRESULT MakeStaging(ID3D11Device* dev, uint32_t w, uint32_t h, DXGI_FORMAT fmt, ComPtr<ID3D11Texture2D>& out)
{
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    return dev->CreateTexture2D(&d, nullptr, &out);
}

HRESULT MakeRawBuffer(ID3D11Device* dev, uint32_t bytes, bool srv, ComPtr<ID3D11Buffer>& buf,
    ComPtr<ID3D11UnorderedAccessView>& uav, ComPtr<ID3D11ShaderResourceView>* srvOut, ComPtr<ID3D11Buffer>* staging)
{
    D3D11_BUFFER_DESC d = {};
    d.ByteWidth = bytes;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_UNORDERED_ACCESS | (srv ? D3D11_BIND_SHADER_RESOURCE : 0);
    d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    HRESULT hr = dev->CreateBuffer(&d, nullptr, &buf);
    if (FAILED(hr)) return hr;
    D3D11_UNORDERED_ACCESS_VIEW_DESC u = {};
    u.Format = DXGI_FORMAT_R32_TYPELESS;
    u.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    u.Buffer.NumElements = bytes / 4;
    u.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    hr = dev->CreateUnorderedAccessView(buf.Get(), &u, &uav);
    if (FAILED(hr)) return hr;
    if (srvOut) {
        D3D11_SHADER_RESOURCE_VIEW_DESC s = {};
        s.Format = DXGI_FORMAT_R32_TYPELESS;
        s.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        s.BufferEx.NumElements = bytes / 4;
        s.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        hr = dev->CreateShaderResourceView(buf.Get(), &s, &*srvOut);
        if (FAILED(hr)) return hr;
    }
    if (staging) {
        D3D11_BUFFER_DESC sd = {};
        sd.ByteWidth = bytes;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        hr = dev->CreateBuffer(&sd, nullptr, &*staging);
    }
    return hr;
}

}  // namespace

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

    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, ARRAYSIZE(levels),
        D3D11_SDK_VERSION, &m_device, nullptr, &m_ctx);
    if (FAILED(hr)) return hr;
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(m_ctx.As(&mt))) mt->SetMultithreadProtected(TRUE);
    delete m_impl;
    m_impl = new Impl();
    hr = CreateResources();
    if (FAILED(hr)) {
        delete m_impl;
        m_impl = nullptr;
        m_ctx.Reset();
        m_device.Reset();
    }
    return hr;
}

HRESULT StereoPipeline::CreateResources()
{
    auto* dev = m_device.Get();
    auto& d = *m_impl;
    HRESULT hr = S_OK;
#define TRY(x)               \
    do {                     \
        hr = (x);            \
        if (FAILED(hr)) return hr; \
    } while (0)
#define CS(name, blob) TRY(dev->CreateComputeShader(blob, sizeof(blob), nullptr, &d.name))
    CS(unpack, g_unpack);
    CS(downscale, g_downscale);
    CS(census, g_census);
    CS(aggregate, g_aggregate);
    CS(wta, g_wta);
    CS(lrfill, g_lrfill);
    CS(guidedPrep, g_guided_prep);
    CS(guidedBox, g_guided_box);
    CS(guidedCoef, g_guided_coef);
    CS(histogram, g_histogram);
    CS(bokeh, g_bokeh);
    CS(composite, g_composite);
    CS(score, g_score);
    CS(lumastats, g_lumastats);
#undef CS

    D3D11_BUFFER_DESC cb = {};
    cb.ByteWidth = sizeof(GpuConstants);
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    TRY(dev->CreateBuffer(&cb, nullptr, &d.constants));

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    TRY(dev->CreateSamplerState(&sd, &d.linearClamp));

    const uint32_t ew = m_stereo.eyeWidth, eh = m_stereo.eyeHeight;
    const uint32_t ww = m_workW, wh = m_workH;
    const uint32_t ow = m_output.width, oh = m_output.height;
    // One R8G8B8A8 texel per YUY2 pair: PackedWidth pixels = PackedWidth / 2 texels.
    TRY(MakeTex(dev, m_stereo.PackedWidth() / 2, m_stereo.PackedHeight(), DXGI_FORMAT_R8G8B8A8_UINT, false, d.packed));
    TRY(MakeTex(dev, ew, eh, DXGI_FORMAT_R8G8B8A8_UNORM, true, d.mainYuv));
    TRY(MakeTex(dev, m_stereo.SecondWidth(), m_stereo.SecondHeight(), DXGI_FORMAT_R8G8B8A8_UNORM, true, d.secondYuv));
    // Depth resources (~45 MB): a single-sensor pipeline never computes depth, so it skips them;
    // Process binds their (null) views only to shaders that do not sample them in Main view.
    if (!m_stereo.mono) {
        TRY(MakeTex(dev, ww, wh, DXGI_FORMAT_R32_FLOAT, true, d.workMain));
        TRY(MakeTex(dev, ww, wh, DXGI_FORMAT_R32_FLOAT, true, d.workSecond));
        TRY(MakeTex(dev, ww, wh, DXGI_FORMAT_R32G32_UINT, true, d.censusMain));
        TRY(MakeTex(dev, ww, wh, DXGI_FORMAT_R32G32_UINT, true, d.censusSecond));
        TRY(MakeTex(dev, ww, wh, DXGI_FORMAT_R32_FLOAT, true, d.dispMain));
        TRY(MakeTex(dev, ww, wh, DXGI_FORMAT_R32_FLOAT, true, d.dispSecond));
        TRY(MakeTex(dev, ww, wh, DXGI_FORMAT_R32_FLOAT, true, d.dispHist[0]));
        TRY(MakeTex(dev, ww, wh, DXGI_FORMAT_R32_FLOAT, true, d.dispHist[1]));
        TRY(MakeTex(dev, ww, wh, DXGI_FORMAT_R32_FLOAT, true, d.leftValid));
        TRY(MakeTex(dev, ww, wh, DXGI_FORMAT_R32G32B32A32_FLOAT, true, d.gfA));
        TRY(MakeTex(dev, ww, wh, DXGI_FORMAT_R32G32B32A32_FLOAT, true, d.gfB));
        TRY(MakeTex(dev, ww, wh, DXGI_FORMAT_R32G32B32A32_FLOAT, true, d.gfC));
        TRY(MakeRawBuffer(dev, ww * wh * kNumDisp * 2, true, d.sum, d.sumUav, &d.sumSrv, nullptr));
    }
    TRY(MakeTex(dev, ow / 2, oh / 2, DXGI_FORMAT_R16G16B16A16_FLOAT, true, d.bokehHalf));
    TRY(MakeTex(dev, ow, oh, DXGI_FORMAT_R8_UNORM, true, d.outY));
    TRY(MakeTex(dev, ow / 2, oh / 2, DXGI_FORMAT_R8G8_UNORM, true, d.outUV));
    for (int i = 0; i < 2; ++i) {
        TRY(MakeStaging(dev, ow, oh, DXGI_FORMAT_R8_UNORM, d.outYStaging[i]));
        TRY(MakeStaging(dev, ow / 2, oh / 2, DXGI_FORMAT_R8G8_UNORM, d.outUVStaging[i]));
        TRY(MakeStaging(dev, ow / 2, oh, DXGI_FORMAT_R8G8B8A8_UNORM, d.outYuy2Staging[i]));
    }
    TRY(MakeTex(dev, ow / 2, oh, DXGI_FORMAT_R8G8B8A8_UNORM, true, d.outYuy2));


    TRY(MakeRawBuffer(dev, kNumDisp * 4, false, d.hist, d.histUav, nullptr, &d.histStaging[0]));
    TRY(MakeRawBuffer(dev, 16, false, d.scoreBuf, d.scoreUav, nullptr, &d.scoreStaging));
    TRY(MakeRawBuffer(dev, kNumDisp * 4, false, d.lumaHist, d.lumaHistUav, nullptr, &d.lumaHistStaging[0]));
    {
        D3D11_BUFFER_DESC sd2 = {};
        sd2.ByteWidth = kNumDisp * 4;
        sd2.Usage = D3D11_USAGE_STAGING;
        sd2.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        TRY(dev->CreateBuffer(&sd2, nullptr, &d.histStaging[1]));
        TRY(dev->CreateBuffer(&sd2, nullptr, &d.lumaHistStaging[1]));
    }

    for (int i = 0; i < 2; ++i) {
        D3D11_QUERY_DESC q = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        TRY(dev->CreateQuery(&q, &d.disjoint[i]));
        q.Query = D3D11_QUERY_TIMESTAMP;
        TRY(dev->CreateQuery(&q, &d.tsBegin[i]));
        TRY(dev->CreateQuery(&q, &d.tsEnd[i]));
    }
#undef TRY
    return S_OK;
}

void StereoPipeline::Reset()
{
    std::lock_guard lock(m_lock);
    m_haveHistory = false;
    m_focus = -1;
    if (m_impl) m_impl->pending = -1;
}

void StereoPipeline::SetRectification(const Rectification& r)
{
    std::lock_guard lock(m_lock);
    m_rect = r;
}

void StereoPipeline::Upload(const uint8_t* yuy2, uint32_t pitch)
{
    m_ctx->UpdateSubresource(m_impl->packed.tex.Get(), 0, nullptr, yuy2, pitch, pitch * m_stereo.PackedHeight());
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
    }

    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(m_ctx->Map(m_impl->constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, &c, sizeof(c));
        m_ctx->Unmap(m_impl->constants.Get(), 0);
    }
}

namespace {

void Bind(ID3D11DeviceContext* ctx, ID3D11ComputeShader* cs, std::initializer_list<ID3D11ShaderResourceView*> srvs,
    std::initializer_list<ID3D11UnorderedAccessView*> uavs)
{
    ID3D11ShaderResourceView* nullSrv[8] = {};
    ID3D11UnorderedAccessView* nullUav[4] = {};
    ctx->CSSetShaderResources(0, 8, nullSrv);
    ctx->CSSetUnorderedAccessViews(0, 4, nullUav, nullptr);
    ctx->CSSetShader(cs, nullptr, 0);
    std::vector<ID3D11ShaderResourceView*> s(srvs);
    std::vector<ID3D11UnorderedAccessView*> u(uavs);
    if (!s.empty()) ctx->CSSetShaderResources(0, static_cast<UINT>(s.size()), s.data());
    if (!u.empty()) ctx->CSSetUnorderedAccessViews(0, static_cast<UINT>(u.size()), u.data(), nullptr);
}

}  // namespace

void StereoPipeline::RunDepth(const EffectSettings& s)
{
    auto* ctx = m_ctx.Get();
    auto& d = *m_impl;
    const uint32_t ww = m_workW, wh = m_workH;

    Bind(ctx, d.downscale.Get(), {d.mainYuv.srv.Get(), d.secondYuv.srv.Get()}, {d.workMain.uav.Get(), d.workSecond.uav.Get()});
    ctx->Dispatch(DivUp(ww, 16), DivUp(wh, 8), 1);
    Bind(ctx, d.census.Get(), {d.workMain.srv.Get(), d.workSecond.srv.Get()},
        {d.censusMain.uav.Get(), d.censusSecond.uav.Get()});
    ctx->Dispatch(DivUp(ww, 16), DivUp(wh, 8), 1);

    for (uint32_t dir = 0; dir < 4; ++dir) {
        UpdateConstants(&s, dir);
        Bind(ctx, d.aggregate.Get(), {d.censusMain.srv.Get(), d.censusSecond.srv.Get(), d.workMain.srv.Get()},
            {d.sumUav.Get()});
        ctx->Dispatch(dir < 2 ? wh : ww, 1, 1);
    }

    Bind(ctx, d.wta.Get(), {d.sumSrv.Get()}, {d.dispMain.uav.Get(), d.dispSecond.uav.Get()});
    ctx->Dispatch(DivUp(ww, 16), DivUp(wh, 8), 1);

    uint32_t cur = d.histIndex ^ 1, prev = d.histIndex;
    if (!m_haveHistory) {
        const float minusOne[4] = {-1, -1, -1, -1};
        ctx->ClearUnorderedAccessViewFloat(d.dispHist[prev].uav.Get(), minusOne);
    }
    Bind(ctx, d.lrfill.Get(), {d.dispMain.srv.Get(), d.dispSecond.srv.Get(), d.dispHist[prev].srv.Get()},
        {d.dispHist[cur].uav.Get(), d.leftValid.uav.Get()});
    ctx->Dispatch(DivUp(wh, 64), 1, 1);
    d.histIndex = cur;
    m_haveHistory = true;

    // Guided filter: means of (I, p, I^2, Ip) -> coefficients (a, b) -> their means.
    Bind(ctx, d.guidedPrep.Get(), {d.workMain.srv.Get(), d.dispHist[cur].srv.Get()}, {d.gfA.uav.Get()});
    ctx->Dispatch(DivUp(ww, 16), DivUp(wh, 8), 1);
    auto box = [&](Tex& src, Tex& tmp) {
        UpdateConstants(&s, 0);
        Bind(ctx, d.guidedBox.Get(), {src.srv.Get()}, {tmp.uav.Get()});
        ctx->Dispatch(DivUp(ww, 16), DivUp(wh, 8), 1);
        UpdateConstants(&s, 1);
        Bind(ctx, d.guidedBox.Get(), {tmp.srv.Get()}, {src.uav.Get()});
        ctx->Dispatch(DivUp(ww, 16), DivUp(wh, 8), 1);
    };
    box(d.gfA, d.gfB);
    Bind(ctx, d.guidedCoef.Get(), {d.gfA.srv.Get()}, {d.gfC.uav.Get()});
    ctx->Dispatch(DivUp(ww, 16), DivUp(wh, 8), 1);
    box(d.gfC, d.gfB);
}

void StereoPipeline::UpdateFocus(const uint32_t* histogram, const EffectSettings& s)
{
    if (!s.autoFocus) {
        m_focus = std::clamp(s.manualFocus, 0.0f, 1.0f) * (kNumDisp - 1);
        return;
    }
    double total = 0;
    float smooth[kNumDisp] = {};
    for (uint32_t i = 0; i < kNumDisp; ++i) {
        total += histogram[i];
        smooth[i] = 0.5f * histogram[i] + 0.25f * histogram[i > 0 ? i - 1 : i] +
                    0.25f * histogram[i + 1 < kNumDisp ? i + 1 : i];
    }
    if (total < 50) return;
    // Nearest peak that holds a meaningful share of the central area: the subject. Bins 0-1
    // collect unmatched pixels and are ignored.
    int pick = -1, globalMax = 2;
    for (int i = kNumDisp - 2; i >= 2; --i) {
        if (smooth[i] > smooth[globalMax]) globalMax = i;
        double mass = 0;
        for (int k = std::max(0, i - 2); k <= std::min<int>(kNumDisp - 1, i + 2); ++k) mass += histogram[k];
        bool peak = smooth[i] >= smooth[i - 1] && smooth[i] >= smooth[i + 1];
        if (pick < 0 && peak && mass / total >= 0.12) pick = i;
    }
    if (pick < 0) pick = globalMax;
    double wsum = 0, dsum = 0;
    for (int k = std::max(0, pick - 2); k <= std::min<int>(kNumDisp - 1, pick + 2); ++k) {
        wsum += histogram[k];
        dsum += double(histogram[k]) * k;
    }
    float target = wsum > 0 ? float(dsum / wsum) : float(pick);
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

HRESULT StereoPipeline::Process(const uint8_t* yuy2, uint32_t yuy2Pitch, const EffectSettings& requested, uint8_t* dst,
    uint8_t* dstUV, uint32_t dstPitch, FrameStats* stats)
{
    std::lock_guard lock(m_lock);
    if (!m_impl) return E_NOT_VALID_STATE;
    EffectSettings s = requested;
    if (m_stereo.mono) s.mode = ViewMode::Main;  // no second sensor: no depth, no side-by-side
    auto* ctx = m_ctx.Get();
    auto& d = *m_impl;
    const uint32_t ow = m_output.width, oh = m_output.height;
    const int slot = (d.pending == 0) ? 1 : 0;

    ctx->Begin(d.disjoint[slot].Get());
    ctx->End(d.tsBegin[slot].Get());

    Upload(yuy2, yuy2Pitch);
    UpdateConstants(&s, 0);
    ID3D11Buffer* cbs[] = {d.constants.Get()};
    ctx->CSSetConstantBuffers(0, 1, cbs);
    ID3D11SamplerState* samplers[] = {d.linearClamp.Get()};
    ctx->CSSetSamplers(0, 1, samplers);

    Bind(ctx, d.unpack.Get(), {d.packed.srv.Get()}, {d.mainYuv.uav.Get(), d.secondYuv.uav.Get()});
    ctx->Dispatch(DivUp(m_stereo.eyeWidth / 2, 16), DivUp(m_stereo.eyeHeight, 8), 1);
    d.slots[slot].brightness = s.autoBrightness;
    if (s.autoBrightness) {
        const UINT zeros[4] = {};
        ctx->ClearUnorderedAccessViewUint(d.lumaHistUav.Get(), zeros);
        Bind(ctx, d.lumastats.Get(), {d.mainYuv.srv.Get()}, {d.lumaHistUav.Get()});
        ctx->Dispatch(DivUp(m_stereo.eyeWidth / 8, 16), DivUp(m_stereo.eyeHeight / 8, 8), 1);
        ctx->CopyResource(d.lumaHistStaging[slot].Get(), d.lumaHist.Get());
    }

    const bool needDepth = s.mode == ViewMode::Bokeh || s.mode == ViewMode::Depth;
    d.slots[slot].depth = needDepth;
    if (needDepth) {
        if (m_frame != m_lastDepthFrame + 1) m_haveHistory = false;  // depth was off: history is stale
        m_lastDepthFrame = m_frame;
        RunDepth(s);
        UpdateConstants(&s, 0);
        const UINT zero[4] = {};
        ctx->ClearUnorderedAccessViewUint(d.histUav.Get(), zero);
        Bind(ctx, d.histogram.Get(), {d.mainYuv.srv.Get(), d.gfC.srv.Get()}, {d.histUav.Get()});
        ctx->Dispatch(DivUp(ow / 4, 16), DivUp(oh / 4, 8), 1);
        ctx->CopyResource(d.histStaging[slot].Get(), d.hist.Get());
    }
    if (s.mode == ViewMode::Bokeh) {
        Bind(ctx, d.bokeh.Get(), {d.mainYuv.srv.Get(), d.gfC.srv.Get()}, {d.bokehHalf.uav.Get()});
        ctx->Dispatch(DivUp(ow / 2, 16), DivUp(oh / 2, 8), 1);
    }
    const bool packedOut = m_output.format == PixelFormat::YUY2;
    Bind(ctx, d.composite.Get(), {d.mainYuv.srv.Get(), d.gfC.srv.Get(), d.bokehHalf.srv.Get(), d.secondYuv.srv.Get()},
        {d.outY.uav.Get(), d.outUV.uav.Get(), d.outYuy2.uav.Get()});
    ctx->Dispatch(DivUp(ow / 2, 16), DivUp(oh / 2, 8), 1);
    Bind(ctx, nullptr, {}, {});

    if (packedOut) {
        ctx->CopyResource(d.outYuy2Staging[slot].Get(), d.outYuy2.tex.Get());
    } else {
        ctx->CopyResource(d.outYStaging[slot].Get(), d.outY.tex.Get());
        ctx->CopyResource(d.outUVStaging[slot].Get(), d.outUV.tex.Get());
    }
    ctx->End(d.tsEnd[slot].Get());
    ctx->End(d.disjoint[slot].Get());
    ctx->Flush();  // start the GPU now; it runs while we read back the previous frame

    // Deliver the previous frame (normally finished long ago). The very first frame only primes
    // the ring, so the caller gets S_FALSE once.
    const int read = d.pending;
    d.pending = slot;
    ++m_frame;
    if (read < 0) return S_FALSE;

    auto readPlane = [&](ID3D11Texture2D* staging, uint8_t* out, uint32_t rows, uint32_t rowBytes) {
        D3D11_MAPPED_SUBRESOURCE m;
        HRESULT hr = ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m);
        if (FAILED(hr)) return hr;
        for (uint32_t y = 0; y < rows; ++y)
            memcpy(out + size_t(y) * dstPitch, static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch, rowBytes);
        ctx->Unmap(staging, 0);
        return S_OK;
    };
    HRESULT hr = packedOut ? readPlane(d.outYuy2Staging[read].Get(), dst, oh, ow * 2)
                           : readPlane(d.outYStaging[read].Get(), dst, oh, ow);
    if (SUCCEEDED(hr) && !packedOut) hr = readPlane(d.outUVStaging[read].Get(), dstUV, oh / 2, ow);
    if (FAILED(hr)) return hr;

    if (d.slots[read].brightness) {
        D3D11_MAPPED_SUBRESOURCE lm;
        if (SUCCEEDED(ctx->Map(d.lumaHistStaging[read].Get(), 0, D3D11_MAP_READ, 0, &lm))) {
            UpdateGain(static_cast<const uint32_t*>(lm.pData), s);
            ctx->Unmap(d.lumaHistStaging[read].Get(), 0);
        }
    }
    if (d.slots[read].depth) {
        D3D11_MAPPED_SUBRESOURCE hm;
        if (SUCCEEDED(ctx->Map(d.histStaging[read].Get(), 0, D3D11_MAP_READ, 0, &hm))) {
            UpdateFocus(static_cast<const uint32_t*>(hm.pData), s);
            ctx->Unmap(d.histStaging[read].Get(), 0);
        }
    }

    if (stats) {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj = {};
        UINT64 t0 = 0, t1 = 0;
        if (ctx->GetData(d.disjoint[read].Get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
            !dj.Disjoint && dj.Frequency &&
            ctx->GetData(d.tsBegin[read].Get(), &t0, sizeof(t0), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
            ctx->GetData(d.tsEnd[read].Get(), &t1, sizeof(t1), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK)
            stats->gpuMs = float(double(t1 - t0) * 1000.0 / dj.Frequency);
        stats->focusDisparity = m_focus;
        stats->gain = s.autoBrightness ? m_gain : 1.0f;
    }
    return S_OK;
}
float StereoPipeline::ScoreAlignment(float dy, float rotationDeg)
{
    auto* ctx = m_ctx.Get();
    auto& d = *m_impl;
    m_rect.dy = dy;
    m_rect.rotation = rotationDeg;
    UpdateConstants(nullptr, 0);
    Bind(ctx, d.downscale.Get(), {d.mainYuv.srv.Get(), d.secondYuv.srv.Get()}, {d.workMain.uav.Get(), d.workSecond.uav.Get()});
    ctx->Dispatch(DivUp(m_workW, 16), DivUp(m_workH, 8), 1);
    Bind(ctx, d.census.Get(), {d.workMain.srv.Get(), d.workSecond.srv.Get()},
        {d.censusMain.uav.Get(), d.censusSecond.uav.Get()});
    ctx->Dispatch(DivUp(m_workW, 16), DivUp(m_workH, 8), 1);
    const UINT zero[4] = {};
    ctx->ClearUnorderedAccessViewUint(d.scoreUav.Get(), zero);
    Bind(ctx, d.score.Get(), {d.censusMain.srv.Get(), d.censusSecond.srv.Get()}, {d.scoreUav.Get()});
    ctx->Dispatch(DivUp(m_workW, 16), DivUp(m_workH, 8), 1);
    Bind(ctx, nullptr, {}, {});
    ctx->CopyResource(d.scoreStaging.Get(), d.scoreBuf.Get());
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(d.scoreStaging.Get(), 0, D3D11_MAP_READ, 0, &m))) return 1e9f;
    const uint32_t* v = static_cast<const uint32_t*>(m.pData);
    float score = v[1] > 500 ? float(v[0]) / v[1] : 1e9f;
    ctx->Unmap(d.scoreStaging.Get(), 0);
    return score;
}

HRESULT StereoPipeline::Calibrate(const uint8_t* yuy2, uint32_t yuy2Pitch, Rectification* result)
{
    std::lock_guard lock(m_lock);
    if (!m_impl || m_stereo.mono) return E_NOT_VALID_STATE;
    auto* ctx = m_ctx.Get();
    auto& d = *m_impl;
    Rectification saved = m_rect;
    Upload(yuy2, yuy2Pitch);
    UpdateConstants(nullptr, 0);
    ID3D11Buffer* cbs[] = {d.constants.Get()};
    ctx->CSSetConstantBuffers(0, 1, cbs);
    ID3D11SamplerState* samplers[] = {d.linearClamp.Get()};
    ctx->CSSetSamplers(0, 1, samplers);
    Bind(ctx, d.unpack.Get(), {d.packed.srv.Get()}, {d.mainYuv.uav.Get(), d.secondYuv.uav.Get()});
    ctx->Dispatch(DivUp(m_stereo.eyeWidth / 2, 16), DivUp(m_stereo.eyeHeight, 8), 1);

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
