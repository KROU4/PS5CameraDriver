// Direct3D 11 backend of the GPU interface (gpu.h): one device on the default hardware adapter, its
// immediate context, the compute shaders compiled by fxc.
#include "gpu.h"

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <cstring>
#include <cwctype>
#include <string>
#include <vector>

#include "shaders/aggregate.h"
#include "shaders/bokeh.h"
#include "shaders/census.h"
#include "shaders/composite.h"
#include "shaders/denoise.h"
#include "shaders/depthout.h"
#include "shaders/downscale.h"
#include "shaders/guided_box.h"
#include "shaders/guided_coef.h"
#include "shaders/guided_prep.h"
#include "shaders/histogram.h"
#include "shaders/holefill.h"
#include "shaders/lrfill.h"
#include "shaders/lumastats.h"
#include "shaders/meter.h"
#include "shaders/motion_down.h"
#include "shaders/motion_refine.h"
#include "shaders/motion_search.h"
#include "shaders/score.h"
#include "shaders/subject_share.h"
#include "shaders/subject_seed.h"
#include "shaders/subject_sweep.h"
#include "shaders/unpack.h"
#include "shaders/wta.h"

using Microsoft::WRL::ComPtr;

namespace ps5cam {

struct GpuImage {
    uint32_t width = 0, height = 0;
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    ComPtr<ID3D11Texture2D> staging[Gpu::kSlots];
};

struct GpuBuffer {
    uint32_t bytes = 0;
    ComPtr<ID3D11Buffer> buf;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
    ComPtr<ID3D11Buffer> staging[Gpu::kSlots];
};

namespace {

struct Blob {
    const BYTE* data;
    size_t size;
};

// In the order of Kernel.
const Blob kKernels[] = {
    {g_unpack, sizeof(g_unpack)}, {g_downscale, sizeof(g_downscale)}, {g_census, sizeof(g_census)},
    {g_aggregate, sizeof(g_aggregate)}, {g_wta, sizeof(g_wta)}, {g_lrfill, sizeof(g_lrfill)},
    {g_holefill, sizeof(g_holefill)}, {g_guided_prep, sizeof(g_guided_prep)}, {g_guided_box, sizeof(g_guided_box)},
    {g_guided_coef, sizeof(g_guided_coef)}, {g_histogram, sizeof(g_histogram)}, {g_bokeh, sizeof(g_bokeh)},
    {g_composite, sizeof(g_composite)}, {g_score, sizeof(g_score)}, {g_lumastats, sizeof(g_lumastats)},
    {g_denoise, sizeof(g_denoise)}, {g_depthout, sizeof(g_depthout)}, {g_subject_seed, sizeof(g_subject_seed)},
    {g_subject_sweep, sizeof(g_subject_sweep)}, {g_subject_share, sizeof(g_subject_share)}, {g_meter, sizeof(g_meter)},
    {g_motion_down, sizeof(g_motion_down)}, {g_motion_search, sizeof(g_motion_search)},
    {g_motion_refine, sizeof(g_motion_refine)},
};
static_assert(sizeof(kKernels) / sizeof(kKernels[0]) == size_t(Kernel::Count), "one blob per kernel");

DXGI_FORMAT ToDxgi(GpuFormat f)
{
    switch (f) {
    case GpuFormat::RGBA8_UINT: return DXGI_FORMAT_R8G8B8A8_UINT;
    case GpuFormat::RGBA8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case GpuFormat::R32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
    case GpuFormat::RG32_UINT: return DXGI_FORMAT_R32G32_UINT;
    case GpuFormat::RGBA32_FLOAT: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case GpuFormat::RGBA16_FLOAT: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case GpuFormat::R8_UNORM: return DXGI_FORMAT_R8_UNORM;
    case GpuFormat::RG8_UNORM: return DXGI_FORMAT_R8G8_UNORM;
    }
    return DXGI_FORMAT_UNKNOWN;
}

}  // namespace

struct Gpu::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11ComputeShader> kernels[size_t(Kernel::Count)];
    ComPtr<ID3D11Buffer> constants;
    uint32_t constantBytes = 0;
    ComPtr<ID3D11SamplerState> linearClamp;
    ComPtr<ID3D11Query> disjoint[kSlots], tsBegin[kSlots], tsEnd[kSlots];
    std::vector<std::unique_ptr<GpuImage>> images;
    std::vector<std::unique_ptr<GpuBuffer>> buffers;
    HRESULT error = S_OK;  // sticky: a recording mistake makes every later Map fail, as on Vulkan

    void Bind(ID3D11ComputeShader* cs, std::initializer_list<GpuRef> reads, std::initializer_list<GpuRef> writes)
    {
        ID3D11ShaderResourceView* nullSrv[Gpu::kMaxReads] = {};
        ID3D11UnorderedAccessView* nullUav[Gpu::kMaxWrites] = {};
        ctx->CSSetShaderResources(0, UINT(Gpu::kMaxReads), nullSrv);
        ctx->CSSetUnorderedAccessViews(0, UINT(Gpu::kMaxWrites), nullUav, nullptr);
        ctx->CSSetShader(cs, nullptr, 0);
        std::vector<ID3D11ShaderResourceView*> s;
        for (const GpuRef& r : reads) s.push_back(r.image ? r.image->srv.Get() : r.buffer ? r.buffer->srv.Get() : nullptr);
        std::vector<ID3D11UnorderedAccessView*> u;
        for (const GpuRef& r : writes) u.push_back(r.image ? r.image->uav.Get() : r.buffer ? r.buffer->uav.Get() : nullptr);
        if (!s.empty()) ctx->CSSetShaderResources(0, static_cast<UINT>(s.size()), s.data());
        if (!u.empty()) ctx->CSSetUnorderedAccessViews(0, static_cast<UINT>(u.size()), u.data(), nullptr);
    }
};

Gpu::Gpu() : m(std::make_unique<Impl>()) {}
Gpu::~Gpu() = default;

HRESULT Gpu::Initialize()
{
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    // PS5CAM_GPU picks an adapter by a part of its name, any case (e.g. "basic render" for the
    // software rasteriser), as on Vulkan; without a match, or by default, the default hardware
    // adapter (a variable left over from a test must not cost the camera its picture).
    ComPtr<IDXGIAdapter1> chosen;
    wchar_t wanted[128];
    const DWORD length = GetEnvironmentVariableW(L"PS5CAM_GPU", wanted, 128);
    if (length > 0 && length < 128) {
        auto lower = [](std::wstring s) {
            for (wchar_t& c : s) c = towlower(c);
            return s;
        };
        const std::wstring needle = lower(wanted);
        ComPtr<IDXGIFactory1> factory;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
            ComPtr<IDXGIAdapter1> adapter;
            for (UINT i = 0; !chosen && factory->EnumAdapters1(i, &adapter) == S_OK; ++i) {
                DXGI_ADAPTER_DESC1 desc;
                if (SUCCEEDED(adapter->GetDesc1(&desc)) && lower(desc.Description).find(needle) != std::wstring::npos)
                    chosen = adapter;
            }
        }
    }
    HRESULT hr = D3D11CreateDevice(chosen.Get(), chosen ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &m->device, nullptr, &m->ctx);
    if (FAILED(hr)) return hr;
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(m->ctx.As(&mt))) mt->SetMultithreadProtected(TRUE);
    for (size_t i = 0; i < size_t(Kernel::Count); ++i) {
        hr = m->device->CreateComputeShader(kKernels[i].data, kKernels[i].size, nullptr, &m->kernels[i]);
        if (FAILED(hr)) return hr;
    }
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    hr = m->device->CreateSamplerState(&sd, &m->linearClamp);
    if (FAILED(hr)) return hr;
    ID3D11SamplerState* samplers[] = {m->linearClamp.Get()};
    m->ctx->CSSetSamplers(0, 1, samplers);
    for (int i = 0; i < kSlots; ++i) {
        D3D11_QUERY_DESC q = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        hr = m->device->CreateQuery(&q, &m->disjoint[i]);
        if (FAILED(hr)) return hr;
        q.Query = D3D11_QUERY_TIMESTAMP;
        hr = m->device->CreateQuery(&q, &m->tsBegin[i]);
        if (FAILED(hr)) return hr;
        hr = m->device->CreateQuery(&q, &m->tsEnd[i]);
        if (FAILED(hr)) return hr;
    }
    return S_OK;
}

std::string Gpu::DeviceName() const
{
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc = {};
    if (!m->device || FAILED(m->device.As(&dxgi)) || FAILED(dxgi->GetAdapter(&adapter)) || FAILED(adapter->GetDesc(&desc)))
        return {};
    char name[128] = {};
    WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
    return name;
}

HRESULT Gpu::CreateImage(uint32_t width, uint32_t height, GpuFormat format, unsigned flags, GpuImage** out)
{
    auto img = std::make_unique<GpuImage>();
    img->width = width;
    img->height = height;
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = width;
    d.Height = height;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = ToDxgi(format);
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | ((flags & kStorage) ? D3D11_BIND_UNORDERED_ACCESS : 0);
    HRESULT hr = m->device->CreateTexture2D(&d, nullptr, &img->tex);
    if (FAILED(hr)) return hr;
    hr = m->device->CreateShaderResourceView(img->tex.Get(), nullptr, &img->srv);
    if (FAILED(hr)) return hr;
    if (flags & kStorage) {
        hr = m->device->CreateUnorderedAccessView(img->tex.Get(), nullptr, &img->uav);
        if (FAILED(hr)) return hr;
    }
    if (flags & kReadback) {
        D3D11_TEXTURE2D_DESC s = d;
        s.Usage = D3D11_USAGE_STAGING;
        s.BindFlags = 0;
        s.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        for (auto& staging : img->staging) {
            hr = m->device->CreateTexture2D(&s, nullptr, &staging);
            if (FAILED(hr)) return hr;
        }
    }
    *out = img.get();
    m->images.push_back(std::move(img));
    return S_OK;
}

HRESULT Gpu::CreateBuffer(uint32_t bytes, bool readback, GpuBuffer** out)
{
    auto b = std::make_unique<GpuBuffer>();
    b->bytes = bytes;
    D3D11_BUFFER_DESC d = {};
    d.ByteWidth = bytes;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    HRESULT hr = m->device->CreateBuffer(&d, nullptr, &b->buf);
    if (FAILED(hr)) return hr;
    D3D11_UNORDERED_ACCESS_VIEW_DESC u = {};
    u.Format = DXGI_FORMAT_R32_TYPELESS;
    u.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    u.Buffer.NumElements = bytes / 4;
    u.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    hr = m->device->CreateUnorderedAccessView(b->buf.Get(), &u, &b->uav);
    if (FAILED(hr)) return hr;
    D3D11_SHADER_RESOURCE_VIEW_DESC s = {};
    s.Format = DXGI_FORMAT_R32_TYPELESS;
    s.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    s.BufferEx.NumElements = bytes / 4;
    s.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    hr = m->device->CreateShaderResourceView(b->buf.Get(), &s, &b->srv);
    if (FAILED(hr)) return hr;
    if (readback) {
        D3D11_BUFFER_DESC sd = {};
        sd.ByteWidth = bytes;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        for (auto& staging : b->staging) {
            hr = m->device->CreateBuffer(&sd, nullptr, &staging);
            if (FAILED(hr)) return hr;
        }
    }
    *out = b.get();
    m->buffers.push_back(std::move(b));
    return S_OK;
}

void Gpu::Upload(GpuImage* image, const uint8_t* data, uint32_t pitch)
{
    m->ctx->UpdateSubresource(image->tex.Get(), 0, nullptr, data, pitch, pitch * image->height);
}

void Gpu::SetConstants(const void* data, uint32_t bytes)
{
    if (!m->constants || bytes > m->constantBytes) {
        D3D11_BUFFER_DESC cb = {};
        cb.ByteWidth = (bytes + 15) & ~15u;
        cb.Usage = D3D11_USAGE_DYNAMIC;
        cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        m->constants.Reset();
        m->constantBytes = 0;
        if (FAILED(m->device->CreateBuffer(&cb, nullptr, &m->constants)) || !m->constants) return;
        m->constantBytes = cb.ByteWidth;
        ID3D11Buffer* cbs[] = {m->constants.Get()};
        m->ctx->CSSetConstantBuffers(0, 1, cbs);
    }
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(m->ctx->Map(m->constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, data, bytes);
        m->ctx->Unmap(m->constants.Get(), 0);
    }
}

void Gpu::ClearUint(GpuBuffer* buffer)
{
    const UINT zeros[4] = {};
    m->ctx->ClearUnorderedAccessViewUint(buffer->uav.Get(), zeros);
}

void Gpu::ClearFloat(GpuImage* image, float value)
{
    const float v[4] = {value, value, value, value};
    m->ctx->ClearUnorderedAccessViewFloat(image->uav.Get(), v);
}

void Gpu::Dispatch(Kernel kernel, std::initializer_list<GpuRef> reads, std::initializer_list<GpuRef> writes, uint32_t x,
    uint32_t y, uint32_t z)
{
    if (reads.size() > kMaxReads || writes.size() > kMaxWrites) {  // would stay bound to later kernels
        m->error = E_INVALIDARG;
        return;
    }
    m->Bind(m->kernels[size_t(kernel)].Get(), reads, writes);
    m->ctx->Dispatch(x, y, z);
}

void Gpu::Unbind()
{
    m->Bind(nullptr, {}, {});
}

void Gpu::CopyToReadback(GpuRef r, int slot)
{
    if (r.image) m->ctx->CopyResource(r.image->staging[slot].Get(), r.image->tex.Get());
    else if (r.buffer) m->ctx->CopyResource(r.buffer->staging[slot].Get(), r.buffer->buf.Get());
}

void Gpu::BeginTiming(int slot)
{
    m->ctx->Begin(m->disjoint[slot].Get());
    m->ctx->End(m->tsBegin[slot].Get());
}

void Gpu::EndTiming(int slot)
{
    m->ctx->End(m->tsEnd[slot].Get());
    m->ctx->End(m->disjoint[slot].Get());
}

void Gpu::Flush()
{
    m->ctx->Flush();
}

HRESULT Gpu::Map(GpuRef r, int slot, GpuMapped* out)
{
    if (FAILED(m->error)) return m->error;
    ID3D11Resource* staging = r.image ? static_cast<ID3D11Resource*>(r.image->staging[slot].Get())
                                      : static_cast<ID3D11Resource*>(r.buffer->staging[slot].Get());
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = m->ctx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) return hr;
    out->data = static_cast<const uint8_t*>(mapped.pData);
    out->rowPitch = r.image ? mapped.RowPitch : r.buffer->bytes;
    return S_OK;
}

void Gpu::Unmap(GpuRef r, int slot)
{
    if (r.image) m->ctx->Unmap(r.image->staging[slot].Get(), 0);
    else m->ctx->Unmap(r.buffer->staging[slot].Get(), 0);
}

bool Gpu::TimingMs(int slot, float* ms)
{
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj = {};
    UINT64 t0 = 0, t1 = 0;
    if (m->ctx->GetData(m->disjoint[slot].Get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
        !dj.Disjoint && dj.Frequency &&
        m->ctx->GetData(m->tsBegin[slot].Get(), &t0, sizeof(t0), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
        m->ctx->GetData(m->tsEnd[slot].Get(), &t1, sizeof(t1), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK) {
        *ms = float(double(t1 - t0) * 1000.0 / dj.Frequency);
        return true;
    }
    return false;
}

}  // namespace ps5cam
