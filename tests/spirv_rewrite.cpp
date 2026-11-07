// Small test driver: read a module, normalize its image interface, write the result.
#include "spirv_images.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <cstring>

int main(int argc, char** argv) {
    if (argc != 3 && argc != 4) return 2;
    std::ifstream input(argv[1], std::ios::binary);
    if (!input) return 2;
    const std::vector<char> bytes{std::istreambuf_iterator<char>(input), {}};
    if (bytes.size() % sizeof(uint32_t)) return 2;
    std::vector<uint32_t> words(bytes.size() / sizeof(uint32_t));
    std::memcpy(words.data(), bytes.data(), bytes.size());
    auto rewritten = ffxvk::rewriteHalfImageTypes(words, argc == 4 && std::strcmp(argv[3], "--driver") == 0);
    if (rewritten.status == ffxvk::ImageRewriteStatus::Invalid) {
        std::fprintf(stderr, "%s\n", rewritten.error.c_str());
        return 1;
    }
    if (rewritten.status == ffxvk::ImageRewriteStatus::Rewritten) words = std::move(rewritten.words);
    std::ofstream output(argv[2], std::ios::binary);
    output.write(reinterpret_cast<const char*>(words.data()), words.size() * sizeof(uint32_t));
    return output ? 0 : 2;
}
