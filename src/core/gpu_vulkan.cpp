// Vulkan backend of the GPU interface (gpu.h): one compute queue and the HLSL kernels compiled to
// SPIR-V by dxc (src/core/CMakeLists.txt maps their registers to bindings of set 0).
//
// Commands are recorded into batches. Flush submits the batch being recorded; so does a Map that
// needs its results, or a batch that runs out of room (descriptors, constants, upload space), so
// the split is invisible to the caller. Every command is followed by a full memory barrier, which
// gives the in-order behaviour of a D3D11 immediate context; images live in the GENERAL layout.
#include "gpu.h"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
#include "shaders/score.h"
#include "shaders/unpack.h"
#include "shaders/wta.h"

namespace ps5cam {

namespace {

// Bindings of the registers (dxc -fvk-*-shift in CMakeLists.txt).
constexpr uint32_t kBindConstants = 0;  // b0
constexpr uint32_t kBindRead = 10;      // t0, t1, ...
constexpr uint32_t kBindSampler = 30;   // s0
constexpr uint32_t kBindWrite = 40;     // u0, u1, ...

constexpr uint32_t kBatches = 3;
constexpr uint32_t kSetsPerBatch = 256;
constexpr uint32_t kConstantSlots = 256;
constexpr uint32_t kMaxConstantBytes = 256;
// A batch taking longer is a hung GPU (a frame takes ~10 ms on a GPU, ~1 s on lavapipe).
constexpr uint64_t kWaitNs = 20ull * 1000 * 1000 * 1000;

struct Blob {
    const void* data;
    size_t size;
};

// In the order of Kernel.
const Blob kKernels[] = {
    {g_unpack, sizeof(g_unpack)}, {g_downscale, sizeof(g_downscale)}, {g_census, sizeof(g_census)},
    {g_aggregate, sizeof(g_aggregate)}, {g_wta, sizeof(g_wta)}, {g_lrfill, sizeof(g_lrfill)},
    {g_holefill, sizeof(g_holefill)}, {g_guided_prep, sizeof(g_guided_prep)}, {g_guided_box, sizeof(g_guided_box)},
    {g_guided_coef, sizeof(g_guided_coef)}, {g_histogram, sizeof(g_histogram)}, {g_bokeh, sizeof(g_bokeh)},
    {g_composite, sizeof(g_composite)}, {g_score, sizeof(g_score)}, {g_lumastats, sizeof(g_lumastats)},
    {g_denoise, sizeof(g_denoise)}, {g_depthout, sizeof(g_depthout)},
};
static_assert(sizeof(kKernels) / sizeof(kKernels[0]) == size_t(Kernel::Count), "one blob per kernel");

struct FormatInfo {
    VkFormat format;
    uint32_t bytes;  // per texel
};

FormatInfo Info(GpuFormat f)
{
    switch (f) {
    case GpuFormat::RGBA8_UINT: return {VK_FORMAT_R8G8B8A8_UINT, 4};
    case GpuFormat::RGBA8_UNORM: return {VK_FORMAT_R8G8B8A8_UNORM, 4};
    case GpuFormat::R32_FLOAT: return {VK_FORMAT_R32_SFLOAT, 4};
    case GpuFormat::RG32_UINT: return {VK_FORMAT_R32G32_UINT, 8};
    case GpuFormat::RGBA32_FLOAT: return {VK_FORMAT_R32G32B32A32_SFLOAT, 16};
    case GpuFormat::RGBA16_FLOAT: return {VK_FORMAT_R16G16B16A16_SFLOAT, 8};
    case GpuFormat::R8_UNORM: return {VK_FORMAT_R8_UNORM, 1};
    case GpuFormat::RG8_UNORM: return {VK_FORMAT_R8G8_UNORM, 2};
    }
    return {VK_FORMAT_UNDEFINED, 0};
}

HRESULT ToHresult(VkResult r)
{
    switch (r) {
    case VK_SUCCESS: return S_OK;
    case VK_ERROR_DEVICE_LOST: return kGpuDeviceLost;
    case VK_ERROR_OUT_OF_HOST_MEMORY:
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return E_OUTOFMEMORY;
    case VK_ERROR_FORMAT_NOT_SUPPORTED:
    case VK_ERROR_FEATURE_NOT_PRESENT:
    case VK_ERROR_INCOMPATIBLE_DRIVER: return E_NOTIMPL;
    default: return E_FAIL;
    }
}

// Host-visible memory: readback copies, upload staging, the constants ring.
struct HostBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint8_t* ptr = nullptr;
    VkDeviceSize size = 0;
    bool coherent = true;
};

}  // namespace

struct GpuImage {
    uint32_t width = 0, height = 0, texelBytes = 0;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    HostBuffer readback[Gpu::kSlots];
    uint64_t readbackBatch[Gpu::kSlots] = {};  // the batch that holds the slot's latest copy
};

struct GpuBuffer {
    uint32_t bytes = 0;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    HostBuffer readback[Gpu::kSlots];
    uint64_t readbackBatch[Gpu::kSlots] = {};
};

struct Gpu::Impl {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props = {};
    VkPhysicalDeviceMemoryProperties memProps = {};
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    uint32_t timestampBits = 0;
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkQueryPool queries = VK_NULL_HANDLE;
    bool timingWritten[kSlots] = {};
    VkResult lost = VK_SUCCESS;  // sticky: a lost device stays lost

    // A kernel's pipeline, made at its first dispatch from the kinds of resources given there.
    struct KernelState {
        VkShaderModule module = VK_NULL_HANDLE;
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        std::vector<char> reads, writes;  // 'i' image, 'b' buffer
        bool mismatchLogged = false;
    } kernels[size_t(Kernel::Count)];

    enum class BatchState { Idle, Recording, Submitted };
    struct Batch {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        HostBuffer upload;
        VkDeviceSize uploadUsed = 0;
        HostBuffer constants;  // kConstantSlots slots of constantStride bytes
        uint32_t constantSlot = 0;
        uint32_t sets = 0;
        uint64_t serial = 0;
        BatchState state = BatchState::Idle;
    } batches[kBatches];
    uint32_t current = 0;
    uint64_t nextSerial = 1;
    uint32_t constantStride = kMaxConstantBytes;
    bool constantsUsed = false;  // a dispatch refers to the current constants slot
    uint8_t lastConstants[kMaxConstantBytes] = {};
    uint32_t lastConstantBytes = 0;

    GpuImage* dummy = nullptr;  // what a kernel reads in place of a read given as none
    std::vector<std::unique_ptr<GpuImage>> images;
    std::vector<std::unique_ptr<GpuBuffer>> buffers;

    ~Impl();

    uint32_t FindMemory(uint32_t typeBits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags fallback) const
    {
        for (VkMemoryPropertyFlags flags : {want, fallback})
            for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
                if ((typeBits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & flags) == flags) return i;
        return UINT32_MAX;
    }

    VkResult AllocateFor(VkMemoryRequirements req, VkMemoryPropertyFlags want, VkMemoryPropertyFlags fallback,
        VkDeviceMemory* memory, VkMemoryPropertyFlags* got = nullptr)
    {
        uint32_t type = FindMemory(req.memoryTypeBits, want, fallback);
        if (type == UINT32_MAX) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        if (got) *got = memProps.memoryTypes[type].propertyFlags;
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type;
        return vkAllocateMemory(device, &ai, nullptr, memory);
    }

    VkResult MakeHostBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool forReading, HostBuffer& out)
    {
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = size;
        bi.usage = usage;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkResult r = vkCreateBuffer(device, &bi, nullptr, &out.buffer);
        if (r != VK_SUCCESS) return r;
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device, out.buffer, &req);
        // The CPU reads readback copies, which is slow from uncached memory.
        const VkMemoryPropertyFlags visible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        const VkMemoryPropertyFlags want = forReading ? visible | VK_MEMORY_PROPERTY_HOST_CACHED_BIT
                                                      : visible | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        VkMemoryPropertyFlags got = 0;
        r = AllocateFor(req, want, visible, &out.memory, &got);
        if (r != VK_SUCCESS) return r;
        r = vkBindBufferMemory(device, out.buffer, out.memory, 0);
        if (r != VK_SUCCESS) return r;
        void* p = nullptr;
        r = vkMapMemory(device, out.memory, 0, VK_WHOLE_SIZE, 0, &p);
        if (r != VK_SUCCESS) return r;
        out.ptr = static_cast<uint8_t*>(p);
        out.size = size;
        out.coherent = (got & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
        return VK_SUCCESS;
    }

    void FreeHostBuffer(HostBuffer& b)
    {
        if (b.memory) vkFreeMemory(device, b.memory, nullptr);  // unmaps too
        if (b.buffer) vkDestroyBuffer(device, b.buffer, nullptr);
        b = HostBuffer();
    }

    static void Barrier(VkCommandBuffer cmd)
    {
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0,
            nullptr, 0, nullptr);
    }

    // Copies for the CPU become visible to it once the batch's fence has signalled.
    static void HostBarrier(VkCommandBuffer cmd)
    {
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0,
            nullptr);
    }

    bool Wait(Batch& b)
    {
        if (b.state != BatchState::Submitted) return lost == VK_SUCCESS;
        VkResult r = vkWaitForFences(device, 1, &b.fence, VK_TRUE, kWaitNs);
        if (r != VK_SUCCESS) {
            fprintf(stderr, "ps5cam gpu: waiting for the GPU failed (%d)\n", int(r));
            lost = VK_ERROR_DEVICE_LOST;
            return false;
        }
        b.state = BatchState::Idle;
        return lost == VK_SUCCESS;
    }

    // The batch being recorded, started if need be.
    Batch& Rec()
    {
        Batch& b = batches[current];
        if (b.state == BatchState::Recording) return b;
        Wait(b);
        vkResetCommandBuffer(b.cmd, 0);
        vkResetDescriptorPool(device, b.pool, 0);
        b.uploadUsed = 0;
        b.constantSlot = 0;
        b.sets = 0;
        b.serial = nextSerial++;
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(b.cmd, &bi);
        b.state = BatchState::Recording;
        // Dispatches without a SetConstants in this batch see the constants set last.
        memcpy(b.constants.ptr, lastConstants, sizeof(lastConstants));
        constantsUsed = false;
        return b;
    }

    void Submit()
    {
        Batch& b = batches[current];
        if (b.state != BatchState::Recording) return;
        VkResult r = vkEndCommandBuffer(b.cmd);
        if (r == VK_SUCCESS) r = vkResetFences(device, 1, &b.fence);
        if (r == VK_SUCCESS) {
            VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
            si.commandBufferCount = 1;
            si.pCommandBuffers = &b.cmd;
            r = vkQueueSubmit(queue, 1, &si, b.fence);
        }
        if (r != VK_SUCCESS) {
            fprintf(stderr, "ps5cam gpu: submit failed (%d)\n", int(r));
            lost = VK_ERROR_DEVICE_LOST;
            b.state = BatchState::Idle;
        } else {
            b.state = BatchState::Submitted;
        }
        current = (current + 1) % kBatches;
    }

    // Waits until the batch with this serial has run (submitting it first if it is still recording).
    bool WaitSerial(uint64_t serial)
    {
        for (Batch& b : batches) {
            if (b.serial != serial) continue;
            if (b.state == BatchState::Recording) Submit();
            return Wait(b);
        }
        return lost == VK_SUCCESS;  // long recycled, hence done
    }

    void ToGeneral(VkImage image)
    {
        Batch& b = Rec();
        VkImageMemoryBarrier ib = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        ib.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ib.image = image;
        ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(b.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
            0, nullptr, 1, &ib);
    }

    KernelState* Prepare(Kernel kernel, std::initializer_list<GpuRef> reads, std::initializer_list<GpuRef> writes);
    VkResult CreateBatches();
    VkResult PickDevice();
};

Gpu::Impl::~Impl()
{
    if (!device) {
        if (instance) vkDestroyInstance(instance, nullptr);
        return;
    }
    vkDeviceWaitIdle(device);
    for (auto& img : images) {
        if (img->view) vkDestroyImageView(device, img->view, nullptr);
        if (img->image) vkDestroyImage(device, img->image, nullptr);
        if (img->memory) vkFreeMemory(device, img->memory, nullptr);
        for (auto& rb : img->readback) FreeHostBuffer(rb);
    }
    for (auto& buf : buffers) {
        if (buf->buffer) vkDestroyBuffer(device, buf->buffer, nullptr);
        if (buf->memory) vkFreeMemory(device, buf->memory, nullptr);
        for (auto& rb : buf->readback) FreeHostBuffer(rb);
    }
    for (auto& k : kernels) {
        if (k.pipeline) vkDestroyPipeline(device, k.pipeline, nullptr);
        if (k.layout) vkDestroyPipelineLayout(device, k.layout, nullptr);
        if (k.setLayout) vkDestroyDescriptorSetLayout(device, k.setLayout, nullptr);
        if (k.module) vkDestroyShaderModule(device, k.module, nullptr);
    }
    for (auto& b : batches) {
        FreeHostBuffer(b.upload);
        FreeHostBuffer(b.constants);
        if (b.pool) vkDestroyDescriptorPool(device, b.pool, nullptr);
        if (b.fence) vkDestroyFence(device, b.fence, nullptr);
    }
    if (commandPool) vkDestroyCommandPool(device, commandPool, nullptr);
    if (queries) vkDestroyQueryPool(device, queries, nullptr);
    if (sampler) vkDestroySampler(device, sampler, nullptr);
    vkDestroyDevice(device, nullptr);
    if (instance) vkDestroyInstance(instance, nullptr);
}

VkResult Gpu::Impl::PickDevice()
{
    uint32_t count = 0;
    VkResult r = vkEnumeratePhysicalDevices(instance, &count, nullptr);
    if (r != VK_SUCCESS) return r;
    std::vector<VkPhysicalDevice> devices(count);
    r = vkEnumeratePhysicalDevices(instance, &count, devices.data());
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) return r;

    std::string wanted;
    if (const char* env = std::getenv("PS5CAM_GPU")) wanted = env;
    auto lower = [](std::string s) {
        for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    int bestScore = -1;
    for (VkPhysicalDevice pd : devices) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd, &p);
        if (p.apiVersion < VK_API_VERSION_1_1) continue;
        uint32_t families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &families, nullptr);
        std::vector<VkQueueFamilyProperties> qf(families);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &families, qf.data());
        // A queue family that does compute; the timestamps matter only for the statistics.
        int family = -1;
        for (uint32_t i = 0; i < families; ++i)
            if ((qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && (family < 0 || qf[i].timestampValidBits)) family = int(i);
        if (family < 0) continue;
        // The kernels filter these formats linearly and write the others as storage images.
        bool formats = true;
        for (VkFormat f : {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT}) {
            VkFormatProperties fp;
            vkGetPhysicalDeviceFormatProperties(pd, f, &fp);
            formats &= (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
        }
        for (VkFormat f : {VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_UINT}) {
            VkFormatProperties fp;
            vkGetPhysicalDeviceFormatProperties(pd, f, &fp);
            formats &= (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
        }
        VkPhysicalDeviceFeatures supported;
        vkGetPhysicalDeviceFeatures(pd, &supported);
        if (!formats || !supported.shaderStorageImageExtendedFormats) {
            fprintf(stderr, "ps5cam gpu: %s lacks an image format the pipeline needs\n", p.deviceName);
            continue;
        }
        int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? 4
                    : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 3
                    : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU    ? 2
                    : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU            ? 1
                                                                             : 0;
        if (!wanted.empty()) score = lower(p.deviceName).find(lower(wanted)) != std::string::npos ? 10 : -1;
        if (score > bestScore) {
            bestScore = score;
            physical = pd;
            props = p;
            queueFamily = uint32_t(family);
            timestampBits = qf[family].timestampValidBits;
        }
    }
    return physical ? VK_SUCCESS : VK_ERROR_INCOMPATIBLE_DRIVER;
}

VkResult Gpu::Impl::CreateBatches()
{
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = commandPool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    const VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, kSetsPerBatch},
        {VK_DESCRIPTOR_TYPE_SAMPLER, kSetsPerBatch},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kSetsPerBatch * 8},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kSetsPerBatch * 4},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kSetsPerBatch * 4},
    };
    for (Batch& b : batches) {
        VkResult r = vkAllocateCommandBuffers(device, &ai, &b.cmd);
        if (r != VK_SUCCESS) return r;
        VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        r = vkCreateFence(device, &fi, nullptr, &b.fence);
        if (r != VK_SUCCESS) return r;
        VkDescriptorPoolCreateInfo pi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets = kSetsPerBatch;
        pi.poolSizeCount = uint32_t(sizeof(sizes) / sizeof(sizes[0]));
        pi.pPoolSizes = sizes;
        r = vkCreateDescriptorPool(device, &pi, nullptr, &b.pool);
        if (r != VK_SUCCESS) return r;
        r = MakeHostBuffer(VkDeviceSize(constantStride) * kConstantSlots, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, false,
            b.constants);
        if (r != VK_SUCCESS) return r;
    }
    return VK_SUCCESS;
}

Gpu::Gpu() : m(std::make_unique<Impl>()) {}
Gpu::~Gpu() = default;

HRESULT Gpu::Initialize()
{
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "ps5cam";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    // PS5CAM_VK_VALIDATION=1: the Khronos validation layer, when installed (development).
    const char* validation = "VK_LAYER_KHRONOS_validation";
    const char* env = std::getenv("PS5CAM_VK_VALIDATION");
    if (env && env[0] == '1') {
        uint32_t n = 0;
        vkEnumerateInstanceLayerProperties(&n, nullptr);
        std::vector<VkLayerProperties> layers(n);
        vkEnumerateInstanceLayerProperties(&n, layers.data());
        for (const auto& l : layers)
            if (strcmp(l.layerName, validation) == 0) {
                ici.enabledLayerCount = 1;
                ici.ppEnabledLayerNames = &validation;
            }
    }
    VkResult r = vkCreateInstance(&ici, nullptr, &m->instance);
    if (r != VK_SUCCESS) return ToHresult(r);
    r = m->PickDevice();
    if (r != VK_SUCCESS) return ToHresult(r);
    vkGetPhysicalDeviceMemoryProperties(m->physical, &m->memProps);

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = m->queueFamily;
    qi.queueCount = 1;
    qi.pQueuePriorities = &priority;
    // Images written as r8, rg8 and rg32ui (composite, depthout, census) need the extended formats.
    VkPhysicalDeviceFeatures features = {};
    features.shaderStorageImageExtendedFormats = VK_TRUE;
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qi;
    dci.pEnabledFeatures = &features;
    r = vkCreateDevice(m->physical, &dci, nullptr, &m->device);
    if (r != VK_SUCCESS) return ToHresult(r);
    vkGetDeviceQueue(m->device, m->queueFamily, 0, &m->queue);

    const uint32_t align = uint32_t(std::max<VkDeviceSize>(m->props.limits.minUniformBufferOffsetAlignment, 16));
    m->constantStride = (kMaxConstantBytes + align - 1) / align * align;

    VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = m->queueFamily;
    r = vkCreateCommandPool(m->device, &cpi, nullptr, &m->commandPool);
    if (r != VK_SUCCESS) return ToHresult(r);
    r = m->CreateBatches();
    if (r != VK_SUCCESS) return ToHresult(r);

    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = VK_LOD_CLAMP_NONE;
    r = vkCreateSampler(m->device, &si, nullptr, &m->sampler);
    if (r != VK_SUCCESS) return ToHresult(r);

    if (m->timestampBits) {
        VkQueryPoolCreateInfo qpi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qpi.queryCount = 2 * kSlots;
        if (vkCreateQueryPool(m->device, &qpi, nullptr, &m->queries) != VK_SUCCESS) m->queries = VK_NULL_HANDLE;
    }

    for (size_t i = 0; i < size_t(Kernel::Count); ++i) {
        // SPIR-V words must be 4-byte aligned; the generated byte arrays need not be.
        std::vector<uint32_t> code((kKernels[i].size + 3) / 4);
        memcpy(code.data(), kKernels[i].data, kKernels[i].size);
        VkShaderModuleCreateInfo smi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        smi.codeSize = kKernels[i].size;
        smi.pCode = code.data();
        r = vkCreateShaderModule(m->device, &smi, nullptr, &m->kernels[i].module);
        if (r != VK_SUCCESS) return ToHresult(r);
    }

    // Reads given as none see zeros, as a null view does in D3D11.
    HRESULT hr = CreateImage(1, 1, GpuFormat::R32_FLOAT, 0, &m->dummy);
    if (FAILED(hr)) return hr;
    Impl::Batch& b = m->Rec();
    VkClearColorValue zero = {};
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(b.cmd, m->dummy->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
    Impl::Barrier(b.cmd);
    return S_OK;
}

std::string Gpu::DeviceName() const
{
    return m->physical ? std::string(m->props.deviceName) : std::string();
}

HRESULT Gpu::CreateImage(uint32_t width, uint32_t height, GpuFormat format, unsigned flags, GpuImage** out)
{
    const FormatInfo fi = Info(format);
    auto img = std::make_unique<GpuImage>();
    img->width = width;
    img->height = height;
    img->texelBytes = fi.bytes;
    VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = fi.format;
    ici.extent = {width, height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (flags & kStorage) ici.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (flags & kReadback) ici.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult r = vkCreateImage(m->device, &ici, nullptr, &img->image);
    GpuImage* raw = img.get();
    m->images.push_back(std::move(img));  // destroyed with the Gpu, also when the rest fails
    if (r != VK_SUCCESS) return ToHresult(r);
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(m->device, raw->image, &req);
    r = m->AllocateFor(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &raw->memory);
    if (r == VK_SUCCESS) r = vkBindImageMemory(m->device, raw->image, raw->memory, 0);
    if (r != VK_SUCCESS) return ToHresult(r);
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = raw->image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fi.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    r = vkCreateImageView(m->device, &vi, nullptr, &raw->view);
    if (r != VK_SUCCESS) return ToHresult(r);
    if (flags & kReadback)
        for (auto& rb : raw->readback) {
            r = m->MakeHostBuffer(VkDeviceSize(width) * height * fi.bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, rb);
            if (r != VK_SUCCESS) return ToHresult(r);
        }
    m->ToGeneral(raw->image);
    *out = raw;
    return S_OK;
}

HRESULT Gpu::CreateBuffer(uint32_t bytes, bool readback, GpuBuffer** out)
{
    auto buf = std::make_unique<GpuBuffer>();
    buf->bytes = bytes;
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = bytes;
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult r = vkCreateBuffer(m->device, &bi, nullptr, &buf->buffer);
    GpuBuffer* raw = buf.get();
    m->buffers.push_back(std::move(buf));
    if (r != VK_SUCCESS) return ToHresult(r);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(m->device, raw->buffer, &req);
    r = m->AllocateFor(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &raw->memory);
    if (r == VK_SUCCESS) r = vkBindBufferMemory(m->device, raw->buffer, raw->memory, 0);
    if (r != VK_SUCCESS) return ToHresult(r);
    if (readback)
        for (auto& rb : raw->readback) {
            r = m->MakeHostBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, rb);
            if (r != VK_SUCCESS) return ToHresult(r);
        }
    *out = raw;
    return S_OK;
}

void Gpu::Upload(GpuImage* image, const uint8_t* data, uint32_t pitch)
{
    const VkDeviceSize rowBytes = VkDeviceSize(image->width) * image->texelBytes;
    const VkDeviceSize bytes = rowBytes * image->height;
    Impl::Batch* b = &m->Rec();
    if (b->uploadUsed + bytes > b->upload.size) {
        if (b->uploadUsed) {
            m->Submit();  // its commands still read this batch's staging memory
            b = &m->Rec();
        }
        if (bytes > b->upload.size) {
            m->FreeHostBuffer(b->upload);
            if (m->MakeHostBuffer(bytes * 2, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false, b->upload) != VK_SUCCESS) {
                m->FreeHostBuffer(b->upload);
                return;
            }
        }
    }
    uint8_t* dst = b->upload.ptr + b->uploadUsed;
    for (uint32_t y = 0; y < image->height; ++y) memcpy(dst + y * rowBytes, data + size_t(y) * pitch, rowBytes);
    VkBufferImageCopy region = {};
    region.bufferOffset = b->uploadUsed;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {image->width, image->height, 1};
    vkCmdCopyBufferToImage(b->cmd, b->upload.buffer, image->image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    Impl::Barrier(b->cmd);
    b->uploadUsed = (b->uploadUsed + bytes + 255) & ~VkDeviceSize(255);
}

void Gpu::SetConstants(const void* data, uint32_t bytes)
{
    bytes = std::min(bytes, kMaxConstantBytes);
    Impl::Batch* b = &m->Rec();
    if (m->constantsUsed) {
        if (b->constantSlot + 1 >= kConstantSlots) {
            m->Submit();
            b = &m->Rec();
        } else {
            ++b->constantSlot;
        }
    }
    memcpy(b->constants.ptr + size_t(b->constantSlot) * m->constantStride, data, bytes);
    memcpy(m->lastConstants, data, bytes);
    m->lastConstantBytes = bytes;
    m->constantsUsed = false;
}

void Gpu::ClearUint(GpuBuffer* buffer)
{
    Impl::Batch& b = m->Rec();
    vkCmdFillBuffer(b.cmd, buffer->buffer, 0, VK_WHOLE_SIZE, 0);
    Impl::Barrier(b.cmd);
}

void Gpu::ClearFloat(GpuImage* image, float value)
{
    Impl::Batch& b = m->Rec();
    VkClearColorValue color = {};
    for (float& f : color.float32) f = value;
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(b.cmd, image->image, VK_IMAGE_LAYOUT_GENERAL, &color, 1, &range);
    Impl::Barrier(b.cmd);
}

Gpu::Impl::KernelState* Gpu::Impl::Prepare(Kernel kernel, std::initializer_list<GpuRef> reads,
    std::initializer_list<GpuRef> writes)
{
    KernelState& k = kernels[size_t(kernel)];
    std::vector<char> r, w;
    for (const GpuRef& ref : reads) r.push_back(ref.buffer ? 'b' : 'i');  // none: the dummy image
    for (const GpuRef& ref : writes) w.push_back(ref.buffer ? 'b' : 'i');
    if (k.pipeline) {
        if (r == k.reads && w == k.writes) return &k;
        if (!k.mismatchLogged) fprintf(stderr, "ps5cam gpu: kernel %u bound with other resource kinds\n", unsigned(kernel));
        k.mismatchLogged = true;
        return nullptr;
    }
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    auto add = [&](uint32_t binding, VkDescriptorType type) {
        VkDescriptorSetLayoutBinding lb = {};
        lb.binding = binding;
        lb.descriptorType = type;
        lb.descriptorCount = 1;
        lb.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings.push_back(lb);
    };
    add(kBindConstants, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC);
    add(kBindSampler, VK_DESCRIPTOR_TYPE_SAMPLER);
    for (size_t i = 0; i < r.size(); ++i)
        add(kBindRead + uint32_t(i), r[i] == 'b' ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
    for (size_t i = 0; i < w.size(); ++i)
        add(kBindWrite + uint32_t(i), w[i] == 'b' ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    VkDescriptorSetLayoutCreateInfo dli = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dli.bindingCount = uint32_t(bindings.size());
    dli.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(device, &dli, nullptr, &k.setLayout) != VK_SUCCESS) return nullptr;
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &k.setLayout;
    if (vkCreatePipelineLayout(device, &pli, nullptr, &k.layout) != VK_SUCCESS) return nullptr;
    VkComputePipelineCreateInfo cpi = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = k.module;
    cpi.stage.pName = "main";
    cpi.layout = k.layout;
    VkResult res = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpi, nullptr, &k.pipeline);
    if (res != VK_SUCCESS) {
        fprintf(stderr, "ps5cam gpu: kernel %u: pipeline creation failed (%d)\n", unsigned(kernel), int(res));
        k.pipeline = VK_NULL_HANDLE;
        return nullptr;
    }
    k.reads = std::move(r);
    k.writes = std::move(w);
    return &k;
}

void Gpu::Dispatch(Kernel kernel, std::initializer_list<GpuRef> reads, std::initializer_list<GpuRef> writes, uint32_t x,
    uint32_t y, uint32_t z)
{
    Impl::KernelState* k = m->Prepare(kernel, reads, writes);
    if (!k) return;
    Impl::Batch* b = &m->Rec();
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &k->setLayout;
    ai.descriptorPool = b->pool;
    if (b->sets >= kSetsPerBatch || vkAllocateDescriptorSets(m->device, &ai, &set) != VK_SUCCESS) {
        // Out of descriptors: carry on in a new batch, with the same constants.
        m->Submit();
        b = &m->Rec();
        ai.descriptorPool = b->pool;
        if (vkAllocateDescriptorSets(m->device, &ai, &set) != VK_SUCCESS) return;
    }
    ++b->sets;

    std::vector<VkWriteDescriptorSet> writesInfo;
    std::vector<VkDescriptorImageInfo> images;
    std::vector<VkDescriptorBufferInfo> buffers;
    images.reserve(reads.size() + writes.size() + 1);
    buffers.reserve(reads.size() + writes.size() + 1);
    auto write = [&](uint32_t binding, VkDescriptorType type) -> VkWriteDescriptorSet& {
        VkWriteDescriptorSet wd = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        wd.dstSet = set;
        wd.dstBinding = binding;
        wd.descriptorCount = 1;
        wd.descriptorType = type;
        writesInfo.push_back(wd);
        return writesInfo.back();
    };
    buffers.push_back({b->constants.buffer, 0, m->constantStride});
    write(kBindConstants, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC).pBufferInfo = &buffers.back();
    images.push_back({m->sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED});
    write(kBindSampler, VK_DESCRIPTOR_TYPE_SAMPLER).pImageInfo = &images.back();
    uint32_t i = 0;
    for (const GpuRef& ref : reads) {
        if (ref.buffer) {
            buffers.push_back({ref.buffer->buffer, 0, VK_WHOLE_SIZE});
            write(kBindRead + i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER).pBufferInfo = &buffers.back();
        } else {
            GpuImage* img = ref.image ? ref.image : m->dummy;
            images.push_back({VK_NULL_HANDLE, img->view, VK_IMAGE_LAYOUT_GENERAL});
            write(kBindRead + i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE).pImageInfo = &images.back();
        }
        ++i;
    }
    i = 0;
    for (const GpuRef& ref : writes) {
        if (ref.buffer) {
            buffers.push_back({ref.buffer->buffer, 0, VK_WHOLE_SIZE});
            write(kBindWrite + i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER).pBufferInfo = &buffers.back();
        } else if (ref.image) {
            images.push_back({VK_NULL_HANDLE, ref.image->view, VK_IMAGE_LAYOUT_GENERAL});
            write(kBindWrite + i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE).pImageInfo = &images.back();
        } else {
            return;  // a kernel always gets the images it writes
        }
        ++i;
    }
    vkUpdateDescriptorSets(m->device, uint32_t(writesInfo.size()), writesInfo.data(), 0, nullptr);
    vkCmdBindPipeline(b->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, k->pipeline);
    const uint32_t offset = b->constantSlot * m->constantStride;
    vkCmdBindDescriptorSets(b->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, k->layout, 0, 1, &set, 1, &offset);
    vkCmdDispatch(b->cmd, x, y, z);
    Impl::Barrier(b->cmd);
    m->constantsUsed = true;
}

void Gpu::Unbind() {}

void Gpu::CopyToReadback(GpuRef r, int slot)
{
    Impl::Batch& b = m->Rec();
    if (r.image) {
        VkBufferImageCopy region = {};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {r.image->width, r.image->height, 1};
        vkCmdCopyImageToBuffer(b.cmd, r.image->image, VK_IMAGE_LAYOUT_GENERAL, r.image->readback[slot].buffer, 1, &region);
        r.image->readbackBatch[slot] = b.serial;
    } else if (r.buffer) {
        VkBufferCopy region = {0, 0, r.buffer->bytes};
        vkCmdCopyBuffer(b.cmd, r.buffer->buffer, r.buffer->readback[slot].buffer, 1, &region);
        r.buffer->readbackBatch[slot] = b.serial;
    }
    Impl::HostBarrier(b.cmd);
    Impl::Barrier(b.cmd);
}

void Gpu::BeginTiming(int slot)
{
    Impl::Batch& b = m->Rec();
    m->timingWritten[slot] = false;
    if (!m->queries) return;
    vkCmdResetQueryPool(b.cmd, m->queries, uint32_t(slot) * 2, 2);
    vkCmdWriteTimestamp(b.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m->queries, uint32_t(slot) * 2);
}

void Gpu::EndTiming(int slot)
{
    Impl::Batch& b = m->Rec();
    if (!m->queries) return;
    vkCmdWriteTimestamp(b.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m->queries, uint32_t(slot) * 2 + 1);
    m->timingWritten[slot] = true;
}

void Gpu::Flush()
{
    m->Submit();
}

HRESULT Gpu::Map(GpuRef r, int slot, GpuMapped* out)
{
    HostBuffer* rb = r.image ? &r.image->readback[slot] : r.buffer ? &r.buffer->readback[slot] : nullptr;
    if (!rb || !rb->ptr) return E_INVALIDARG;
    const uint64_t serial = r.image ? r.image->readbackBatch[slot] : r.buffer->readbackBatch[slot];
    if (!m->WaitSerial(serial)) return kGpuDeviceLost;
    if (!rb->coherent) {
        VkMappedMemoryRange range = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = rb->memory;
        range.size = VK_WHOLE_SIZE;
        vkInvalidateMappedMemoryRanges(m->device, 1, &range);
    }
    out->data = rb->ptr;
    out->rowPitch = r.image ? r.image->width * r.image->texelBytes : r.buffer->bytes;
    return S_OK;
}

void Gpu::Unmap(GpuRef, int) {}

bool Gpu::TimingMs(int slot, float* ms)
{
    if (!m->queries || !m->timingWritten[slot]) return false;
    uint64_t t[2] = {};
    if (vkGetQueryPoolResults(m->device, m->queries, uint32_t(slot) * 2, 2, sizeof(t), t, sizeof(uint64_t),
            VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
        return false;
    const uint64_t mask = m->timestampBits >= 64 ? ~0ull : (1ull << m->timestampBits) - 1;
    *ms = float(double((t[1] - t[0]) & mask) * m->props.limits.timestampPeriod / 1e6);
    return true;
}

}  // namespace ps5cam
