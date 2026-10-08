#pragma once
// The GPU under the stereo pipeline: compute kernels over images and buffers, uploads and readback,
// on Direct3D 11 (Windows, gpu_d3d11.cpp) or Vulkan (Linux, gpu_vulkan.cpp). Recording behaves like
// a D3D11 immediate context: commands run in the order given and each sees what the earlier ones
// wrote; Flush starts the GPU without waiting; Map of a readback copy waits for that copy.
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>

#include "platform.h"

namespace ps5cam {

// One per compute shader in shaders/ (guided.hlsl has three entry points).
enum class Kernel : uint32_t {
    Unpack, Downscale, Census, Aggregate, Wta, LrFill, HoleFill, GuidedPrep, GuidedBox, GuidedCoef, Histogram, Bokeh,
    Composite, Score, LumaStats, Denoise, DepthOut, Count
};

enum class GpuFormat : uint32_t { RGBA8_UINT, RGBA8_UNORM, R32_FLOAT, RG32_UINT, RGBA32_FLOAT, RGBA16_FLOAT, R8_UNORM, RG8_UNORM };

struct GpuImage;   // owned by the Gpu that made it
struct GpuBuffer;  // raw 32-bit words (ByteAddressBuffer in the shaders)

// A resource bound to a kernel: an image, a buffer, or none. Kernels see the reads in the order
// given as t0, t1, ... and the writes as u0, u1, ... ; the constants are b0, the sampler s0.
struct GpuRef {
    GpuRef(std::nullptr_t = nullptr) {}
    GpuRef(GpuImage* i) : image(i) {}
    GpuRef(GpuBuffer* b) : buffer(b) {}
    GpuImage* image = nullptr;
    GpuBuffer* buffer = nullptr;
};

struct GpuMapped {
    const uint8_t* data = nullptr;
    uint32_t rowPitch = 0;  // images: bytes between rows; buffers: the size
};

// The HRESULT of a lost device on every backend (DXGI_ERROR_DEVICE_REMOVED on Windows).
constexpr HRESULT kGpuDeviceLost = static_cast<HRESULT>(0x887A0005u);

class Gpu {
public:
    static constexpr int kSlots = 2;  // readback copies per resource: the GPU fills one, the CPU reads the other

    // Image flags.
    static constexpr unsigned kStorage = 1;   // kernels may write it
    static constexpr unsigned kReadback = 2;  // CopyToReadback / Map

    Gpu();
    ~Gpu();
    Gpu(const Gpu&) = delete;
    Gpu& operator=(const Gpu&) = delete;

    // The default hardware adapter (Vulkan: the first discrete GPU, else integrated, else any; the
    // PS5CAM_GPU environment variable picks one by a part of its name), the kernels and the sampler.
    HRESULT Initialize();
    std::string DeviceName() const;

    HRESULT CreateImage(uint32_t width, uint32_t height, GpuFormat format, unsigned flags, GpuImage** out);
    HRESULT CreateBuffer(uint32_t bytes, bool readback, GpuBuffer** out);

    // Recording.
    void Upload(GpuImage* image, const uint8_t* data, uint32_t pitch);
    void SetConstants(const void* data, uint32_t bytes);  // b0 of the following dispatches
    void ClearUint(GpuBuffer* buffer);                    // to zero
    void ClearFloat(GpuImage* image, float value);
    void Dispatch(Kernel kernel, std::initializer_list<GpuRef> reads, std::initializer_list<GpuRef> writes, uint32_t x,
        uint32_t y, uint32_t z = 1);
    void Unbind();  // D3D11: release the views bound by the last dispatch
    void CopyToReadback(GpuRef resource, int slot);
    void BeginTiming(int slot);
    void EndTiming(int slot);
    void Flush();

    // Waits for the slot's latest CopyToReadback. Unmap before the next copy into that slot.
    HRESULT Map(GpuRef resource, int slot, GpuMapped* out);
    void Unmap(GpuRef resource, int slot);
    // GPU time between BeginTiming and EndTiming of the slot, if already known (does not wait).
    bool TimingMs(int slot, float* ms);

    struct Impl;

private:
    std::unique_ptr<Impl> m;
};

}  // namespace ps5cam
