// Additional Windows synchronization and thread services for the native PE host.
#pragma once

#include <string>

namespace ffxvk {

// Returns a Microsoft x64 ABI entry point, or nullptr for an unknown import.
void* runtimeStandIn(const std::string& dll, const std::string& name);

} // namespace ffxvk
