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
UpdateProvider driverProvider = nullptr;

__attribute__((ms_abi)) void msProperties2(VkPhysicalDevice device, VkPhysicalDeviceProperties2* properties) {
    r_vkGetPhysicalDeviceProperties2(device, properties);
    if (!driverProvider) return;
    identifyDriverAdapter(device, r_vkGetPhysicalDeviceProperties2, properties);
}

__attribute__((ms_abi)) VkResult msCreateShaderModule(VkDevice device,
        const VkShaderModuleCreateInfo* info, const VkAllocationCallbacks* allocator, VkShaderModule* out) {
    if (!info || !info->pCode || info->codeSize % sizeof(uint32_t)) return VK_ERROR_INVALID_SHADER_NV;
    auto rewritten = rewriteHalfImageTypes({info->pCode, info->codeSize / sizeof(uint32_t)}, driverProvider != nullptr);
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
    return r_vkCreateShaderModule(device, &normalized, allocator, out);
}

__attribute__((ms_abi)) int32_t msUpdateProvider(VkDevice, void* data, uint32_t size) {
    const int32_t result = driverProvider(data, size);
    if (verbose()) {
        uint64_t id = 0;
        if (data && size >= 24) std::memcpy(&id, static_cast<const char*>(data) + 16, 8);
        std::fprintf(stderr, "ffx-vk-linux: driver provider request (%u bytes): 0x%08x, id %016llx\n", size, uint32_t(result), (unsigned long long)id);
    }
    return result;
}

// the DLL's vkGetDeviceProcAddr: the real function recorded into its thunk, the thunk given
__attribute__((ms_abi)) PFN_vkVoidFunction msGetDeviceProcAddr(VkDevice device, const char* name) {
    if (driverProvider && std::strcmp(name, "vkUpdateFfxApiProviderAMD") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&msUpdateProvider);
    const VkThunk* t = thunkOf(name);
    PFN_vkVoidFunction real = realDeviceProcAddr ? realDeviceProcAddr(device, name) : nullptr;
    if (verbose()) std::fprintf(stderr, "ffx-vk-linux: vkGetDeviceProcAddr(%s) %s\n", name, !real ? "none" : t ? "" : "(no thunk)");
    if (!real || !t) return nullptr;
    std::lock_guard<std::mutex> lock(fillMutex);
    *t->real = real; // (one device at a time: the program's)
    if (std::strcmp(name, "vkGetPhysicalDeviceProperties2") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&msProperties2);
    if (std::strcmp(name, "vkCreateShaderModule") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&msCreateShaderModule);
    return t->wrapper;
}
__attribute__((ms_abi)) PFN_vkVoidFunction msGetInstanceProcAddr(VkInstance instance, const char* name) {
    const VkThunk* t = thunkOf(name);
    PFN_vkVoidFunction real = realInstanceProcAddr ? realInstanceProcAddr(instance, name) : nullptr;
    if (verbose()) std::fprintf(stderr, "ffx-vk-linux: vkGetInstanceProcAddr(%s) %s\n", name, !real ? "none" : t ? "" : "(no thunk)");
    if (!real || !t) return nullptr;
    std::lock_guard<std::mutex> lock(fillMutex);
    *t->real = real;
    if (std::strcmp(name, "vkGetPhysicalDeviceProperties2") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&msProperties2);
    if (std::strcmp(name, "vkCreateShaderModule") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&msCreateShaderModule);
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
    if (name == "vkGetPhysicalDeviceProperties2") return reinterpret_cast<void*>(&msProperties2);
    if (name == "vkCreateShaderModule") return reinterpret_cast<void*>(&msCreateShaderModule);
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

void setDriverProvider(void* update) { driverProvider = reinterpret_cast<UpdateProvider>(update); }

} // namespace ffxvk
