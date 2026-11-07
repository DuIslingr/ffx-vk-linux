// Vulkan between a Windows DLL and Linux: each command wrapped with the Microsoft x64 calling
// convention, forwarding to the real (Linux) one.
#pragma once

#include <vulkan/vulkan.h>

#include <string>

namespace ffxvk {

// an import from vulkan-1.dll: its wrapper, forwarding to libvulkan's own (null: unknown)
void* vulkanImport(const std::string& name);
// a vkGetDeviceProcAddr for the DLL (ms_abi): the wrapper of what the real one gives
PFN_vkVoidFunction msGetDeviceProcAddrFor(PFN_vkGetDeviceProcAddr real);
// a vkGetInstanceProcAddr the same way
PFN_vkVoidFunction msGetInstanceProcAddrFor(PFN_vkGetInstanceProcAddr real);

// Optional driver provider: UpdateFfxApiProvider, with the Microsoft x64 ABI.
// The SDK's private Vulkan entry additionally takes the VkDevice as its first argument.
void setDriverProvider(void* update);

} // namespace ffxvk
