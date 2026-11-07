#pragma once
#include <string>
namespace ffxvk {
// Restricted, file-backed VERSION/Authenticode/Crypt32 compatibility for FSR's
// SDK-module checks. Signature verification checks integrity, not Windows policy.
void* securityStandIn(const std::string& dll, const std::string& name);
}
