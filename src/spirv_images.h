// MIT: adapt nonstandard half-float image interfaces to Vulkan's float32 image interface.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ffxvk {

enum class ImageRewriteStatus { Unchanged, Rewritten, Invalid };

struct ImageRewrite {
    ImageRewriteStatus status = ImageRewriteStatus::Unchanged;
    // Populated only when rewritten; the caller retains its original module otherwise.
    std::vector<uint32_t> words;
    std::string error;
};

// Converts image sampled types from float16 to float32, inserting conversions at image IO
// boundaries so the shader's half-float arithmetic and storage formats remain unchanged.
// Invalid instruction streams, sparse half-image IO and half-image texel pointers fail
// explicitly. This parses instruction syntax; it does not replace a full shader validator.
// driverCompatibility additionally repairs the driver provider's short storage-image
// writes and its recurrent image's mismatched format declaration. That path requires
// shaderStorageImageReadWithoutFormat and shaderStorageImageWriteWithoutFormat enabled.
ImageRewrite rewriteHalfImageTypes(std::span<const uint32_t> code, bool driverCompatibility = false);

} // namespace ffxvk
