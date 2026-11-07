#pragma once

#include "ffx_vk_linux.h"

// The context must use maxRenderSize 128x72, maxUpscaleSize 256x144,
// render-resolution motion vectors, ordinary depth, and automatic exposure.
bool upscaleSmoke(VkDevice device, VkPhysicalDevice physicalDevice, uint32_t queueFamily,
                  ffxContext* context, const char* outputPath);
