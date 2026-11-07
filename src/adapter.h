// Windows driver adapter identity backed by Vulkan UUIDs and Linux DRM queries.
#pragma once
#include <string>
#include <vulkan/vulkan.h>
namespace ffxvk {
void identifyDriverAdapter(VkPhysicalDevice physical, PFN_vkGetPhysicalDeviceProperties2 query,
                          VkPhysicalDeviceProperties2* properties);
void* adapterStandIn(const std::string& dll, const std::string& name);
}
