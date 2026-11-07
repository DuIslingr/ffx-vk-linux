# ffx-vk-linux

AMD's FidelityFX / FSR SDK 2.4 Vulkan DLLs (`amd_fidelityfx_loader_vk.dll`,
`amd_fidelityfx_upscaler_vk.dll` - FSR Upscaling 4.1.2, with FSR 3.1.6 inside) loaded into a
**native Linux** Vulkan program, without Wine.

AMD ships FSR 4 for Vulkan only as signed Windows DLLs. FSR's Vulkan API takes the program's
own Vulkan function pointers, so a DLL needs no Windows Vulkan loader - only a handful of
Windows functions and a bridge between the Windows and System V calling conventions. This
library is that: a small PE loader that maps the DLL, resolves its imports to Linux
stand-ins, thunks every function pointer crossing the boundary, and exposes AMD's FSR API
(`ffxCreateContext`, `ffxDispatch`, ... - `third_party/ffx/include`) to Linux code.

**Status: in progress.** Not usable yet.

## What you need

- AMD's two DLLs from the FSR SDK 2.4 (`Kits/FidelityFX/signedbin` in AMD's repository),
  supplied by you: they are not part of this repository and never will be (AMD's licence
  allows them to be redistributed in binary form only, under its terms).
- An x86-64 Linux system with a Vulkan driver that has what FSR 4 needs (Mesa RADV on AMD
  RDNA 2 and later is the target). Elsewhere the SDK selects FSR 3.1.6.

## Licence

MIT (`LICENSE`). AMD's API headers in `third_party/ffx` are MIT too. Not affiliated with or
endorsed by AMD. AMD's DLL licence forbids reverse engineering them: this loader only reads
the PE format's own tables (sections, relocations, imports, exports) to load the files, as
any loader does.
