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
- **Host-visible buffers:** the DLL's host-visible buffers (about 200 MB at 4K for
  FSR 4) land in system memory, because RADV lists the system-memory type before
  the CPU-visible VRAM one. With Resizable BAR (a CPU-visible VRAM heap of 4 GB or
  more) they go to VRAM with the same flags plus device-local.
  `FFXVK_HOST_VRAM=0` leaves them where they are.
- **Wave size:** RADV compiles the DLL's compute shaders as wave32, and FSR 4 asks
  for wave32 on 13 of its 30. A shader whose workgroup size is a multiple of 64 and
  that uses no subgroup operations, so its results can't depend on the wave size,
  gets `requiredSubgroupSize` 64, replacing an explicit wave32 request. This needs
  `subgroupSizeControl` on the application's device. `FFXVK_WAVE64=0` leaves wave
  sizes alone.

One more setting is up to the application, because Mesa reads it when the Vulkan
instance is created: **`radv_disable_dcc_stores=true`** (a RADV driconf option, also
accepted as an environment variable) keeps DCC compression off storage images. FSR 4's
compressed storage writes cost about 0.3 ms per frame at 4K (2.60 -> 2.28 ms). FSR 3
and the game's own rendering measured the same either way. Set it in the environment,
or with `setenv` before the program's `vkCreateInstance`. Other drivers ignore it.

The probe's output is byte-identical with and without these changes, for FSR 3 and
FSR 4 over three frames. Measured on an RX 7900 XTX (Mesa RADV 26.2.6), upscaling to
3840x2160 in Quality mode inside a game; GPU time from timestamps around the FSR
dispatch:

| GPU time per frame | FSR 3.1.6 | FSR 4.1.2 |
|---|---|---|
| Before | 1.15 ms | 4.3 ms |
| Cached memory types | | 3.2 ms |
| Cached memory and wave64 | 0.7 ms | 2.7 ms |
| All three | 0.7 ms | 2.6 ms |
| All three and `radv_disable_dcc_stores` | 0.7 ms | 2.28 ms |

Against the same SDK's D3D12 version under Proton (vkd3d-proton, same GPU and
driver), back to back in desktop windows: FSR's GPU cost is the GPU frame time
(Unity's FrameTimingManager) with FSR minus the GPU frame time with bilinear
upscaling from the same 2/3 render resolution, averaged over five views. The D3D12
window was 3840x1999, scaled to 3840x2160.

| GPU cost, 4K Quality | FSR 3 | FSR 4 |
|---|---|---|
| This library, all three changes, `radv_disable_dcc_stores` | 0.72 ms (3.1.6) | 2.30 ms (4.1.2) |
| D3D12 through vkd3d-proton | 0.74 ms (3.1.5) | 2.54 ms (4.1.1) |
| D3D12 through vkd3d-proton, `radv_disable_dcc_stores` | | 2.28 ms (4.1.1) |
| This library, all three changes, DCC stores on | 0.76 ms | 2.67 ms |

Running the DLLs as Windows code under Wine, without this library, cost the same as
the "before" rows: the changes, not the bridge, close the gap. vkd3d-proton makes the
first three choices on its own (it allocates only cached and Resizable BAR VRAM types
and runs those passes as wave64), and with `radv_disable_dcc_stores` it gains the same
~0.25 ms: the two paths are then level, so nothing else vkd3d-proton does differently
(images kept in `GENERAL`, fewer barriers, constant buffers without copies) is worth
measurable time. Proton players of D3D12 games with FSR 4 on RADV gain from the setting
too. The D3D12 FSR 4 is the SDK's 4.1.1, not the driver's 4.1.2. `FFXVK_DEBUG=1` logs each moved allocation
and each pipeline made wave64.

## Asynchronous compute

FSR's passes are all compute, so a program can run FSR on a compute-only queue (AMD's
API records into whatever command buffer it is given) while its graphics queue starts
on the next frame. The output is the same: the probe's `--compute-queue` gives
byte-identical images for FSR 3 and FSR 4. AMD's backend records one barrier per
context with graphics stages in it. The bridge turns those stages into
`ALL_COMMANDS`, which every queue accepts, so validation stays clean on both queues.

`build/ffxasync <DLL directory> [--fsr 3|4] [--load raster|compute]` measures what
this gains. Each frame has a load (a stand-in for the game's rendering), FSR at
3840x2160 Quality, and a light post pass. It runs every frame on the graphics queue,
then again with FSR on the compute queue, overlapped with the next frame's load. The
inputs are double-buffered, the images use `VK_SHARING_MODE_CONCURRENT`, and timeline
semaphores keep the order. Async adds one frame of latency.

RX 7900 XTX, RADV, GPU time per frame with a depth-only raster load (full-screen
passes: fill-bound, the shader cores mostly free, like shadow maps or a depth prepass):

| Load | FSR 3.1.6 serial | FSR 3.1.6 async | FSR 4.1.2 serial | FSR 4.1.2 async |
|---|---|---|---|---|
| ~0.8 ms | 1.88 ms | **1.34 ms** (+40%) | 2.92 ms | 3.06 ms (-5%) |
| ~3 ms | 3.98 ms | **3.47 ms** (+15%) | 3.77 ms | 3.98 ms (-5%) |
| ~12 ms | 12.56 ms | **12.08 ms** (+4%) | 13.52 ms | 13.70 ms (-1%) |

- **FSR 3:** async hides about 0.5 ms of its 0.77 ms. On the graphics queue, AMD's
  FSR 3 backend waits for the work before it.
- **FSR 4:** its passes already overlap fill-bound work on the graphics queue, so
  async gains nothing and adds about 0.15 ms of synchronisation.
- **Shader-bound load** (`--load compute`): neither version gains, since the load
  leaves no shader cores free.

So async compute is worth it for FSR 3 in programs whose frames have fill- or
geometry-bound work. For FSR 4 on this GPU, keep it on the graphics queue.
`--concurrency` checks that the two queues run at the same time at all on a given
GPU and driver.

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
