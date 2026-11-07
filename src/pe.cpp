#include "pe.h"

#include "teb.h"

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

namespace ffxvk {

namespace {

template <typename T> T rd(const std::vector<uint8_t>& f, size_t off) {
    T v{};
    if (off + sizeof(T) <= f.size()) std::memcpy(&v, f.data() + off, sizeof(T));
    return v;
}

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

constexpr uint32_t kScnExecute = 0x20000000, kScnRead = 0x40000000, kScnWrite = 0x80000000;

} // namespace

std::vector<PeImage*>& loadedImages() {
    static std::vector<PeImage*> images;
    return images;
}

void* PeImage::exportOf(const std::string& want) const {
    const uint32_t* dirs = reinterpret_cast<const uint32_t*>(base + *reinterpret_cast<const uint32_t*>(base + 0x3c) + 24 + 112);
    const uint32_t er = dirs[0], esize = dirs[1];
    if (!er) return nullptr;
    const uint8_t* e = base + er;
    const uint32_t nnames = *reinterpret_cast<const uint32_t*>(e + 24), nfuncs = *reinterpret_cast<const uint32_t*>(e + 20);
    const uint32_t* funcs = reinterpret_cast<const uint32_t*>(base + *reinterpret_cast<const uint32_t*>(e + 28));
    const uint32_t* names = reinterpret_cast<const uint32_t*>(base + *reinterpret_cast<const uint32_t*>(e + 32));
    const uint16_t* ords = reinterpret_cast<const uint16_t*>(base + *reinterpret_cast<const uint32_t*>(e + 36));
    uint32_t rva = 0;
    if (!want.empty() && want[0] == '#') {
        const uint32_t ord = uint32_t(std::stoul(want.substr(1))) - *reinterpret_cast<const uint32_t*>(e + 16);
        if (ord < nfuncs) rva = funcs[ord];
    } else {
        for (uint32_t i = 0; i < nnames && !rva; ++i)
            if (want == reinterpret_cast<const char*>(base + names[i])) rva = funcs[ords[i]];
    }
    if (!rva || (rva >= er && rva < er + esize)) return nullptr; // (a forwarder: not supported)
    return base + rva;
}

PeImage* loadPe(const std::string& path, const ImportResolver& resolve, std::string* error) {
    auto fail = [&](const std::string& why) -> PeImage* {
        if (error) *error = path + ": " + why;
        return nullptr;
    };
    std::ifstream in(path, std::ios::binary);
    if (!in) return fail("can't be opened");
    const std::vector<uint8_t> f((std::istreambuf_iterator<char>(in)), {});
    if (rd<uint16_t>(f, 0) != 0x5a4d) return fail("not a Windows executable (no MZ)");
    const uint32_t pe = rd<uint32_t>(f, 0x3c);
    if (rd<uint32_t>(f, pe) != 0x4550 || rd<uint16_t>(f, pe + 4) != 0x8664) return fail("not an x86-64 PE file");
    const uint16_t nsec = rd<uint16_t>(f, pe + 6), optSize = rd<uint16_t>(f, pe + 20);
    const size_t opt = pe + 24;
    if (rd<uint16_t>(f, opt) != 0x20b) return fail("not PE32+");
    const uint64_t prefBase = rd<uint64_t>(f, opt + 24);
    const uint32_t imageSize = rd<uint32_t>(f, opt + 56), headerSize = rd<uint32_t>(f, opt + 60);
    auto dir = [&](int d, uint32_t& rva, uint32_t& size) { rva = rd<uint32_t>(f, opt + 112 + d * 8), size = rd<uint32_t>(f, opt + 116 + d * 8); };

    // its preferred base if free (no relocating), else anywhere
    void* at = mmap(reinterpret_cast<void*>(prefBase), imageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (at == MAP_FAILED) at = mmap(nullptr, imageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (at == MAP_FAILED) return fail("no memory for its image");
    auto* img = new PeImage;
    img->path = path;
    img->name = lower(path.substr(path.find_last_of('/') + 1));
    img->base = static_cast<uint8_t*>(at);
    img->size = imageSize;
    img->entry = rd<uint32_t>(f, opt + 16);
    std::memcpy(img->base, f.data(), std::min<size_t>(headerSize, f.size()));
    struct Sec {
        uint32_t va, vsize, flags;
    };
    std::vector<Sec> secs;
    for (uint16_t i = 0; i < nsec; ++i) {
        const size_t s = opt + optSize + i * 40;
        const uint32_t vsize = rd<uint32_t>(f, s + 8), va = rd<uint32_t>(f, s + 12), rawSize = rd<uint32_t>(f, s + 16), raw = rd<uint32_t>(f, s + 20);
        if (raw && rawSize) std::memcpy(img->base + va, f.data() + raw, std::min<size_t>({rawSize, vsize ? vsize : rawSize, f.size() - raw}));
        secs.push_back({va, std::max(vsize, rawSize), rd<uint32_t>(f, s + 36)});
    }

    // base relocations (IMAGE_REL_BASED_DIR64 only on x86-64)
    const int64_t delta = int64_t(reinterpret_cast<uint64_t>(img->base) - prefBase);
    uint32_t rr, rs;
    dir(5, rr, rs);
    if (delta && rr) {
        for (uint32_t o = 0; o + 8 <= rs;) {
            const uint32_t page = *reinterpret_cast<uint32_t*>(img->base + rr + o), block = *reinterpret_cast<uint32_t*>(img->base + rr + o + 4);
            if (block < 8) break;
            for (uint32_t k = 8; k + 2 <= block; k += 2) {
                const uint16_t e = *reinterpret_cast<uint16_t*>(img->base + rr + o + k);
                if ((e >> 12) == 10) *reinterpret_cast<uint64_t*>(img->base + page + (e & 0xfff)) += uint64_t(delta);
                else if ((e >> 12) != 0) return fail("a relocation of type " + std::to_string(e >> 12));
            }
            o += block;
        }
    } else if (delta && !rr) {
        return fail("can't be placed at its base and has no relocations");
    }

    // imports: each DLL's lookup table resolved into its address table
    uint32_t ir, is;
    dir(1, ir, is);
    for (uint32_t d = ir; ir; d += 20) {
        const uint32_t ilt = *reinterpret_cast<uint32_t*>(img->base + d), nameRva = *reinterpret_cast<uint32_t*>(img->base + d + 12),
                       iat = *reinterpret_cast<uint32_t*>(img->base + d + 16);
        if (!nameRva) break;
        const std::string dll = lower(reinterpret_cast<const char*>(img->base + nameRva));
        for (uint32_t t = 0;; t += 8) {
            const uint64_t e = *reinterpret_cast<uint64_t*>(img->base + (ilt ? ilt : iat) + t);
            if (!e) break;
            const std::string name = (e >> 63) ? "#" + std::to_string(e & 0xffff) : std::string(reinterpret_cast<const char*>(img->base + uint32_t(e) + 2));
            void* p = resolve(dll, name);
            if (!p) return fail("its import " + dll + "!" + name + " has nothing to resolve to");
            *reinterpret_cast<void**>(img->base + iat + t) = p;
        }
    }

    // TLS: the template and callbacks (addresses relocated by now)
    uint32_t tr, ts;
    dir(9, tr, ts);
    if (tr) {
        const uint64_t* t = reinterpret_cast<const uint64_t*>(img->base + tr);
        const uint64_t start = t[0], end = t[1], index = t[2], callbacks = t[3];
        img->tlsZeroFill = *reinterpret_cast<const uint32_t*>(img->base + tr + 32);
        if (end > start) img->tlsTemplate.assign(reinterpret_cast<const uint8_t*>(start), reinterpret_cast<const uint8_t*>(end));
        img->tlsIndex = registerTls(img);
        if (index) *reinterpret_cast<uint32_t*>(index) = uint32_t(img->tlsIndex);
        for (const uint64_t* cb = reinterpret_cast<const uint64_t*>(callbacks); cb && *cb; ++cb) img->tlsCallbacks.push_back(*cb);
    }

    // the sections' own protections
    const long pageSize = sysconf(_SC_PAGESIZE);
    for (const Sec& s : secs) {
        int prot = 0;
        if (s.flags & kScnRead) prot |= PROT_READ;
        if (s.flags & kScnWrite) prot |= PROT_WRITE;
        if (s.flags & kScnExecute) prot |= PROT_EXEC | PROT_READ;
        const uintptr_t from = reinterpret_cast<uintptr_t>(img->base + s.va) & ~uintptr_t(pageSize - 1);
        const uintptr_t to = (reinterpret_cast<uintptr_t>(img->base + s.va + s.vsize) + pageSize - 1) & ~uintptr_t(pageSize - 1);
        mprotect(reinterpret_cast<void*>(from), to - from, prot ? prot : PROT_READ);
    }
    mprotect(img->base, size_t(pageSize), PROT_READ); // (the headers)
    loadedImages().push_back(img);
    return img;
}

bool runInit(PeImage& img, std::string* error) {
    using DllMain = int __attribute__((ms_abi)) (*)(void*, uint32_t, void*);
    ensureThread();
    for (uint64_t cb : img.tlsCallbacks) reinterpret_cast<DllMain>(cb)(img.base, 1, nullptr);
    if (img.entry && !reinterpret_cast<DllMain>(img.base + img.entry)(img.base, 1, nullptr)) {
        if (error) *error = img.path + ": its DllMain failed";
        return false;
    }
    return true;
}

} // namespace ffxvk
