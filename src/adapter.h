// Windows driver adapter identity backed by Vulkan UUIDs and Linux DRM queries.
#pragma once
#include <string>
#include <vulkan/vulkan.h>
namespace ffxvk {
// liteOwnAdapter: FSR 4 Lite's questions (model() 1) get an adapter key of their own, for the SDK DLL's FSR 4, which
// keeps the GPU it identified per adapter key (with the driver DLL, Lite has an instance of that DLL to itself instead).
void identifyDriverAdapter(VkPhysicalDevice physical, PFN_vkGetPhysicalDeviceProperties2 query,
                          VkPhysicalDeviceProperties2* properties, bool liteOwnAdapter);
void* adapterStandIn(const std::string& dll, const std::string& name);
}
