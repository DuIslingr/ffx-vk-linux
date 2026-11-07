# FSR 4 on Linux: what was tried, what was found

November 2026. Hardware: AMD Radeon RX 7900 XTX (RDNA 3), Mesa RADV 26.2.6.

## Goal

Upscale with AMD FSR 4.1.2 from native Linux Vulkan programs - first the Chaos Rings ports
(CR3-U, CR2-U, CR1:U, CRO-U), which use FSR 3.1 today.

## What AMD ships

- **FSR SDK 2.4** (v2.4.0, 28 October 2026, github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK)
  added Vulkan support for FSR Upscaling 4.1.2 - as **signed Windows DLLs only**:
  `amd_fidelityfx_loader_vk.dll` and `amd_fidelityfx_upscaler_vk.dll` (described as holding FSR
  4.1.2, 3.1.6 and 2.3.4). Windows 10 and 11 are the only systems listed.
- The DLLs' licence (`Kits/FidelityFX/docs/license.md`) allows redistribution "in binary form
  only" and forbids reverse engineering, decompilation and disassembly. They can't ship in GPL
  buil