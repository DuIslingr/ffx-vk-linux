// The Windows functions AMD's DLLs import, stood in for on Linux
#pragma once

#include <string>

namespace ffxvk {

// an import: a stand-in, a Vulkan thunk, else a trap naming it
void* resolveImport(const std::string& dll, const std::string& name);
// a stand-in by DLL and name (null: none)
void* standIn(const std::string& dll, const std::string& name);
// where AMD's DLLs are; the system DLLs' pseudo-handles made
void setDllDirectory(const std::string& dir);
// LoadLibrary: one of AMD's from that directory (its base: its handle), mapped and started
void* loadDll(const std::string& name);
std::string fromUtf16(const char16_t* s);

} // namespace ffxvk
