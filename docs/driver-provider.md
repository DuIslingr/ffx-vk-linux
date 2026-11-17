# Native Linux FSR 4 driver provider

## Goal and current scope

Enable native Linux Vulkan clients (initially the Chaos Rings ports) to use AMD's
FSR 4.1.2 implementation through its existing public SDK API. The earlier loader
could create an FSR 3 context but did not implement `vkUpdateFfxApiProviderAMD`.
This change adds that compatibility path and a headless GPU dispatch/readback probe.
It does not modify the ports.

## Findings and implementation

The SDK calls the private Vulkan entry with the Microsoft x64 ABI as
`(VkDevice, void* request, uint32_t bytes) -> int32_t status`. Observed request
sizes are 0x60 (version 3) and 0x50 (older versions). The bridge forwards the
request to `amdvkffx64.dll!UpdateFfxApiProvider(request, bytes)`, which uses the same
ABI without the device argument. The SDK still enumerates providers, runs their
support checks and routes contexts. Provider function pointers stay between
Windows-compiled modules, so their ABI does not need an intermediate conversion.

The driver validates the SDK's file version and signature, then compares its
signer's complete RSA key to a built-in AMD key. The compatibility layer reads
actual PE version resources, verifies the Authenticode PE digest and PKCS7 signer
signature with OpenSSL, and returns the actual certificate issuer, serial and CNG
RSA public-key blob. It leaves the driver's key comparison intact. A changed
executable byte fails verification. This is deliberately narrower than Windows
WinTrust policy: no Windows trust-store, revocation or timestamp equivalence is
claimed. Unsupported certificate/PE layouts fail verification.

Adapter support also needs Windows OS metadata. The DLL requests
`VkPhysicalDeviceIDProperties` and refuses the older GPU paths without a valid
LUID. The DLL-facing wrapper derives a stable 64-bit adapter key from the Vulkan
device UUID. Native application queries are unchanged. Its subsequent
`D3DKMTEnumAdapters` and AMD legacy `D3DKMTQueryAdapterInfo` request are backed by
the physical device's actual DRM render node and `amdgpu_query_gpu_info` family
and external-revision values. On the tested RX 7900 XTX these are family 0x91,
revision 1. This enables the matching RDNA 3 path without changing reported shader
capabilities or pretending that the GPU is a different model.

The shader wrapper promotes nonstandard float16 sampled-image types to float32
and inserts conversions around image IO so half arithmetic remains half. It uses
SPIRV-Tools' operand parser to remap IDs without changing numeric literals.
Malformed modules and unsupported sparse/texel-pointer half-image operations fail
explicitly. Further Vulkan image fixes, first made for the driver provider, apply to
every module (see "The SDK's own FSR 4" below).

Windows thread, event and recursive critical-section services use real pthread
and Linux synchronization facilities. Unsupported calls retain named traps, so
an unimplemented Windows operation is visible instead of silently succeeding.

## Reproduce

Build and run the commands in the repository README. The probe requires Vulkan
1.3. It creates a compute-capable queue, enables supported core shader features,
and enables compute shader derivatives and AMD coherent memory when advertised.
`--require-fsr4` pins the enumerated FSR 4 provider and returns an error if it is
missing. `--dispatch` uploads a known color pattern, constant depth and zero motion,
upscales 128x72 to 256x144 for three frames, and checks finite/nonconstant output,
spatial gradient error and left/right ordering. `--output` writes a PPM readback.
The test is evidence of GPU execution and basic correctness, not a temporal
quality, performance or production-game compatibility benchmark.

The SDK-only path can be checked by putting just the two SDK DLLs in a separate
directory and leaving `FFXVK_DRIVER_DLL` unset. Supplying a nonexistent explicit
driver path is a load failure, not an unnoticed fallback.

## Evidence and provenance

Driver artifact: Adrenalin 26.10.1 `amdvkffx64.dll`, file version 2.4.0.4370,
SHA-256 `80e761f7c8c0ec8ead7b321df01911baaf917defae51388d29eaecb1d6bc58bd`.
SDK files: loader version 1.0.2.4555 and upscaler version 4.1.2.4555.

Static investigation used REA/Ghidra 12.1.2. Pseudocode is decompiler output, not
original source; indirect-call coverage is limited. Relevant retained evidence:

- Driver provider export: `ev_fe91c6c1c8c10034cb7d4bd8ec748fec25bc393074d166c21502c75642ab0167`.
- Version/signature entry: `ev_148849d5c6565f4f55e86ca7176fdaf96bd821f43cdf85d70da9f05cf6da788f`.
- Signature and RSA pin check: `ev_876b7c66d1c6439319866aba58b688ef91f8e9686a028358c3fdd571e78066d1`.
- UUID/LUID and ASIC detection: `ev_ee2d2f58fd93af53cf3558540d9a8a29660d1577c2fa3e8eb7753d56d89c437e`.
- Legacy graphics adapter query: `ev_de9411d36e88cf70cb40d402f5be528681348f6517c344f8bf4904726ce76301`.

A runtime GDB observation of the SDK's call established the three hook arguments;
provider negotiation, GPU creation and pixel readback are separate runtime checks.
OptiScaler's [provider interface notes](https://github.com/optiscaler/OptiScaler/blob/master/OptiScaler/fsr4/FSR4DriverProvider.h)
helped locate the provider boundary. No OptiScaler implementation is copied into
this MIT code. The [SDK API](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK)
and the installed Vulkan/SPIR-V headers define the public interfaces.

## Verified result (7 November 2026)

On RX 7900 XTX / RADV 26.2.6, the SDK lists FSR 4.1.2 and reports the selected
provider as `f5a5ca1e01001002` / `4.1.2`. Three upscale frames completed with output
RGB range 0.099609–0.798828 and spatial gradient mean absolute error 0.001727.
The final probe completed with **zero Vulkan validation errors**, including
context and Vulkan object teardown.

Two extra image fixes were needed after observing actual dispatches: float3
storage writes are expanded to four components with alpha 1, and the recurrent
state image named `rw_recurrent_0` uses a formatless storage-image declaration
instead of the driver's Rgba32f declaration, allowing the actual UNORM view's
conversion. The latter adds storage-image read/write-without-format capabilities;
applications must enable the corresponding Vulkan features. These fixes were
limited to driver mode until 17 November 2026 (see below). All 30 shader modules captured during the probe also
passed standalone Vulkan SPIR-V validation after normalization.

Known limits remain: one device/callback set, the observed driver's legacy
adapter-query layout, no frame generation, and no temporal-quality or game-scale
performance testing. New driver releases may change private interfaces. Signature
support rejects nonstandard PE section layouts it does not understand rather
than treating them as verified.

Debug and optimized Release builds both passed the three CTest suites (runtime,
security, shader normalization). Real signed-DLL tests passed, including rejection
of a modified executable byte. The SDK-only FSR 3.1.6 path also completed the
three-frame readback; its Windows shaders emit typed-storage-format validation
warnings, which this FSR 4 change does not normalize. Missing explicit driver
paths and `--require-fsr4` without an available FSR 4 provider correctly exit 1.

## The SDK's own FSR 4 (17 November 2026)

SDK 2.4.0's `amd_fidelityfx_upscaler_vk.dll` (SHA-256
`4a7e895af6e0b615d40a84d59eb8c2b1054ad91cdd0c739567940270d51bdab7`) carries FSR 4
itself (read from its disassembly with llvm-mingw's objdump; GDB showed the bridge's
adapter identity was never given without the driver DLL). Its static initializer registers six providers: four FSR 4 ones under the
driver's version id `f5a5ca1e01001002` (vtables `0x1821acf70` FP8, `0x1821ace40`
INT8 `FSR4-i8`, `0x1821acec8` RDNA 2 INT8 `FSR4-i8-2`, `0x1821acda0` Lite `FSR4L-i8`),
then FSR 3.1.6 and 2.3.4. Each FSR 4 provider's support check (vtable slot 2) calls
one GPU identification routine (`0x18000eb40`). It reads
`VkPhysicalDeviceIDProperties` and returns false without a valid LUID, caches its
answer per LUID, and for vendor 0x1002 loads `gdi32.dll` and asks
`D3DKMTEnumAdapters` / `D3DKMTQueryAdapterInfo` (type 0, 0x260 bytes) for the family
at offset 48 and the external revision at offset 52, as the driver DLL does. The
checks then take family 0x91 (any revision 1-0xFE), 0x8F with revision 0x28-0x4F
(RDNA 2), or 0x96 (Lite for revisions 1-0x3F, the INT8 network for 0xC0-0xCF).
When the driver's `vkUpdateFfxApiProviderAMD` is available, the SDK asks the driver's
provider for a version before its own table.

The bridge already answered those questions, but only while the driver DLL was
loaded: without it the DLL-facing LUID stayed invalid and the SDK listed only FSR 3.
The adapter identity is now given whenever the DLL asks, and the image fixes above
apply to every module (FSR 3's 11 modules are unchanged by them). For FSR 4 Lite
without the driver DLL, Lite's thread model gets a second LUID derived from the same
UUID, so the DLL's per-LUID cache identifies the GPU again and receives the
integrated family; one SDK DLL instance serves both models.

Checked on RX 7900 XTX / RADV 26.2.6 with only the two SDK DLLs: `--require-fsr4
--dispatch --validate` lists 4.1.2 and completes with zero validation errors, and
its readback is byte-identical to the driver DLL's (full: SHA-256 prefix
`3b962038eca7`; Lite: `486c03cac9c1`, also identical between the two DLLs). Contexts
of both models live together, and each made again after the other's destruction,
give the same images with and without the driver DLL. `ffxasync --fsr 4`, 3840x2160
Quality with `radv_disable_dcc_stores`, lightest load, serial: 2.576 ms (SDK) and
2.572 ms (driver) for the full model, 1.657 ms and 1.653 ms for Lite. The FP8
provider's check and RDNA 2 and integrated GPUs were not run.
