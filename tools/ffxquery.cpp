// Exercise the native SDK/provider path, optionally recording and checking a real upscale.
#include "ffx_vk_linux.h"
#include "upscale_smoke.h"

#include <atomic>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace {
std::atomic<unsigned> validationErrors{0};
VKAPI_ATTR VkBool32 VKAPI_CALL validation(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++validationErrors;
    std::fprintf(stderr, "[Vulkan] %s\n", data->pMessage);
    return VK_FALSE;
}
void message(uint32_t type, const wchar_t* text) { std::fprintf(stderr, "[ffx %u] %ls\n", type, text); }
struct Vulkan {
    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    void cleanup() {
        if (device) { vkDeviceWaitIdle(device); vkDestroyDevice(device, nullptr); }
        if (messenger) {
            auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
            destroy(instance, messenger, nullptr);
        }
        if (instance) vkDestroyInstance(instance, nullptr);
        device = VK_NULL_HANDLE; messenger = VK_NULL_HANDLE; instance = VK_NULL_HANDLE;
    }
    ~Vulkan() { cleanup(); }
};
}

int main(int argc, char** argv) {
    bool requireFsr4 = false, dispatch = false, validate = false;
    const char* directory = nullptr;
    const char* output = nullptr;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--require-fsr4") requireFsr4 = true;
        else if (arg == "--dispatch") dispatch = true;
        else if (arg == "--validate") validate = true;
        else if (arg == "--output" && i + 1 < argc) { output = argv[++i]; dispatch = true; }
        else if (!directory && arg[0] != '-') directory = argv[i];
        else { std::fprintf(stderr, "unknown or incomplete argument: %s\n", argv[i]); return 2; }
    }
    if (!directory) {
        std::fprintf(stderr, "ffxquery <SDK DLL directory> [--require-fsr4] [--dispatch] [--validate] [--output image.ppm]\n");
        return 2;
    }
    Vulkan vk;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "ffxquery";
    app.apiVersion = VK_API_VERSION_1_3;
    const char* layer = "VK_LAYER_KHRONOS_validation";
    const char* extension = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    debug.pfnUserCallback = validation;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    if (validate) {
        ici.enabledLayerCount = 1; ici.ppEnabledLayerNames = &layer;
        ici.enabledExtensionCount = 1; ici.ppEnabledExtensionNames = &extension;
        ici.pNext = &debug;
    }
    if (VkResult r = vkCreateInstance(&ici, nullptr, &vk.instance); r != VK_SUCCESS) {
        std::fprintf(stderr, "no Vulkan instance (%d)\n", r); return 1;
    }
    if (validate) {
        auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(vk.instance, "vkCreateDebugUtilsMessengerEXT"));
        if (!create || create(vk.instance, &debug, nullptr, &vk.messenger) != VK_SUCCESS) return 1;
    }
    uint32_t n = 0;
    if (vkEnumeratePhysicalDevices(vk.instance, &n, nullptr) != VK_SUCCESS || !n) {
        std::fprintf(stderr, "no GPU\n"); return 1;
    }
    std::vector<VkPhysicalDevice> gpus(n);
    if (vkEnumeratePhysicalDevices(vk.instance, &n, gpus.data()) != VK_SUCCESS) return 1;
    VkPhysicalDevice gpu = gpus[0];
    for (auto candidate : gpus) {
        VkPhysicalDeviceProperties p{}; vkGetPhysicalDeviceProperties(candidate, &p);
        if (p.vendorID == 0x1002 && p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { gpu = candidate; break; }
    }
    VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &driver};
    vkGetPhysicalDeviceProperties2(gpu, &props);
    std::printf("GPU: %s (%s %s)\n", props.properties.deviceName, driver.driverName, driver.driverInfo);
    if (props.properties.apiVersion < VK_API_VERSION_1_3) { std::fprintf(stderr, "Vulkan 1.3 required by this probe\n"); return 1; }
    uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(gpu, &nq, nullptr);
    std::vector<VkQueueFamilyProperties> queues(nq);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &nq, queues.data());
    uint32_t family = 0;
    while (family < nq && !(queues[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) ++family;
    if (family == nq) { std::fprintf(stderr, "no compute queue\n"); return 1; }

    // Core 1.3 includes float16/int8, integer dot product and subgroup-size control.
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &f12};
    VkPhysicalDeviceFeatures2 feats{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f11};
    uint32_t extensionCount = 0;
    if (vkEnumerateDeviceExtensionProperties(gpu, nullptr, &extensionCount, nullptr) != VK_SUCCESS) return 1;
    std::vector<VkExtensionProperties> available(extensionCount);
    if (vkEnumerateDeviceExtensionProperties(gpu, nullptr, &extensionCount, available.data()) != VK_SUCCESS) return 1;
    auto supports = [&](const char* name) {
        return std::any_of(available.begin(), available.end(), [&](const auto& e) { return std::strcmp(e.extensionName, name) == 0; });
    };
    std::vector<const char*> extensions;
    VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR derivatives{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_KHR};
    VkPhysicalDeviceCoherentMemoryFeaturesAMD coherent{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COHERENT_MEMORY_FEATURES_AMD};
    if (supports(VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME)) {
        extensions.push_back(VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME);
        derivatives.pNext = f13.pNext; f13.pNext = &derivatives;
    }
    if (supports(VK_AMD_DEVICE_COHERENT_MEMORY_EXTENSION_NAME)) {
        extensions.push_back(VK_AMD_DEVICE_COHERENT_MEMORY_EXTENSION_NAME);
        coherent.pNext = f13.pNext; f13.pNext = &coherent;
    }
    vkGetPhysicalDeviceFeatures2(gpu, &feats);
    float priority = 1;
    VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    q.queueFamilyIndex = family; q.queueCount = 1; q.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &feats};
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &q;
    dci.enabledExtensionCount = uint32_t(extensions.size()); dci.ppEnabledExtensionNames = extensions.data();
    if (VkResult r = vkCreateDevice(gpu, &dci, nullptr, &vk.device); r != VK_SUCCESS) {
        std::fprintf(stderr, "no device (%d)\n", r); return 1;
    }
    if (ffxvkLoad(directory) != 0) { std::fprintf(stderr, "ffxvkLoad: %s\n", ffxvkError()); return 1; }
    std::printf("AMD's DLL loaded\n");
    VkDeviceContext dc{vk.device, gpu, vkGetDeviceProcAddr};
    uint64_t count = 0;
    ffxQueryDescGetVersions v{};
    v.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    v.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    v.device = &dc; v.outputCount = &count;
    ffxReturnCode_t r = ffxQuery(nullptr, &v.header);
    if (r != FFX_API_RETURN_OK || !count || count > 256) {
        std::fprintf(stderr, "versions query failed: %u (%llu versions)\n", r, (unsigned long long)count); return 1;
    }
    std::vector<uint64_t> ids(count);
    std::vector<const char*> names(count);
    v.versionIds = ids.data(); v.versionNames = names.data();
    r = ffxQuery(nullptr, &v.header);
    if (r != FFX_API_RETURN_OK || count > ids.size()) return 1;
    uint64_t fsr4 = 0;
    for (uint64_t i = 0; i < count; ++i) {
        std::printf("  %016llx %s\n", (unsigned long long)ids[i], names[i] ? names[i] : "?");
        if (names[i] && names[i][0] == '4' && names[i][1] == '.') fsr4 = ids[i];
    }
    if (requireFsr4 && !fsr4) { std::fprintf(stderr, "FSR 4 required but unavailable; refusing fallback\n"); return 1; }
    ffxCreateContextDescUpscaleVersion version{};
    version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    version.version = FFX_UPSCALER_VERSION;
    ffxOverrideVersion override{};
    override.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    override.header.pNext = &version.header;
    override.versionId = fsr4;
    ffxCreateBackendVKDesc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    backend.header.pNext = requireFsr4 ? &override.header : &version.header;
    backend.deviceContext = dc;
    ffxCreateContextDescUpscale up{};
    up.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    up.header.pNext = &backend.header;
    up.maxRenderSize = dispatch ? FfxApiDimensions2D{128, 72} : FfxApiDimensions2D{1280, 720};
    up.maxUpscaleSize = dispatch ? FfxApiDimensions2D{256, 144} : FfxApiDimensions2D{2560, 1440};
    up.flags = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE | FFX_UPSCALE_ENABLE_DEBUG_CHECKING;
    up.fpMessage = message;
    ffxContext ctx = nullptr;
    r = ffxCreateContext(&ctx, &up.header, nullptr);
    std::printf("upscale context: %u\n", r);
    if (r != FFX_API_RETURN_OK) return 1;
    ffxQueryGetProviderVersion pv{};
    pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    r = ffxQuery(&ctx, &pv.header);
    std::printf("provider: %u %016llx %s\n", r, (unsigned long long)pv.versionId, pv.versionName ? pv.versionName : "?");
    bool ok = r == FFX_API_RETURN_OK && (!requireFsr4 || pv.versionId == fsr4);
    if (ok && dispatch) ok = upscaleSmoke(vk.device, gpu, family, &ctx, output);
    if (ffxDestroyContext(&ctx, nullptr) != FFX_API_RETURN_OK) ok = false;
    vk.cleanup();
    if (validate) { std::printf("validation errors: %u\n", validationErrors.load()); ok &= validationErrors == 0; }
    return ok ? 0 : 1;
}
