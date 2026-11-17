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

// FSR 4's model for the contexts made from now on: "lite" FSR 4 Lite (AMD's smaller network, the one it gives its
// integrated GPUs: faster, its own picture), NULL or anything else the GPU's own. Contexts keep the model they were made
// with, so a program can change it while it runs: destroy the context and make it again. Starts as $FFXVK_FSR4_MODEL
// says (unset: the GPU's own). Call it with no other call into this library running.
// 0 done, else nonzero and nothing changed - ffxvkError says why (with the driver DLL, each model has its own instance
// of amdvkffx64.dll, the second one loaded when first asked for, and that load failed; without it, the SDK's
// amd_fidelityfx_upscaler_vk.dll holds both models and this cannot fail)
__attribute__((visibility("default"))) int ffxvkFsr4Model(const char* model);

#ifdef __cplusplus
}
#endif
