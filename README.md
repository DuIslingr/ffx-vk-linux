# ffx-vk-linux

Run AMD's Windows FidelityFX / FSR Vulkan DLLs in a **native x86-64 Linux Vulkan
program**, without Wine. The library provides a small PE loader, Windows runtime
services, calling-convention bridges, and AMD's public FSR API.

**Experimental FSR 4.1.2 support:** the SDK's Vulkan provider request is bridged to
`amdvkffx64.dll` from AMD's Windows driver. Tested with SDK 2.4.0, Adrenalin 26.10.1,
an RX 7900 XTX and Mesa RADV 26.2.6. The probe checks the selected provider and can
submit three upscale frames and verify their pixel readback. This is an initial
headless correctness test; game integration and temporal image quality need further
validation. See [the driver compatibility notes](docs/driver-provider.md).

Without the optional driver DLL, the SDK supplies its FSR 3.1.6 / 2.3.4 providers.
Unsupported hardware can also fall back to those providers. Use `--require-fsr4`
to make the probe reject that fallback.

## Build

Dependencies: CMake 3.20+, a C++20 x86-64 compiler supporting `ms_abi`, Python 3,
Vulkan headers/loader and `vk.xml`, OpenSSL, libdrm with libdrm_amdgpu, SPIRV-Tools
and SPIR-V headers. `spirv-as` and `spirv-val` enable the shader regression tests.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Run

Supply your own AMD DLLs; they are excluded from version control:

- SDK 2.4.0: `amd_fidelityfx_loader_vk.dll` and `amd_fidelityfx_upscaler_vk.dll`
  from `Kits/FidelityFX/signedbin`.
- For FSR 4: `amdvkffx64.dll` from the Adrenalin Windows driver. Put it beside
  the SDK DLLs, or set `FFXVK_DRIVER_DLL` to its absolute path.

```sh
FFXVK_DRIVER_DLL=/path/to/amdvkffx64.dll \
  build/ffxquery /path/to/sdk-dlls --require-fsr4 --dispatch --validate \
  --output /tmp/fsr4.ppm
```

`--validate` requires `VK_LAYER_KHRONOS_validation`. GPU access is necessary;
a sandbox exposing only llvmpipe cannot exercise the AMD provider. `FFXVK_DEBUG=1`
logs imports and provider requests. `FFXVK_SHADER_DUMP=/existing/directory` writes
the SPIR-V sent to Vulkan for local inspection; those files contain AMD shader code.
`build/security_checks /path/to/dll-directory` additionally verifies the real DLLs,
the driver's pinned signing key and rejection of a modified copy.

## Integrate

Include `ffx_vk_linux.h`, link `ffx_vk_linux`, call `ffxvkLoad(directory)` once,
then use AMD's `ffxCreateContext`, `ffxQuery`, `ffxConfigure`, `ffxDispatch` and
`ffxDestroyContext`. `FFXVK_DLL_DIR` can supply the directory for lazy loading.
Function pointers in the API descriptions are native Linux pointers; the bridge
handles their calling conventions. Pass the SDK 2.4 upscale-version description
in the create chain, as shown in `tools/ffxquery.cpp`.

The probe demonstrates Vulkan device features/extensions needed by this driver
path, including float16/int8 arithmetic, integer dot product, storage images
without format, compute shader derivatives, and AMD coherent memory where
advertised. Applications must enable the features their provider uses on the
logical device; the bridge cannot change an already-created device.

## Performance on RADV

Two choices of AMD's Vulkan code cost a lot on RADV, and the bridge changes both in
the Vulkan functions it gives the DLL:

- **Memory types:** the DLL allocates its buffers (about 300 MB per 4K context)
  from RADV's "device coherent, device uncached" types (`VK_AMD_device_coherent_memory`;
  RADV lists them whether or not the device enabled the feature), which the GPU's
  caches skip. The bridge allocates them from the cached type with the same other
  properties instead. `FFXVK_UNCACHED=1` keeps the DLL's choice.
- **Wave size:** RADV compiles the DLL's compute shaders as wave32, and FSR 4 asks
  for wave32 on 13 of its 30. A shader whose workgroup size is a multiple of 64 and
  that uses no subgroup operations, so its results can't depend on the wave size,
  gets `requiredSubgroupSize` 64, replacing an explicit wave32 request. This needs
  `subgroupSizeControl` on the application's device. `FFXVK_WAVE64=0` leaves wave
  sizes alone.

The probe's output is byte-identical with and without both changes, for FSR 3 and
FSR 4 over three frames. Measured on an RX 7900 XTX (Mesa RADV 26.2.6), upscaling to
3840x2160 in Quality mode inside a game:

| GPU time per frame | FSR 3.1.6 | FSR 4.1.2 |
|---|---|---|
| Before | 1.15 ms | 4.3 ms |
| Cached memory types | | 3.2 ms |
| Cached memory and wave64 | 0.7 ms | 2.7 ms |
| D3D12 version through vkd3d-proton, same GPU (estimated from frame times) | ~0.4 ms | ~2.2 ms |

Running the DLLs as Windows code under Wine, without this library, gave the same
timings as the "Before" row, so the remaining gap is in the Vulkan path itself.
`FFXVK_DEBUG=1` logs each moved allocation and each pipeline made wave64.

The current Vulkan thunk table and message/allocation callback bridges assume one
application device and one callback set. Concurrent independent devices/contexts,
Vulkan allocation callbacks supplied by the DLL, Windows exception unwinding,
frame generation and window-system integration are not supported by this work.

## Licence

The compatibility implementation is MIT (`LICENSE`). AMD's vendored API headers
are MIT. AMD DLLs retain their own licence terms and are supplied by the user.
No AMD binaries, models or shader dumps are included. The certificate adapter
verifies PE integrity and the signer signature and supplies the actual signer key;
Windows certificate-store, timestamp and revocation policy are outside its scope.
Not affiliated with or endorsed by AMD.
