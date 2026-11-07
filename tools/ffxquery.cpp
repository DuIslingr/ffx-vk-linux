// ffxquery <dll directory>: a Vulkan device on the first GPU, AMD's DLL loaded through
// ffx-vk-linux, its upscaler versions listed, an upscale context created and its provider named.
#include "ffx_vk_linux.h"

#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

static void message(uint32_t type, const wchar_t* text) { std::fprintf(stderr, "[ffx %u] %ls\n", type, text); }

int main(int argc, char** argv) {
    if (argc < 2) return std::fprintf(stderr, "ffxquery <directory with amd_fidelityfx_*_vk.dll>\n"), 2;
    // Vulkan 1.3: an instance and a device with a compute queue
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "ffxquery";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance instance;
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) return std::fprintf(stderr, "no Vulkan instance\n"), 1;
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance, &n, nullptr);
    std::vector<VkPhysicalDevice> gpus(n);
    vkEnumeratePhysicalDevices(instance, &n, gpus.data());
    if (!n) return std::fprintf(stderr, "no GPU\n"), 1;
    VkPhysicalDevice gpu = gpus[0];
    VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &driver};
    vkGetPhysicalDeviceProperties2(gpu, &props);
    std::printf("GPU: %s (%s %s)\n", props.properties.deviceName, driver.driverName, driver.driverInfo);
    // every extension and feature the GPU has, on (FSR 4 picks what it needs)
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &ne, nullptr);
    std::vector<VkExtensionProperties> exts(ne);
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &ne, exts.data());
    std::vector<const char*> names;
    for (const auto& e : exts) {
        const std::string s = e.extensionName;
        if (s.find("_KHR_") != std::string::npos || s.find("_EXT_shader") != std::string::npos || s == "VK_AMD_shader_info")
            if (s.find("video") == std::string::npos && s.find("swapchain") == std::string::npos && s.find("present") == std::string::npos &&
                s.find("ray_tracing") == std::string::npos && s.find("acceleration") == std::string::npos && s.find("ray_query") == std::string::npos &&
                s.find("portability") == std::string::npos)
                names.push_back(e.extensionName);
    }
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &f12};
    VkPhysicalDeviceFeatures2 feats{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f11};
    vkGetPhysicalDeviceFeatures2(gpu, &feats);
    float prio = 1;
    VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    q.queueFamilyIndex = 0, q.queueCount = 1, q.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &feats};
    dci.queueCreateInfoCount = 1, dci.pQueueCreateInfos = &q;
    dci.enabledExtensionCount = uint32_t(names.size()), dci.ppEnabledExtensionNames = names.data();
    VkDevice device;
    if (VkResult r = vkCreateDevice(gpu, &dci, nullptr, &device); r != VK_SUCCESS) return std::fprintf(stderr, "no device (%d)\n", r), 1;

    if (ffxvkLoad(argv[1]) != 0) return std::fprintf(stderr, "ffxvkLoad: %s\n", ffxvkError()), 1;
    std::printf("AMD's DLL loaded\n");
    VkDeviceContext dc{device, gpu, vkGetDeviceProcAddr};

    uint64_t count = 0;
    ffxQueryDescGetVersions v{};
    v.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    v.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    v.device = &dc;
    v.outputCount = &count;
    ffxReturnCode_t r = ffxQuery(nullptr, &v.header);
    std::printf("versions query: %u, %llu versions\n", r, (unsigned long long)count);
    std::vector<uint64_t> ids(count);
    std::vector<const char*> vnames(count);
    v.versionIds = ids.data(), v.versionNames = vnames.data();
    r = ffxQuery(nullptr, &v.header);
    for (uint64_t i = 0; i < count; ++i) std::printf("  %016llx %s\n", (unsigned long long)ids[i], vnames[i] ? vnames[i] : "?");

    ffxCreateBackendVKDesc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    backend.deviceContext = dc;
    ffxCreateContextDescUpscale up{};
    up.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    up.header.pNext = &backend.header;
    up.maxRenderSize = {1280, 720};
    up.maxUpscaleSize = {2560, 1440};
    up.flags = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
    up.fpMessage = message;
    ffxContext ctx = nullptr;
    r = ffxCreateContext(&ctx, &up.header, nullptr);
    std::printf("upscale context: %u\n", r);
    if (r == FFX_API_RETURN_OK) {
        ffxQueryGetProviderVersion pv{};
        pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
        r = ffxQuery(&ctx, &pv.header);
        std::printf("provider: %u %016llx %s\n", r, (unsigned long long)pv.versionId, pv.versionName ? pv.versionName : "?");
        ffxDestroyContext(&ctx, nullptr);
    }
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    return 0;
}
