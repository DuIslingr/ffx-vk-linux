// A Windows x86-64 DLL (PE32+) mapped into a Linux process: sections placed, relocated, imports
// resolved through a callback, its TLS template kept, its entry point and TLS callbacks run.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ffxvk {

struct PeImage {
    std::string path;      // as loaded (a Linux path)
    std::string name;      // its file name, lower case ("amd_fidelityfx_upscaler_vk.dll")
    uint8_t* base = nullptr;
    size_t size = 0;
    uint32_t entry = 0;    // DllMain's RVA (0: none)
    // TLS (IMAGE_TLS_DIRECTORY64): the template, its zero fill, where the index goes, callbacks
    int tlsIndex = -1;
    std::vector<uint8_t> tlsTemplate;
    uint32_t tlsZeroFill = 0;
    std::vector<uint64_t> tlsCallbacks;
    void* exportOf(const std::string& name) const;
    bool contains(const void* p) const { return p >= base && p < base + size; }
};

// what an import resolves to: (dll name lower case, function name or "#ordinal") -> an address
// (a function with the Microsoft x64 calling convention, or a data object); null: unresolved
using ImportResolver = std::function<void*(const std::string& dll, const std::string& name)>;

// maps `path`; `error` says why not. Runs nothing - see runInit
PeImage* loadPe(const std::string& path, const ImportResolver& resolve, std::string* error);
// TLS callbacks and DllMain with DLL_PROCESS_ATTACH (the thread must have its TEB: teb.h)
bool runInit(PeImage& img, std::string* error);

std::vector<PeImage*>& loadedImages();

} // namespace ffxvk
