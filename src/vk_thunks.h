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
// One per FSR 4 model (0 the GPU's own, 1 FSR 4 Lite): the driver DLL settles on its model the first time it is asked,
// so each model has its own instance of the DLL.
void setDriverProvider(int model, void* update);
// The model of the instance loaded first (-1: none). Its provider is FSR 4 as AMD's SDK lists it; the second
// instance's is given a version id of its own, which secondProviderId returns once the SDK has asked for it (0 before).
int firstDriverModel();
uint64_t secondProviderId();
// The model this thread is working for, for the length of a call into the DLLs: which instance the SDK's provider
// requests reach and which GPU family that instance is told (adapter.cpp).
void setModel(int model);
int model();

// The upscaler whose context this thread is making or dispatching (its major version, 0: none or unknown) and the
// program's physical device: FSR 4's images are made differently (vk_thunks.cpp, "linear tiling").
void setUpscaler(int major, VkPhysicalDevice device);

} // namespace ffxvk
