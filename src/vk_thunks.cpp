#include "vk_thunks.h"

#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

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

// the DLL's vkGetDeviceProcAddr: the real function recorded into its thunk, the thunk given
__attribute__((ms_abi)) PFN_vkVoidFunction msGetDeviceProcAddr(VkDevice device, const char* name) {
    const VkThunk* t = thunkOf(name);
    PFN_vkVoidFunction real = realDeviceProcAddr ? realDeviceProcAddr(device, name) : nullptr;
    if (verbose()) std::fprintf(stderr, "ffx-vk-linux: vkGetDeviceProcAddr(%s) %s\n", name, !real ? "none" : t ? "" : "(no thunk)");
    if (!real || !t) return nullptr;
    std::lock_guard<std::mutex> lock(fillMutex);
    *t->real = real; // (one device at a time: the program's)
    return t->wrapper;
}
__attribute__((ms_abi)) PFN_vkVoidFunction msGetInstanceProcAddr(VkInstance instance, const char* name) {
    const VkThunk* t = thunkOf(name);
    PFN_vkVoidFunction real = realInstanceProcAddr ? realInstanceProcAddr(instance, name) : nullptr;
    if (verbose()) std::fprintf(stderr, "ffx-vk-linux: vkGetInstanceProcAddr(%s) %s\n", name, !real ? "none" : t ? "" : "(no thunk)");
    if (!real || !t) return nullptr;
    std::lock_guard<std::mutex> lock(fillMutex);
    *t->real = real;
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

} // namespace ffxvk
