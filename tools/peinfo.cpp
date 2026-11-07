// peinfo <file.dll>: a PE32+ file's sections, imports (by DLL) and exports - what a loader must
// map, resolve and offer. Reads only the format's own tables.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> file;

template <typename T> T at(size_t off) {
    T v{};
    if (off + sizeof(T) <= file.size()) std::memcpy(&v, file.data() + off, sizeof(T));
    return v;
}

struct Section {
    std::string name;
    uint32_t va, vsize, raw, rawSize, flags;
};
std::vector<Section> sections;

// a virtual address's offset in the file (0: none)
size_t rvaToOff(uint32_t rva) {
    for (const Section& s : sections)
        if (rva >= s.va && rva < s.va + std::max(s.vsize, s.rawSize)) return rva - s.va + s.raw;
    return 0;
}

std::string cstr(uint32_t rva) {
    std::string s;
    for (size_t o = rvaToOff(rva); o && o < file.size() && file[o]; ++o) s += char(file[o]);
    return s;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) return std::fprintf(stderr, "peinfo <file.dll>\n"), 2;
    std::ifstream in(argv[1], std::ios::binary);
    file.assign(std::istreambuf_iterator<char>(in), {});
    if (at<uint16_t>(0) != 0x5a4d) return std::fprintf(stderr, "not an MZ file\n"), 1;
    const uint32_t pe = at<uint32_t>(0x3c);
    if (at<uint32_t>(pe) != 0x4550) return std::fprintf(stderr, "no PE header\n"), 1;
    const uint16_t machine = at<uint16_t>(pe + 4), nsec = at<uint16_t>(pe + 6), optSize = at<uint16_t>(pe + 20);
    const size_t opt = pe + 24;
    if (at<uint16_t>(opt) != 0x20b) return std::fprintf(stderr, "not PE32+\n"), 1;
    const uint64_t base = at<uint64_t>(opt + 24);
    const uint32_t entry = at<uint32_t>(opt + 16), imageSize = at<uint32_t>(opt + 56), ndirs = at<uint32_t>(opt + 108);
    std::printf("machine %04x, image base %016llx, size %u, entry %08x, %u directories\n", machine, (unsigned long long)base, imageSize,
                entry, ndirs);
    static const char* const kDirs[16] = {"export", "import", "resource", "exception", "security", "basereloc", "debug", "arch",
                                          "globalptr", "tls", "loadconfig", "boundimport", "iat", "delayimport", "clr", "-"};
    for (uint32_t d = 0; d < ndirs && d < 16; ++d) {
        const uint32_t rva = at<uint32_t>(opt + 112 + d * 8), size = at<uint32_t>(opt + 116 + d * 8);
        if (size) std::printf("  dir %-11s %08x +%u\n", kDirs[d], rva, size);
    }
    for (uint16_t i = 0; i < nsec; ++i) {
        const size_t s = opt + optSize + i * 40;
        char name[9] = {};
        std::memcpy(name, file.data() + s, 8);
        sections.push_back({name, at<uint32_t>(s + 12), at<uint32_t>(s + 8), at<uint32_t>(s + 20), at<uint32_t>(s + 16), at<uint32_t>(s + 36)});
        std::printf("section %-8s va %08x vsize %8u raw %08x size %8u flags %08x\n", name, sections.back().va, sections.back().vsize,
                    sections.back().raw, sections.back().rawSize, sections.back().flags);
    }
    // imports: descriptors of 20 bytes, each a DLL's name and its lookup table (by name or ordinal)
    auto imports = [&](uint32_t dirRva, bool delay) {
        for (size_t d = rvaToOff(dirRva); d; d += delay ? 32 : 20) {
            const uint32_t nameRva = at<uint32_t>(d + (delay ? 4 : 12));
            const uint32_t ilt = delay ? at<uint32_t>(d + 16) : (at<uint32_t>(d) ? at<uint32_t>(d) : at<uint32_t>(d + 16));
            if (!nameRva) break;
            std::printf("%simport %s:", delay ? "delay " : "", cstr(nameRva).c_str());
            int n = 0;
            for (size_t t = rvaToOff(ilt); t; t += 8) {
                const uint64_t e = at<uint64_t>(t);
                if (!e) break;
                if (e >> 63) std::printf(" #%u", unsigned(e & 0xffff));
                else std::printf(" %s", cstr(uint32_t(e) + 2).c_str());
                ++n;
            }
            std::printf("  (%d)\n", n);
        }
    };
    imports(at<uint32_t>(opt + 112 + 1 * 8), false);
    if (ndirs > 13) imports(at<uint32_t>(opt + 112 + 13 * 8), true);
    // exports
    if (const uint32_t er = at<uint32_t>(opt + 112); er) {
        const size_t e = rvaToOff(er);
        const uint32_t nnames = at<uint32_t>(e + 24), names = at<uint32_t>(e + 32), ords = at<uint32_t>(e + 36), funcs = at<uint32_t>(e + 28);
        std::printf("exports of %s (%u named):\n", cstr(at<uint32_t>(e + 12)).c_str(), nnames);
        for (uint32_t i = 0; i < nnames; ++i) {
            const uint16_t ord = at<uint16_t>(rvaToOff(ords) + i * 2);
            std::printf("  %-40s %08x\n", cstr(at<uint32_t>(rvaToOff(names) + i * 4)).c_str(), at<uint32_t>(rvaToOff(funcs) + ord * 4));
        }
    }
    return 0;
}
