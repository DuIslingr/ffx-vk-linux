// ffx-vk-linux: AMD's FidelityFX / FSR SDK Vulkan DLLs in a native Linux program.
//
// Include this in place of AMD's ffx_api.h (it includes it, and the Vulkan and upscale headers),
// call ffxvkLoad once with the directory holding AMD's DLLs, then use AMD's API as on Windows:
// ffxCreateContext, ffxConfigure, ffxQuery, ffxDispatch, ffxDestroyContext - this library
// exports them. Function pointers in the descriptions (vkDeviceProcAddr, a message callback)
// are the program's own, Linux ones: the library bridges them.
#pragma once

#if !defined(_WIN32) && !defined(__declspec)
#define __declspec(x) __attribute__((visibility("default"))) // (AMD's headers mark their entry points dllexport)
#endif

#include <vulkan/vulkan.h>

#include "ffx_api.h"
#include "ffx_upscale.h"
#include "vk/ffx_api_vk.h"

#ifdef __cplusplus
extern "C" {
#endif

// loads amd_fidelityfx_loader_vk.dll (else amd_fidelityfx_upscaler_vk.dll) from `directory`:
// 0 done (or already), else nonzero - ffxvkError says why
__attribute__((visibility("default"))) int ffxvkLoad(const char* directory);
__attribute__((visibility("default"))) const char* ffxvkError(void);

#ifdef __cplusplus
}
#endif
