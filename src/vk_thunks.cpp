#include "vk_thunks.h"
#include "spirv_images.h"
#include "adapter.h"

#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <atomic>
#include <fstream>
#include <filesystem>
#include <vector>

namespace ffxvk {

namespace {

struct VkThunk {
    const char* name;
    PFN_vkVoidFunction* real;
    PFN_vkVoidFunction wrapper;
};
#include "vk_thunks.inc"

const VkThunk* thunkOf(const char* name) {
    for (const VkThunk& t : kThunks)
        if (std::strcmp(t.name, name) == 0) return &t;
    return nullptr;
}

void* libvulkan() {
    static void* lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    return lib;
}

bool verbose() {
    static const bool v = std::getenv("FFXVK_DEBUG") != nullptr;
    return v;
}

PFN_vkGetDeviceProcAddr realDeviceProcAddr;
PFN_vkGetInstanceProcAddr realInstanceProcAddr;
std::mutex fillMutex;
using UpdateProvider = int32_t __attribute__((ms_abi)) (*)(void*, uint32_t);
// The driver DLL's entry, one instance of the DLL per FSR 4 model (it settles on its model once): [0] the GPU's own,
// [1] FSR 4 Lite. A thread's calls go to the instance of the model it is working for (setModel).
UpdateProvider driverProviders[2] = {};
int firstModel = -1;                    // the model of the instance loaded first: its provider keeps AMD's version id
std::atomic<uint64_t> secondId{0};      // the version id the second instance's provider was given (0: none yet)
thread_local int threadModel = 0;
UpdateProvider driverProvider() {
    return driverProviders[threadModel] ? driverProviders[threadModel] : driverProviders[!threadModel];
}

__attribute__((ms_abi)) void msProperties2(VkPhysicalDevice device, VkPhysicalDeviceProperties2* properties) {
    r_vkGetPhysicalDeviceProperties2(device, properties);
    // (for the driver DLL's FSR 4 and for the SDK DLL's own, which asks the same questions when there is no driver DLL)
    identifyDriverAdapter(device, r_vkGetPhysicalDeviceProperties2, properties, !driverProvider());
}

// ---- Faster on RADV: memory types and wave size --------------------------------------------------------------------
// Measured with FSR 3.1.6 and 4.1.2 on an RX 7900 XTX (RADV, Mesa 26.2.6) against the same SDK's D3D12 version through
// vkd3d-proton, which picks its own memory types and wave sizes: at 4K, FSR 4 took 4.3 ms here against ~2.2 there, FSR
// 3 1.15 ms against ~0.4. Two choices of AMD's Vulkan code made the difference; both are changed here, and the probe's
// output is byte-identical either way (FSR 3 and 4, three frames):
// - Memory: the DLL allocates its buffers (~300 MB per 4K context) from RADV's "device coherent, device uncached" types
//   (VK_AMD_device_coherent_memory; RADV lists them whether or not the device enabled the feature), which the GPU's
//   caches skip. They get the cached type with the same other properties in the same heap instead. FFXVK_UNCACHED=1
//   keeps the DLL's choice. And its host-visible buffers (~200 MB at 4K for FSR 4) land in system memory: RADV lists
//   the system-memory type before the CPU-visible VRAM one. With Resizable BAR (a CPU-visible VRAM heap of 4 GB or
//   more) they go to VRAM, the same flags plus device-local. FFXVK_HOST_VRAM=0 leaves them where they are.
// - Wave size: RADV compiles the DLL's compute shaders as wave32 (FSR 4 also asks for 32 on 13 of its 30); vkd3d-proton
//   runs the same passes as wave64. A shader whose workgroup size is a multiple of 64 and that uses no subgroup
//   operations (so its results can't depend on the wave size) gets requiredSubgroupSize 64, an explicit wave32 request
//   included. Needs subgroupSizeControl on the program's device; FFXVK_WAVE64=0 leaves the sizes alone.
// FSR 4 went from 4.3 to 3.2 ms (uncached memory) to 2.7 ms (wave size) to 2.6 ms (host-visible VRAM), FSR 3 from 1.15
// to 0.7 ms (GPU timestamps around the dispatch).

std::mutex tuneMutex;
VkPhysicalDeviceMemoryProperties memoryTypes{};   // the DLL's own query (one device: the program's)
std::vector<std::pair<VkShaderModule, bool>> modules;   // module, may run as wave64

bool cachedMemory() {
    static const bool on = [] { const char* e = std::getenv("FFXVK_UNCACHED"); return !e || e[0] == '0'; }();
    return on;
}
bool hostVram() {
    static const bool on = [] { const char* e = std::getenv("FFXVK_HOST_VRAM"); return !e || e[0] != '0'; }();
    return on;
}
bool wave64() {
    static const bool on = [] { const char* e = std::getenv("FFXVK_WAVE64"); return !e || e[0] != '0'; }();
    return on;
}

// a compute shader that can run as wave64: no subgroup operations, a workgroup size that is a multiple of 64
bool wave64Safe(const uint32_t* w, size_t n) {
    uint32_t total = 0;
    for (size_t i = 5; i < n;) {
        const uint32_t op = w[i] & 0xffff, count = w[i] >> 16;
        if (!count || i + count > n) return false;
        if (op == 17 && w[i + 1] >= 61 && w[i + 1] <= 69) return false;   // Capability GroupNonUniform*
        if (op == 17 && (w[i + 1] == 4423 || w[i + 1] == 5568 || w[i + 1] == 5569)) return false;   // SubgroupBallotKHR, Intel shuffles
        if (op == 16 && count >= 6 && w[i + 2] == 17) total = w[i + 3] * w[i + 4] * w[i + 5];   // ExecutionMode LocalSize
        if (op == 331) return false;   // ExecutionModeId (LocalSizeId: not known here)
        i += count;
    }
    return total && total % 64 == 0;
}

void noteModule(VkShaderModule module, const uint32_t* words, size_t n) {
    if (!wave64()) return;
    std::lock_guard<std::mutex> lock(tuneMutex);
    modules.push_back({module, wave64Safe(words, n)});
}

__attribute__((ms_abi)) void msMemoryProperties(VkPhysicalDevice device, VkPhysicalDeviceMemoryProperties* p) {
    r_vkGetPhysicalDeviceMemoryProperties(device, p);
    std::lock_guard<std::mutex> lock(tuneMutex);
    memoryTypes = *p;
}
__attribute__((ms_abi)) void msMemoryProperties2(VkPhysicalDevice device, VkPhysicalDeviceMemoryProperties2* p) {
    r_vkGetPhysicalDeviceMemoryProperties2(device, p);
    std::lock_guard<std::mutex> lock(tuneMutex);
    memoryTypes = p->memoryProperties;
}

__attribute__((ms_abi)) VkResult msAllocateMemory(VkDevice device, const VkMemoryAllocateInfo* info,
        const VkAllocationCallbacks* allocator, VkDeviceMemory* out) {
    const VkMemoryPropertyFlags amd = VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD | VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD;
    if (cachedMemory() && info) {
        VkPhysicalDeviceMemoryProperties m;
        { std::lock_guard<std::mutex> lock(tuneMutex); m = memoryTypes; }
        const uint32_t t = info->memoryTypeIndex;
        if (t < m.memoryTypeCount && (m.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD)) {
            const VkMemoryPropertyFlags want = m.memoryTypes[t].propertyFlags & ~amd;
            for (uint32_t i = 0; i < m.memoryTypeCount; ++i)
                if (m.memoryTypes[i].propertyFlags == want && m.memoryTypes[i].heapIndex == m.memoryTypes[t].heapIndex) {
                    VkMemoryAllocateInfo cached = *info;
                    cached.memoryTypeIndex = i;
                    if (verbose()) std::fprintf(stderr, "ffx-vk-linux: %llu bytes: memory type %u (uncached) -> %u\n", (unsigned long long)info->allocationSize, t, i);
                    return r_vkAllocateMemory(device, &cached, allocator, out);
                }
        }
    }
    // host-visible allocations in system memory: the device-local, host-visible type with the same flags where that heap
    // is large (Resizable BAR), as AMD's Windows driver would give them
    if (hostVram() && info) {
        VkPhysicalDeviceMemoryProperties m;
        { std::lock_guard<std::mutex> lock(tuneMutex); m = memoryTypes; }
        const uint32_t t = info->memoryTypeIndex;
        if (t < m.memoryTypeCount) {
            const VkMemoryPropertyFlags f = m.memoryTypes[t].propertyFlags;
            if ((f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && !(f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
                for (uint32_t i = 0; i < m.memoryTypeCount; ++i) {
                    const auto& heap = m.memoryHeaps[m.memoryTypes[i].heapIndex];
                    if (m.memoryTypes[i].propertyFlags == (f | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) && (heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && heap.size >= (4ull << 30)) {
                        VkMemoryAllocateInfo vram = *info;
                        vram.memoryTypeIndex = i;
                        if (verbose()) std::fprintf(stderr, "ffx-vk-linux: %llu bytes: memory type %u (system) -> %u (VRAM)\n", (unsigned long long)info->allocationSize, t, i);
                        return r_vkAllocateMemory(device, &vram, allocator, out);
                    }
                }
        }
    }
    return r_vkAllocateMemory(device, info, allocator, out);
}

__attribute__((ms_abi)) VkResult msCreateComputePipelines(VkDevice device, VkPipelineCache cache, uint32_t n,
        const VkComputePipelineCreateInfo* infos, const VkAllocationCallbacks* allocator, VkPipeline* out) {
    if (!wave64() || !infos) return r_vkCreateComputePipelines(device, cache, n, infos, allocator, out);
    std::vector<VkComputePipelineCreateInfo> copy(infos, infos + n);
    std::vector<VkPipelineShaderStageRequiredSubgroupSizeCreateInfo> sizes(n);
    unsigned changed = 0;
    for (uint32_t i = 0; i < n; ++i) {
        bool safe = false;
        {
            std::lock_guard<std::mutex> lock(tuneMutex);
            for (const auto& [module, ok] : modules) if (module == infos[i].stage.module) safe = ok;
        }
        if (!safe) continue;
        // an explicit wave32 request alone in the stage's chain is replaced; any other chain or size stays as it is
        const VkBaseInStructure* chain = static_cast<const VkBaseInStructure*>(infos[i].stage.pNext);
        if (chain && !(chain->sType == VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO && !chain->pNext &&
                       reinterpret_cast<const VkPipelineShaderStageRequiredSubgroupSizeCreateInfo*>(chain)->requiredSubgroupSize == 32))
            continue;
        sizes[i] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO, nullptr, 64};
        copy[i].stage.pNext = &sizes[i];
        copy[i].stage.flags &= ~VK_PIPELINE_SHADER_STAGE_CREATE_ALLOW_VARYING_SUBGROUP_SIZE_BIT;
        ++changed;
    }
    if (verbose() && changed) std::fprintf(stderr, "ffx-vk-linux: %u of %u compute pipelines as wave64\n", changed, n);
    return r_vkCreateComputePipelines(device, cache, n, copy.data(), allocator, out);
}

// ---- On a compute-only queue (FSR run asynchronously) ---------------------------------------------------------------
// FSR's passes are compute; on a compute-only queue family (FFX records into whatever command buffer the program gives
// it) the output is the same - the probe's --compute-queue. One barrier per context, on its first use, names graphics
// stages: AMD's backend moves its own new images to COLOR_ATTACHMENT_OPTIMAL with COLOR_ATTACHMENT_OUTPUT in its stage
// masks, which a compute queue's command buffer may not hold. Graphics-only stages become ALL_COMMANDS and graphics-only
// access bits MEMORY_READ / MEMORY_WRITE - a superset, valid on every queue: on a graphics queue only more conservative.
constexpr VkPipelineStageFlags kGraphicsStages = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
    VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT | VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT |
    VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
    VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT;
constexpr VkAccessFlags kGraphicsReads = VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INPUT_ATTACHMENT_READ_BIT |
    VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
constexpr VkAccessFlags kGraphicsWrites = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

VkPipelineStageFlags anyQueueStages(VkPipelineStageFlags s) {
    return (s & kGraphicsStages) ? (s & ~kGraphicsStages) | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : s;
}
VkAccessFlags anyQueueAccess(VkAccessFlags a) {
    if (a & kGraphicsReads) a = (a & ~kGraphicsReads) | VK_ACCESS_MEMORY_READ_BIT;
    if (a & kGraphicsWrites) a = (a & ~kGraphicsWrites) | VK_ACCESS_MEMORY_WRITE_BIT;
    return a;
}

__attribute__((ms_abi)) void msCmdPipelineBarrier(VkCommandBuffer cmd, VkPipelineStageFlags src, VkPipelineStageFlags dst, VkDependencyFlags dep,
        uint32_t nm, const VkMemoryBarrier* m, uint32_t nb, const VkBufferMemoryBarrier* b, uint32_t ni, const VkImageMemoryBarrier* im) {
    if (!((src | dst) & kGraphicsStages)) return r_vkCmdPipelineBarrier(cmd, src, dst, dep, nm, m, nb, b, ni, im);
    std::vector<VkMemoryBarrier> mm(m, m + nm);
    std::vector<VkBufferMemoryBarrier> bb(b, b + nb);
    std::vector<VkImageMemoryBarrier> ii(im, im + ni);
    for (auto& x : mm) x.srcAccessMask = anyQueueAccess(x.srcAccessMask), x.dstAccessMask = anyQueueAccess(x.dstAccessMask);
    for (auto& x : bb) x.srcAccessMask = anyQueueAccess(x.srcAccessMask), x.dstAccessMask = anyQueueAccess(x.dstAccessMask);
    for (auto& x : ii) x.srcAccessMask = anyQueueAccess(x.srcAccessMask), x.dstAccessMask = anyQueueAccess(x.dstAccessMask);
    if (verbose()) std::fprintf(stderr, "ffx-vk-linux: barrier stages 0x%x->0x%x: graphics stages as ALL_COMMANDS\n", src, dst);
    r_vkCmdPipelineBarrier(cmd, anyQueueStages(src), anyQueueStages(dst), dep, nm, mm.data(), nb, bb.data(), ni, ii.data());
}

// ---- FSR 4's own images: linear tiling ------------------------------------------------------------------------------
// FSR 4's last pass writes three full-size images (its two histories and the program's output), a 2x2 block of pixels per
// thread. On RADV (RX 7900 XTX, Mesa 26.2.6) that pass takes 0.80 ms at 4K when the program's output image is RGBA16F
// like FSR's own histories, 0.54 ms with a 32-bit output, and 0.48 ms either way once FSR's RGBA16F images are linear
// instead of tiled: FSR 4 as a whole 2.64 -> 2.32 ms with an RGBA16F output (1.25 -> 1.10 ms at 1440p, the same at
// 1080p), 2.41 -> 2.35 ms with a 32-bit one. The output is byte-identical (the probe). Why the tiled layout costs this
// was not established. FSR 3 measures 1 to 3% slower with linear images, so only an FSR 4 context's images change: the
// DLL's backend makes every version's images, so the version comes from the context being made or dispatched
// (setUpscaler). Only on RADV, where it was measured, and only where the driver takes the image as linear.
// FFXVK_LINEAR_IMAGES=0 leaves the tiling alone.
thread_local int upscalerMajor = 0;
thread_local VkPhysicalDevice upscalerDevice = VK_NULL_HANDLE;

bool linearImages() {
    static const bool on = [] { const char* e = std::getenv("FFXVK_LINEAR_IMAGES"); return !e || e[0] != '0'; }();
    return on;
}

bool isRadv(VkPhysicalDevice device) {
    static std::mutex m;
    static std::vector<std::pair<VkPhysicalDevice, bool>> known;
    std::lock_guard<std::mutex> lock(m);
    for (const auto& [d, radv] : known) if (d == device) return radv;
    bool radv = false;
    if (auto get = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(dlsym(libvulkan(), "vkGetPhysicalDeviceProperties2"))) {
        VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 p{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &driver};
        get(device, &p);
        radv = driver.driverID == VK_DRIVER_ID_MESA_RADV;
    }
    known.push_back({device, radv});
    return radv;
}

__attribute__((ms_abi)) VkResult msCreateImage(VkDevice device, const VkImageCreateInfo* info, const VkAllocationCallbacks* allocator, VkImage* out) {
    if (linearImages() && info && upscalerMajor == 4 && upscalerDevice && info->tiling == VK_IMAGE_TILING_OPTIMAL &&
        info->imageType == VK_IMAGE_TYPE_2D && info->format == VK_FORMAT_R16G16B16A16_SFLOAT && info->mipLevels == 1 && info->arrayLayers == 1 &&
        info->samples == VK_SAMPLE_COUNT_1_BIT && (info->usage & VK_IMAGE_USAGE_STORAGE_BIT) && !info->pNext && isRadv(upscalerDevice)) {
        static const auto supported = reinterpret_cast<PFN_vkGetPhysicalDeviceImageFormatProperties>(dlsym(libvulkan(), "vkGetPhysicalDeviceImageFormatProperties"));
        VkImageFormatProperties p{};
        if (supported && supported(upscalerDevice, info->format, info->imageType, VK_IMAGE_TILING_LINEAR, info->usage, info->flags, &p) == VK_SUCCESS &&
            p.maxExtent.width >= info->extent.width && p.maxExtent.height >= info->extent.height) {
            VkImageCreateInfo linear = *info;
            linear.tiling = VK_IMAGE_TILING_LINEAR;
            if (verbose()) std::fprintf(stderr, "ffx-vk-linux: FSR 4's %ux%u RGBA16F image (usage 0x%x) as linear\n", info->extent.width, info->extent.height, info->usage);
            return r_vkCreateImage(device, &linear, allocator, out);
        }
    }
    return r_vkCreateImage(device, info, allocator, out);
}

__attribute__((ms_abi)) VkResult msCreateShaderModule(VkDevice device,
        const VkShaderModuleCreateInfo* info, const VkAllocationCallbacks* allocator, VkShaderModule* out) {
    if (!info || !info->pCode || info->codeSize % sizeof(uint32_t)) return VK_ERROR_INVALID_SHADER_NV;
    auto rewritten = rewriteHalfImageTypes({info->pCode, info->codeSize / sizeof(uint32_t)}, true);
    if (rewritten.status == ImageRewriteStatus::Invalid) {
        std::fprintf(stderr, "ffx-vk-linux: shader normalization failed: %s\n", rewritten.error.c_str());
        return VK_ERROR_INVALID_SHADER_NV;
    }
    VkShaderModuleCreateInfo normalized = *info;
    if (rewritten.status == ImageRewriteStatus::Rewritten) {
        normalized.pCode = rewritten.words.data();
        normalized.codeSize = rewritten.words.size() * sizeof(uint32_t);
        if (verbose()) std::fprintf(stderr, "ffx-vk-linux: normalized half-float image shader (%zu -> %zu bytes)\n", info->codeSize, normalized.codeSize);
    }
    if (const char* directory = std::getenv("FFXVK_SHADER_DUMP")) {
        static std::atomic<unsigned> next{0};
        const auto path = std::filesystem::path(directory) / (std::to_string(next++) + ".spv");
        std::ofstream file(path, std::ios::binary);
        file.write(reinterpret_cast<const char*>(normalized.pCode), normalized.codeSize);
    }
    VkResult result = r_vkCreateShaderModule(device, &normalized, allocator, out);
    if (result == VK_SUCCESS) noteModule(*out, normalized.pCode, normalized.codeSize / sizeof(uint32_t));
    return result;
}

// The SDK's request for the driver's provider (made with every query and context): answered by the instance of the
// thread's model. The SDK keeps one provider per version id, the first it is given, so the second instance's would
// never be used: its id gets a bit no version number has (0x800 of the patch number), which makes it a version of its
// own to the SDK, one a context is made with by naming it (ffxOverrideVersion; ffxvk.cpp does that and keeps it out of
// the program's version list).
__attribute__((ms_abi)) int32_t msUpdateProvider(VkDevice, void* data, uint32_t size) {
    const UpdateProvider provider = driverProvider();
    const int32_t result = provider(data, size);
    uint64_t id = 0;
    if (data && size >= 24) std::memcpy(&id, static_cast<const char*>(data) + 16, 8);
    if (result == 0 && id && firstModel >= 0 && provider == driverProviders[!firstModel]) {
        id |= 0x800;
        std::memcpy(static_cast<char*>(data) + 16, &id, 8);
        secondId = id;
    }
    if (verbose()) std::fprintf(stderr, "ffx-vk-linux: driver provider request (%u bytes): 0x%08x, id %016llx\n", size, uint32_t(result), (unsigned long long)id);
    return result;
}

// the hand-written wrappers in place of the generated ones (null: the generated one)
PFN_vkVoidFunction special(const char* name) {
    if (std::strcmp(name, "vkGetPhysicalDeviceProperties2") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&msProperties2);
    if (std::strcmp(name, "vkCreateShaderModule") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&msCreateShaderModule);
    if (std::strcmp(name, "vkGetPhysicalDeviceMemoryProperties") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&msMemoryProperties);
    if (std::strcmp(name, "vkGetPhysicalDeviceMemoryProperties2") == 0 || std::strcmp(name, "vkGetPhysicalDeviceMemoryProperties2KHR") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&msMemoryProperties2);
    if (std::strcmp(name, "vkAllocateMemory") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&msAllocateMemory);
    if (std::strcmp(name, "vkCreateComputePipelines") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&msCreateComputePipelines);
    if (std::strcmp(name, "vkCmdPipelineBarrier") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&msCmdPipelineBarrier);
    if (std::strcmp(name, "vkCreateImage") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&msCreateImage);
    return nullptr;
}

// the DLL's vkGetDeviceProcAddr: the real function recorded into its thunk, the thunk given
__attribute__((ms_abi)) PFN_vkVoidFunction msGetDeviceProcAddr(VkDevice device, const char* name) {
    if (driverProvider() && std::strcmp(name, "vkUpdateFfxApiProviderAMD") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&msUpdateProvider);
    const VkThunk* t = thunkOf(name);
    PFN_vkVoidFunction real = realDeviceProcAddr ? realDeviceProcAddr(device, name) : nullptr;
    if (verbose()) std::fprintf(stderr, "ffx-vk-linux: vkGetDeviceProcAddr(%s) %s\n", name, !real ? "none" : t ? "" : "(no thunk)");
    if (!real || !t) return nullptr;
    std::lock_guard<std::mutex> lock(fillMutex);
    *t->real = real; // (one device at a time: the program's)
    if (PFN_vkVoidFunction s = special(name)) return s;
    return t->wrapper;
}
__attribute__((ms_abi)) PFN_vkVoidFunction msGetInstanceProcAddr(VkInstance instance, const char* name) {
    const VkThunk* t = thunkOf(name);
    PFN_vkVoidFunction real = realInstanceProcAddr ? realInstanceProcAddr(instance, name) : nullptr;
    if (verbose()) std::fprintf(stderr, "ffx-vk-linux: vkGetInstanceProcAddr(%s) %s\n", name, !real ? "none" : t ? "" : "(no thunk)");
    if (!real || !t) return nullptr;
    std::lock_guard<std::mutex> lock(fillMutex);
    *t->real = real;
    if (PFN_vkVoidFunction s = special(name)) return s;
    return t->wrapper;
}

// (FFXVK_DEBUG: what the DLL asks of the GPU - the structures it chains into its queries)
void logChain(const char* what, const void* p) {
    std::string s;
    for (auto* h = static_cast<const VkBaseInStructure*>(p); h; h = h->pNext) s += " " + std::to_string(int(h->sType));
    std::fprintf(stderr, "ffx-vk-linux: %s:%s\n", what, s.c_str());
}
__attribute__((ms_abi)) void dbgProperties2(VkPhysicalDevice d, VkPhysicalDeviceProperties2* p) {
    logChain("vkGetPhysicalDeviceProperties2", p);
    r_vkGetPhysicalDeviceProperties2(d, p);
}
__attribute__((ms_abi)) void dbgFeatures2(VkPhysicalDevice d, VkPhysicalDeviceFeatures2* p) {
    logChain("vkGetPhysicalDeviceFeatures2", p);
    r_vkGetPhysicalDeviceFeatures2(d, p);
}
__attribute__((ms_abi)) VkResult dbgExtensions(VkPhysicalDevice d, const char* layer, uint32_t* n, VkExtensionProperties* e) {
    std::fprintf(stderr, "ffx-vk-linux: vkEnumerateDeviceExtensionProperties(%s)\n", e ? "list" : "count");
    return r_vkEnumerateDeviceExtensionProperties(d, layer, n, e);
}

} // namespace

void* vulkanImport(const std::string& name) {
    if (name == "vkGetDeviceProcAddr") {
        if (!realDeviceProcAddr && libvulkan()) realDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(dlsym(libvulkan(), "vkGetDeviceProcAddr"));
        return reinterpret_cast<void*>(&msGetDeviceProcAddr);
    }
    if (name == "vkGetInstanceProcAddr") {
        if (!realInstanceProcAddr && libvulkan()) realInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(libvulkan(), "vkGetInstanceProcAddr"));
        return reinterpret_cast<void*>(&msGetInstanceProcAddr);
    }
    const VkThunk* t = thunkOf(name.c_str());
    if (!t || !libvulkan()) return nullptr;
    *t->real = reinterpret_cast<PFN_vkVoidFunction>(dlsym(libvulkan(), name.c_str())); // (the loader's exported entry: any device, any instance)
    if (!*t->real) return nullptr;
    if (PFN_vkVoidFunction s = special(name.c_str())) return reinterpret_cast<void*>(s);
    if (verbose() && name == "vkGetPhysicalDeviceProperties2") return reinterpret_cast<void*>(&dbgProperties2);
    if (verbose() && name == "vkGetPhysicalDeviceFeatures2") return reinterpret_cast<void*>(&dbgFeatures2);
    if (verbose() && name == "vkEnumerateDeviceExtensionProperties") return reinterpret_cast<void*>(&dbgExtensions);
    return reinterpret_cast<void*>(t->wrapper);
}

PFN_vkVoidFunction msGetDeviceProcAddrFor(PFN_vkGetDeviceProcAddr real) {
    if (real) realDeviceProcAddr = real;
    return reinterpret_cast<PFN_vkVoidFunction>(&msGetDeviceProcAddr);
}

PFN_vkVoidFunction msGetInstanceProcAddrFor(PFN_vkGetInstanceProcAddr real) {
    if (real) realInstanceProcAddr = real;
    return reinterpret_cast<PFN_vkVoidFunction>(&msGetInstanceProcAddr);
}

void setDriverProvider(int model, void* update) {
    driverProviders[model != 0] = reinterpret_cast<UpdateProvider>(update);
    if (firstModel < 0) firstModel = model != 0;
}
int firstDriverModel() { return firstModel; }
uint64_t secondProviderId() { return secondId; }
void setModel(int model) { threadModel = model != 0; }
int model() { return threadModel; }
void setUpscaler(int major, VkPhysicalDevice device) { upscalerMajor = major; upscalerDevice = device; }

} // namespace ffxvk
