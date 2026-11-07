// The Windows functions AMD's FidelityFX Vulkan DLLs import, as Linux stand-ins with the
// Microsoft x64 calling convention (ms_abi): KERNEL32's few, the Universal C Runtime's and
// VCRUNTIME's C library, MSVCP140's mutex and error helpers, VERSION.dll. An import with no
// stand-in resolves to a trap that names it and stops.
#include "win32.h"

#include "pe.h"
#include "teb.h"
#include "vk_thunks.h"

#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>
#include <mutex>
#include <pthread.h>
#include <string>
#include <utility>
#include <vector>

#define WINAPI __attribute__((ms_abi))

namespace ffxvk {

namespace {

bool verbose() {
    static const bool v = std::getenv("FFXVK_DEBUG") != nullptr;
    return v;
}
#define LOG(...) (verbose() ? (void)std::fprintf(stderr, "ffx-vk-linux: " __VA_ARGS__) : (void)0)

[[noreturn]] void die(const char* what) {
    std::fprintf(stderr, "ffx-vk-linux: %s\n", what);
    std::fflush(stderr);
    std::abort();
}

std::string utf16ToUtf8(const char16_t* s, int len = -1) {
    std::string out;
    for (int i = 0; s && (len < 0 ? s[i] : i < len); ++i) {
        uint32_t c = s[i];
        if (c >= 0xd800 && c < 0xdc00 && (len < 0 || i + 1 < len) && s[i + 1] >= 0xdc00 && s[i + 1] < 0xe000)
            c = 0x10000 + ((c - 0xd800) << 10) + (s[++i] - 0xdc00);
        if (c < 0x80) out += char(c);
        else if (c < 0x800) out += char(0xc0 | c >> 6), out += char(0x80 | (c & 0x3f));
        else if (c < 0x10000) out += char(0xe0 | c >> 12), out += char(0x80 | ((c >> 6) & 0x3f)), out += char(0x80 | (c & 0x3f));
        else out += char(0xf0 | c >> 18), out += char(0x80 | ((c >> 12) & 0x3f)), out += char(0x80 | ((c >> 6) & 0x3f)), out += char(0x80 | (c & 0x3f));
    }
    return out;
}
std::u16string utf8ToUtf16(const char* s, int len = -1) {
    std::u16string out;
    const auto* u = reinterpret_cast<const unsigned char*>(s);
    for (int i = 0; s && (len < 0 ? u[i] : i < len);) {
        uint32_t c = u[i++];
        int more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
        if (more) c &= 0x3f >> more;
        for (; more > 0 && (len < 0 ? u[i] : i < len); --more) c = c << 6 | (u[i++] & 0x3f);
        if (c >= 0x10000) out += char16_t(0xd800 + ((c - 0x10000) >> 10)), out += char16_t(0xdc00 + ((c - 0x10000) & 0x3ff));
        else out += char16_t(c);
    }
    return out;
}

// ---- modules: the loaded DLLs, and pseudo-handles for the system DLLs stood in for
struct SystemModule {
    std::string name;
};
std::map<std::string, SystemModule*>& systemModules() {
    static std::map<std::string, SystemModule*> m;
    return m;
}
std::string dllDirectory; // (where AMD's DLLs are looked for)

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
// a Windows path's file name ("C:\x\y.dll", "y.dll" -> "y.dll")
std::string fileName(const std::string& p) {
    const size_t at = p.find_last_of("\\/");
    return at == std::string::npos ? p : p.substr(at + 1);
}
PeImage* imageNamed(const std::string& name) {
    for (PeImage* img : loadedImages())
        if (img->name == lower(fileName(name))) return img;
    return nullptr;
}
PeImage* imageHolding(const void* p) {
    for (PeImage* img : loadedImages())
        if (img->contains(p)) return img;
    return nullptr;
}
std::string windowsPath(const std::string& unixPath) { // ("/a/b.dll" -> "\a\b.dll": its directory found by '\')
    std::string w = unixPath;
    for (char& c : w)
        if (c == '/') c = '\\';
    return w;
}

void* loadLibrary(const std::string& asked) {
    const std::string name = lower(fileName(asked));
    if (PeImage* img = imageNamed(name)) return img->base;
    if (auto it = systemModules().find(name); it != systemModules().end()) return it->second;
    // one of AMD's beside the first: loaded the same way
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dllDirectory, ec))
        if (lower(e.path().filename().string()) == name) {
            std::string err;
            PeImage* img = loadPe(e.path().string(), resolveImport, &err);
            if (!img || !runInit(*img, &err)) {
                std::fprintf(stderr, "ffx-vk-linux: %s\n", err.c_str());
                return nullptr;
            }
            LOG("loaded %s at %p\n", img->path.c_str(), static_cast<void*>(img->base));
            return img->base;
        }
    LOG("LoadLibrary(%s): not here\n", asked.c_str());
    lastError() = 126; // (ERROR_MOD_NOT_FOUND)
    return nullptr;
}

// ---- KERNEL32
WINAPI void* k_LoadLibraryA(const char* n) { return loadLibrary(n ? n : ""); }
WINAPI void* k_LoadLibraryExA(const char* n, void*, uint32_t) { return loadLibrary(n ? n : ""); }
WINAPI void* k_LoadLibraryExW(const char16_t* n, void*, uint32_t) { return loadLibrary(utf16ToUtf8(n)); }
WINAPI int k_FreeLibrary(void*) { return 1; }
WINAPI void* k_GetModuleHandleA(const char* n) {
    if (!n) return loadedImages().empty() ? nullptr : loadedImages().front()->base;
    if (PeImage* img = imageNamed(n)) return img->base;
    auto it = systemModules().find(lower(fileName(n)));
    return it == systemModules().end() ? nullptr : it->second;
}
WINAPI int k_GetModuleHandleExW(uint32_t flags, const void* what, void** out) {
    void* h = nullptr;
    if (flags & 4) { // (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS)
        if (PeImage* img = imageHolding(what)) h = img->base;
    } else {
        h = what ? k_GetModuleHandleA(utf16ToUtf8(static_cast<const char16_t*>(what)).c_str()) : k_GetModuleHandleA(nullptr);
    }
    if (out) *out = h;
    return h != nullptr;
}
std::string modulePath(void* h) {
    for (PeImage* img : loadedImages())
        if (img->base == h || (!h && img == loadedImages().front())) return windowsPath(img->path);
    return {};
}
WINAPI uint32_t k_GetModuleFileNameA(void* h, char* buf, uint32_t size) {
    const std::string p = modulePath(h);
    if (!buf || !size) return 0;
    std::snprintf(buf, size, "%s", p.c_str());
    return uint32_t(std::min<size_t>(p.size(), size - 1));
}
WINAPI uint32_t k_GetModuleFileNameW(void* h, char16_t* buf, uint32_t size) {
    const std::u16string p = utf8ToUtf16(modulePath(h).c_str());
    if (!buf || !size) return 0;
    const size_t n = std::min<size_t>(p.size(), size - 1);
    std::memcpy(buf, p.data(), n * 2);
    buf[n] = 0;
    return uint32_t(n);
}
WINAPI void* k_GetProcAddress(void* h, const char* name) {
    const std::string n = reinterpret_cast<uintptr_t>(name) < 0x10000 ? "#" + std::to_string(reinterpret_cast<uintptr_t>(name)) : std::string(name);
    for (PeImage* img : loadedImages())
        if (img->base == h) {
            void* p = img->exportOf(n);
            LOG("GetProcAddress(%s, %s) = %p\n", img->name.c_str(), n.c_str(), p);
            return p;
        }
    for (auto& [dll, m] : systemModules())
        if (m == h) {
            void* p = standIn(dll, n);
            LOG("GetProcAddress(%s, %s) = %p\n", dll.c_str(), n.c_str(), p);
            return p;
        }
    return nullptr;
}
WINAPI int k_DisableThreadLibraryCalls(void*) { return 1; }
WINAPI uint32_t k_GetLastError() { return lastError(); }
WINAPI uint32_t k_GetCurrentThreadId() { return uint32_t(syscall(SYS_gettid)); }
WINAPI uint32_t k_GetCurrentProcessId() { return uint32_t(getpid()); }
WINAPI void* k_GetCurrentProcess() { return reinterpret_cast<void*>(intptr_t(-1)); }
WINAPI int k_TerminateProcess(void*, uint32_t code) { die(("TerminateProcess(" + std::to_string(code) + ")").c_str()); }
WINAPI int k_IsDebuggerPresent() { return 0; }
WINAPI void k_DebugBreak() { LOG("DebugBreak\n"); }
WINAPI int k_IsProcessorFeaturePresent(uint32_t f) { return f == 23 ? 0 : 1; } // (not PF_FASTFAIL_AVAILABLE: no int 0x29 here)
WINAPI void k_OutputDebugStringA(const char* s) { std::fprintf(stderr, "ffx: %s%s", s ? s : "", s && *s && s[std::strlen(s) - 1] == '\n' ? "" : "\n"); }
WINAPI void k_OutputDebugStringW(const char16_t* s) { k_OutputDebugStringA(utf16ToUtf8(s).c_str()); }
WINAPI void k_InitializeSListHead(void* h) { std::memset(h, 0, 16); }
WINAPI void* k_SetUnhandledExceptionFilter(void*) { return nullptr; }
WINAPI long k_UnhandledExceptionFilter(void*) { die("an unhandled exception"); }
WINAPI void k_RtlCaptureContext(void*) { die("RtlCaptureContext (a security check failed?)"); }
WINAPI void* k_RtlLookupFunctionEntry(uint64_t, uint64_t*, void*) { return nullptr; }
WINAPI void* k_RtlVirtualUnwind(uint32_t, uint64_t, uint64_t, void*, void*, void**, uint64_t*, void*) { return nullptr; }
WINAPI uint64_t k_GetTickCount64() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return uint64_t(t.tv_sec) * 1000 + uint64_t(t.tv_nsec) / 1000000;
}
WINAPI int k_QueryPerformanceCounter(int64_t* c) {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    *c = int64_t(t.tv_sec) * 1000000000 + t.tv_nsec;
    return 1;
}
WINAPI int k_QueryPerformanceFrequency(int64_t* f) { return *f = 1000000000, 1; }
WINAPI void k_GetSystemTimeAsFileTime(uint64_t* ft) {
    timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    *ft = (uint64_t(t.tv_sec) + 11644473600ull) * 10000000ull + uint64_t(t.tv_nsec) / 100;
}
WINAPI int k_MultiByteToWideChar(uint32_t, uint32_t, const char* s, int len, char16_t* out, int outLen) {
    std::u16string w = utf8ToUtf16(s, len);
    if (len < 0) w += u'\0';
    if (!outLen) return int(w.size());
    if (int(w.size()) > outLen) return lastError() = 122, 0; // (ERROR_INSUFFICIENT_BUFFER)
    std::memcpy(out, w.data(), w.size() * 2);
    return int(w.size());
}
WINAPI int k_WideCharToMultiByte(uint32_t, uint32_t, const char16_t* s, int len, char* out, int outLen, const char*, int* usedDefault) {
    std::string n = utf16ToUtf8(s, len);
    if (len < 0) n += '\0';
    if (usedDefault) *usedDefault = 0;
    if (!outLen) return int(n.size());
    if (int(n.size()) > outLen) return lastError() = 122, 0;
    std::memcpy(out, n.data(), n.size());
    return int(n.size());
}
WINAPI int k_AreFileApisANSI() { return 1; }
WINAPI int k_GetLocaleInfoEx(const char16_t*, uint32_t, char16_t* out, int n) {
    if (out && n) out[0] = 0;
    return 0;
}
WINAPI uint32_t k_FormatMessageA(uint32_t, const void*, uint32_t, uint32_t, char* buf, uint32_t, void*) {
    if (buf) buf[0] = 0;
    return 0;
}
WINAPI void* k_LocalFree(void* p) { return std::free(p), nullptr; }

// slim reader/writer locks and condition variables: a pointer-sized word each, zero to begin
// with - here a futex word (exclusive only: what these DLLs take)
long futex(void* addr, int op, uint32_t val) { return syscall(SYS_futex, addr, op, val, nullptr, nullptr, 0); }
WINAPI void k_AcquireSRWLockExclusive(void* lock) {
    auto* w = static_cast<std::atomic<uint32_t>*>(lock);
    uint32_t c = 0;
    if (w->compare_exchange_strong(c, 1)) return;
    if (c != 2) c = w->exchange(2);
    while (c != 0) {
        futex(lock, FUTEX_WAIT_PRIVATE, 2);
        c = w->exchange(2);
    }
}
WINAPI void k_ReleaseSRWLockExclusive(void* lock) {
    auto* w = static_cast<std::atomic<uint32_t>*>(lock);
    if (w->exchange(0) == 2) futex(lock, FUTEX_WAKE_PRIVATE, 1);
}
WINAPI int k_SleepConditionVariableSRW(void* cv, void* lock, uint32_t ms, uint32_t) {
    auto* seq = static_cast<std::atomic<uint32_t>*>(cv);
    const uint32_t s = seq->load();
    k_ReleaseSRWLockExclusive(lock);
    if (ms == 0xffffffffu) {
        futex(cv, FUTEX_WAIT_PRIVATE, s);
    } else {
        timespec t{long(ms / 1000), long(ms % 1000) * 1000000};
        syscall(SYS_futex, cv, FUTEX_WAIT_PRIVATE, s, &t, nullptr, 0);
    }
    k_AcquireSRWLockExclusive(lock);
    return 1;
}
WINAPI void k_WakeAllConditionVariable(void* cv) {
    static_cast<std::atomic<uint32_t>*>(cv)->fetch_add(1);
    futex(cv, FUTEX_WAKE_PRIVATE, INT_MAX);
}
WINAPI void k_WakeConditionVariable(void* cv) {
    static_cast<std::atomic<uint32_t>*>(cv)->fetch_add(1);
    futex(cv, FUTEX_WAKE_PRIVATE, 1);
}

// ---- VERSION.dll: no Windows files' version resources here
WINAPI uint32_t v_GetFileVersionInfoSizeA(const char* f, uint32_t* h) {
    LOG("GetFileVersionInfoSizeA(%s)\n", f ? f : "");
    if (h) *h = 0;
    lastError() = 2;
    return 0;
}
WINAPI int v_GetFileVersionInfoA(const char* f, uint32_t, uint32_t, void*) {
    LOG("GetFileVersionInfoA(%s)\n", f ? f : "");
    return 0;
}
WINAPI int v_VerQueryValueA(const void*, const char* q, void**, uint32_t* n) {
    LOG("VerQueryValueA(%s)\n", q ? q : "");
    if (n) *n = 0;
    return 0;
}

// ---- the C runtime (UCRT, VCRUNTIME140)
WINAPI void* c_malloc(size_t n) { return std::malloc(n); }
WINAPI void* c_calloc(size_t a, size_t b) { return std::calloc(a, b); }
WINAPI void c_free(void* p) { std::free(p); }
WINAPI int c_callnewh(size_t) { return 0; }
WINAPI void* c_memcpy(void* d, const void* s, size_t n) { return std::memcpy(d, s, n); }
WINAPI void* c_memmove(void* d, const void* s, size_t n) { return std::memmove(d, s, n); }
WINAPI void* c_memset(void* d, int v, size_t n) { return std::memset(d, v, n); }
WINAPI int c_memcmp(const void* a, const void* b, size_t n) { return std::memcmp(a, b, n); }
WINAPI size_t c_strlen(const char* s) { return std::strlen(s); }
WINAPI int c_strcmp(const char* a, const char* b) { return std::strcmp(a, b); }
WINAPI int c_strncpy_s(char* d, size_t size, const char* s, size_t n) {
    if (!d || !size) return EINVAL;
    const size_t len = std::min(n == size_t(-1) ? std::strlen(s) : strnlen(s, n), size - 1);
    std::memcpy(d, s, len);
    d[len] = 0;
    return 0;
}
WINAPI int c_wcscpy_s(char16_t* d, size_t size, const char16_t* s) {
    size_t i = 0;
    for (; i + 1 < size && s[i]; ++i) d[i] = s[i];
    if (size) d[i] = 0;
    return 0;
}
WINAPI int c_getenv_s(size_t* req, char* buf, size_t size, const char* name) {
    const char* v = std::getenv(name);
    if (!v) {
        if (req) *req = 0;
        if (buf && size) buf[0] = 0;
        return 0;
    }
    const size_t n = std::strlen(v) + 1;
    if (req) *req = n;
    if (!buf || size < n) return size ? ERANGE : 0;
    std::memcpy(buf, v, n);
    return 0;
}
WINAPI unsigned c_lc_codepage() { return 65001; }
// startup: the C++ static constructors, the onexit tables
using VoidFn = void WINAPI (*)();
using IntFn = int WINAPI (*)();
WINAPI void c_initterm(VoidFn* a, VoidFn* b) {
    for (; a < b; ++a)
        if (*a) (*a)();
}
WINAPI int c_initterm_e(IntFn* a, IntFn* b) {
    for (; a < b; ++a)
        if (*a)
            if (int r = (*a)()) return r;
    return 0;
}
WINAPI int c_zero() { return 0; }
WINAPI int c_initialize_onexit_table(void* t) { return std::memset(t, 0, 24), 0; }
std::mutex onexitMutex;
std::vector<VoidFn> onexit;
WINAPI int c_register_onexit_function(void*, VoidFn f) {
    std::lock_guard<std::mutex> lock(onexitMutex);
    onexit.push_back(f);
    return 0;
}
WINAPI int c_crt_atexit(VoidFn f) { return c_register_onexit_function(nullptr, f); }
WINAPI int c_execute_onexit_table(void*) { return 0; }
WINAPI void c_cexit() {}
WINAPI int c_seh_filter_dll(unsigned long, void*) { return 0; }
WINAPI void c_invoke_watson(const char16_t*, const char16_t*, const char16_t*, unsigned, uintptr_t) { die("the C runtime's invalid-parameter handler"); }
// C++ support: exceptions are never thrown through here (a throw stops, naming itself)
WINAPI void c_CxxThrowException(void*, void*) { die("a C++ exception was thrown in AMD's DLL (no unwinding here)"); }
WINAPI int c_frame_handler(void*, void*, void*, void*) { return 1; } // (ExceptionContinueSearch: never called without SEH)
WINAPI void c_std_terminate() { die("std::terminate in AMD's DLL"); }
WINAPI int c_purecall() { die("a pure virtual call in AMD's DLL"); }
WINAPI void c_type_info_destroy_list(void*) {}
struct StdExceptionData {
    const char* what;
    bool doFree;
};
WINAPI void c_std_exception_copy(const StdExceptionData* from, StdExceptionData* to) {
    to->what = from->what ? strdup(from->what) : nullptr, to->doFree = from->what != nullptr;
}
WINAPI void c_std_exception_destroy(StdExceptionData* d) {
    if (d->doFree) std::free(const_cast<char*>(d->what));
    d->what = nullptr, d->doFree = false;
}
// maths
WINAPI double m_log2(double x) { return std::log2(x); }
WINAPI double m_floor(double x) { return std::floor(x); }
WINAPI float m_floorf(float x) { return std::floor(x); }
WINAPI float m_cosf(float x) { return std::cos(x); }
WINAPI float m_sinf(float x) { return std::sin(x); }
WINAPI float m_tanf(float x) { return std::tan(x); }
WINAPI float m_atanf(float x) { return std::atan(x); }
WINAPI float m_powf(float a, float b) { return std::pow(a, b); }
WINAPI float m_roundf(float x) { return std::round(x); }
WINAPI float m_log2f(float x) { return std::log2(x); }
WINAPI float m_exp2f(float x) { return std::exp2(x); }
short classOf(int c) { return c == FP_INFINITE ? 1 : c == FP_NAN ? 2 : c == FP_NORMAL ? -1 : c == FP_SUBNORMAL ? -2 : 0; } // (MSVC's)
WINAPI short m_dclass(double x) { return classOf(std::fpclassify(x)); }
WINAPI short m_fdclass(float x) { return classOf(std::fpclassify(x)); }
WINAPI short m_ldclass(double x) { return classOf(std::fpclassify(x)); } // (long double is double on Windows)
WINAPI int m_dsign(double x) { return std::signbit(x) ? 0x8000 : 0; }
WINAPI int m_fdsign(float x) { return std::signbit(x) ? 0x8000 : 0; }
WINAPI int m_ldsign(double x) { return std::signbit(x) ? 0x8000 : 0; }

// printf with a Microsoft va_list: each argument in an 8-byte slot (floats as doubles), long 4 bytes
std::string formatMs(const char* fmt, const uint64_t* args, bool wide) {
    std::string out;
    for (const char* p = fmt; *p; ++p) {
        if (*p != '%') {
            out += *p;
            continue;
        }
        if (p[1] == '%') {
            out += '%', ++p;
            continue;
        }
        std::string spec = "%";
        ++p;
        while (*p && std::strchr("-+ #0", *p)) spec += *p++;
        while (*p && (std::isdigit(static_cast<unsigned char>(*p)) || *p == '.' || *p == '*')) {
            if (*p == '*') spec += std::to_string(int(*args++)), ++p;
            else spec += *p++;
        }
        int len = 0; // 0 int, 1 long (4 on Windows), 2 64-bit, 3 short
        bool wideArg = false;
        for (;; ++p) {
            if (*p == 'l') len = len == 1 ? 2 : 1, wideArg = true;
            else if (*p == 'h') len = 3, wideArg = false;
            else if (*p == 'z' || *p == 'j' || *p == 't') len = 2;
            else if (*p == 'I' && p[1] == '6' && p[2] == '4') len = 2, p += 2;
            else if (*p == 'I' && p[1] == '3' && p[2] == '2') len = 0, p += 2;
            else if (*p == 'I') len = 2;
            else if (*p == 'L' || *p == 'w') wideArg = true;
            else break;
        }
        const char conv = *p;
        if (!conv) break;
        char buf[512];
        const uint64_t a = *args++;
        switch (conv) {
        case 'd': case 'i':
            std::snprintf(buf, sizeof buf, (spec + "lld").c_str(), len == 2 ? (long long)a : len == 3 ? (long long)(int16_t)a : (long long)(int32_t)a);
            out += buf;
            break;
        case 'u': case 'x': case 'X': case 'o':
            std::snprintf(buf, sizeof buf, (spec + "ll" + conv).c_str(), len == 2 ? (unsigned long long)a : len == 3 ? (unsigned long long)(uint16_t)a : (unsigned long long)(uint32_t)a);
            out += buf;
            break;
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            double d;
            std::memcpy(&d, &a, 8);
            std::snprintf(buf, sizeof buf, (spec + conv).c_str(), d);
            out += buf;
            break;
        }
        case 'c': case 'C':
            out += char(a);
            break;
        case 'p':
            std::snprintf(buf, sizeof buf, "%016llX", (unsigned long long)a);
            out += buf;
            break;
        case 's': case 'S': {
            const bool w = (conv == 's') == (wide ? !(len == 3) : wideArg); // (in a wide printf %s is wide, %hs narrow)
            const std::string s = !a ? "(null)" : w ? utf16ToUtf8(reinterpret_cast<const char16_t*>(a)) : std::string(reinterpret_cast<const char*>(a));
            std::snprintf(buf, sizeof buf, (spec + "s").c_str(), s.c_str());
            out += buf;
            break;
        }
        default:
            out += spec + conv;
            break;
        }
    }
    return out;
}
WINAPI int c_stdio_common_vsprintf(uint64_t, char* buf, size_t count, const char* fmt, void*, const uint64_t* args) {
    const std::string s = formatMs(fmt, args, false);
    if (buf && count) {
        const size_t n = std::min(s.size(), count - 1);
        std::memcpy(buf, s.data(), n);
        buf[n] = 0;
        if (s.size() >= count) return -1;
    }
    return int(s.size());
}
WINAPI int c_stdio_common_vswprintf_s(uint64_t, char16_t* buf, size_t count, const char16_t* fmt, void*, const uint64_t* args) {
    const std::u16string s = utf8ToUtf16(formatMs(utf16ToUtf8(fmt).c_str(), args, true).c_str());
    if (!buf || !count) return -1;
    const size_t n = std::min(s.size(), count - 1);
    std::memcpy(buf, s.data(), n * 2);
    buf[n] = 0;
    return s.size() >= count ? -1 : int(s.size());
}

// ---- MSVCP140: std::mutex's in-place storage (a pthread mutex in it), the error throwers
WINAPI void p_Mtx_init_in_situ(void* m, int) { pthread_mutex_init(static_cast<pthread_mutex_t*>(m), nullptr); }
WINAPI int p_Mtx_lock(void* m) { return pthread_mutex_lock(static_cast<pthread_mutex_t*>(m)), 0; }
WINAPI int p_Mtx_unlock(void* m) { return pthread_mutex_unlock(static_cast<pthread_mutex_t*>(m)), 0; }
WINAPI void p_Throw_Cpp_error(int code) { die(("std::_Throw_Cpp_error(" + std::to_string(code) + ")").c_str()); }
WINAPI void p_Xlength_error(const char* s) { die((std::string("std::length_error: ") + (s ? s : "")).c_str()); }
WINAPI void p_Xout_of_range(const char* s) { die((std::string("std::out_of_range: ") + (s ? s : "")).c_str()); }
WINAPI void p_Xbad_alloc() { die("std::bad_alloc"); }
// (data: locale ids)
int localeIdCount = 0;
size_t numpunctCharId = 0, numpunctWcharId = 0;

// ---- traps: an import with no stand-in, named when it's called
constexpr int kTraps = 256;
std::vector<std::string> trapNames;
template <int N> WINAPI uint64_t trap() {
    die(("an unimplemented Windows function was called: " + (N < int(trapNames.size()) ? trapNames[N] : std::string("?"))).c_str());
}
template <int... N> constexpr std::array<void*, sizeof...(N)> trapTable(std::integer_sequence<int, N...>) {
    return {reinterpret_cast<void*>(&trap<N>)...};
}
const auto traps = trapTable(std::make_integer_sequence<int, kTraps>{});

#define F(dll, name, fn) {dll "!" name, reinterpret_cast<void*>(&fn)}
#define D(dll, name, obj) {dll "!" name, reinterpret_cast<void*>(&obj)}
const std::map<std::string, void*>& table() {
    static const std::map<std::string, void*> t = {
        F("kernel32.dll", "LoadLibraryA", k_LoadLibraryA), F("kernel32.dll", "LoadLibraryExA", k_LoadLibraryExA),
        F("kernel32.dll", "LoadLibraryExW", k_LoadLibraryExW), F("kernel32.dll", "FreeLibrary", k_FreeLibrary),
        F("kernel32.dll", "GetModuleHandleA", k_GetModuleHandleA), F("kernel32.dll", "GetModuleHandleExW", k_GetModuleHandleExW),
        F("kernel32.dll", "GetModuleFileNameA", k_GetModuleFileNameA), F("kernel32.dll", "GetModuleFileNameW", k_GetModuleFileNameW),
        F("kernel32.dll", "GetProcAddress", k_GetProcAddress), F("kernel32.dll", "DisableThreadLibraryCalls", k_DisableThreadLibraryCalls),
        F("kernel32.dll", "GetLastError", k_GetLastError), F("kernel32.dll", "GetCurrentThreadId", k_GetCurrentThreadId),
        F("kernel32.dll", "GetCurrentProcessId", k_GetCurrentProcessId), F("kernel32.dll", "GetCurrentProcess", k_GetCurrentProcess),
        F("kernel32.dll", "TerminateProcess", k_TerminateProcess), F("kernel32.dll", "IsDebuggerPresent", k_IsDebuggerPresent),
        F("kernel32.dll", "DebugBreak", k_DebugBreak), F("kernel32.dll", "IsProcessorFeaturePresent", k_IsProcessorFeaturePresent),
        F("kernel32.dll", "OutputDebugStringA", k_OutputDebugStringA), F("kernel32.dll", "OutputDebugStringW", k_OutputDebugStringW),
        F("kernel32.dll", "InitializeSListHead", k_InitializeSListHead),
        F("kernel32.dll", "SetUnhandledExceptionFilter", k_SetUnhandledExceptionFilter),
        F("kernel32.dll", "UnhandledExceptionFilter", k_UnhandledExceptionFilter), F("kernel32.dll", "RtlCaptureContext", k_RtlCaptureContext),
        F("kernel32.dll", "RtlLookupFunctionEntry", k_RtlLookupFunctionEntry), F("kernel32.dll", "RtlVirtualUnwind", k_RtlVirtualUnwind),
        F("kernel32.dll", "GetTickCount64", k_GetTickCount64), F("kernel32.dll", "QueryPerformanceCounter", k_QueryPerformanceCounter),
        F("kernel32.dll", "QueryPerformanceFrequency", k_QueryPerformanceFrequency),
        F("kernel32.dll", "GetSystemTimeAsFileTime", k_GetSystemTimeAsFileTime), F("kernel32.dll", "MultiByteToWideChar", k_MultiByteToWideChar),
        F("kernel32.dll", "WideCharToMultiByte", k_WideCharToMultiByte), F("kernel32.dll", "AreFileApisANSI", k_AreFileApisANSI),
        F("kernel32.dll", "GetLocaleInfoEx", k_GetLocaleInfoEx), F("kernel32.dll", "FormatMessageA", k_FormatMessageA),
        F("kernel32.dll", "LocalFree", k_LocalFree), F("kernel32.dll", "AcquireSRWLockExclusive", k_AcquireSRWLockExclusive),
        F("kernel32.dll", "ReleaseSRWLockExclusive", k_ReleaseSRWLockExclusive),
        F("kernel32.dll", "SleepConditionVariableSRW", k_SleepConditionVariableSRW),
        F("kernel32.dll", "WakeAllConditionVariable", k_WakeAllConditionVariable), F("kernel32.dll", "WakeConditionVariable", k_WakeConditionVariable),
        F("version.dll", "GetFileVersionInfoSizeA", v_GetFileVersionInfoSizeA), F("version.dll", "GetFileVersionInfoA", v_GetFileVersionInfoA),
        F("version.dll", "VerQueryValueA", v_VerQueryValueA),
        F("api-ms-win-crt-heap-l1-1-0.dll", "malloc", c_malloc), F("api-ms-win-crt-heap-l1-1-0.dll", "calloc", c_calloc),
        F("api-ms-win-crt-heap-l1-1-0.dll", "free", c_free), F("api-ms-win-crt-heap-l1-1-0.dll", "_callnewh", c_callnewh),
        F("vcruntime140.dll", "memcpy", c_memcpy), F("vcruntime140.dll", "memmove", c_memmove), F("vcruntime140.dll", "memset", c_memset),
        F("vcruntime140.dll", "memcmp", c_memcmp), F("api-ms-win-crt-string-l1-1-0.dll", "strlen", c_strlen),
        F("api-ms-win-crt-string-l1-1-0.dll", "strcmp", c_strcmp), F("api-ms-win-crt-string-l1-1-0.dll", "strncpy_s", c_strncpy_s),
        F("api-ms-win-crt-string-l1-1-0.dll", "wcscpy_s", c_wcscpy_s), F("api-ms-win-crt-environment-l1-1-0.dll", "getenv_s", c_getenv_s),
        F("api-ms-win-crt-locale-l1-1-0.dll", "___lc_codepage_func", c_lc_codepage),
        F("api-ms-win-crt-runtime-l1-1-0.dll", "_initterm", c_initterm), F("api-ms-win-crt-runtime-l1-1-0.dll", "_initterm_e", c_initterm_e),
        F("api-ms-win-crt-runtime-l1-1-0.dll", "_configure_narrow_argv", c_zero),
        F("api-ms-win-crt-runtime-l1-1-0.dll", "_initialize_narrow_environment", c_zero),
        F("api-ms-win-crt-runtime-l1-1-0.dll", "_initialize_onexit_table", c_initialize_onexit_table),
        F("api-ms-win-crt-runtime-l1-1-0.dll", "_register_onexit_function", c_register_onexit_function),
        F("api-ms-win-crt-runtime-l1-1-0.dll", "_execute_onexit_table", c_execute_onexit_table),
        F("api-ms-win-crt-runtime-l1-1-0.dll", "_crt_atexit", c_crt_atexit), F("api-ms-win-crt-runtime-l1-1-0.dll", "_cexit", c_cexit),
        F("api-ms-win-crt-runtime-l1-1-0.dll", "_seh_filter_dll", c_seh_filter_dll),
        F("api-ms-win-crt-runtime-l1-1-0.dll", "_invoke_watson", c_invoke_watson),
        F("api-ms-win-crt-stdio-l1-1-0.dll", "__stdio_common_vsprintf", c_stdio_common_vsprintf),
        F("api-ms-win-crt-stdio-l1-1-0.dll", "__stdio_common_vswprintf_s", c_stdio_common_vswprintf_s),
        F("vcruntime140.dll", "_CxxThrowException", c_CxxThrowException), F("vcruntime140.dll", "__C_specific_handler", c_frame_handler),
        F("vcruntime140_1.dll", "__CxxFrameHandler4", c_frame_handler), F("vcruntime140.dll", "__std_terminate", c_std_terminate),
        F("vcruntime140.dll", "_purecall", c_purecall), F("vcruntime140.dll", "__std_type_info_destroy_list", c_type_info_destroy_list),
        F("vcruntime140.dll", "__std_exception_copy", c_std_exception_copy),
        F("vcruntime140.dll", "__std_exception_destroy", c_std_exception_destroy),
        F("api-ms-win-crt-math-l1-1-0.dll", "log2", m_log2), F("api-ms-win-crt-math-l1-1-0.dll", "floor", m_floor),
        F("api-ms-win-crt-math-l1-1-0.dll", "floorf", m_floorf), F("api-ms-win-crt-math-l1-1-0.dll", "cosf", m_cosf),
        F("api-ms-win-crt-math-l1-1-0.dll", "sinf", m_sinf), F("api-ms-win-crt-math-l1-1-0.dll", "tanf", m_tanf),
        F("api-ms-win-crt-math-l1-1-0.dll", "atanf", m_atanf), F("api-ms-win-crt-math-l1-1-0.dll", "powf", m_powf),
        F("api-ms-win-crt-math-l1-1-0.dll", "roundf", m_roundf), F("api-ms-win-crt-math-l1-1-0.dll", "log2f", m_log2f),
        F("api-ms-win-crt-math-l1-1-0.dll", "exp2f", m_exp2f), F("api-ms-win-crt-math-l1-1-0.dll", "_dclass", m_dclass),
        F("api-ms-win-crt-math-l1-1-0.dll", "_fdclass", m_fdclass), F("api-ms-win-crt-math-l1-1-0.dll", "_ldclass", m_ldclass),
        F("api-ms-win-crt-math-l1-1-0.dll", "_dsign", m_dsign), F("api-ms-win-crt-math-l1-1-0.dll", "_fdsign", m_fdsign),
        F("api-ms-win-crt-math-l1-1-0.dll", "_ldsign", m_ldsign),
        F("msvcp140.dll", "_Mtx_init_in_situ", p_Mtx_init_in_situ), F("msvcp140.dll", "_Mtx_lock", p_Mtx_lock),
        F("msvcp140.dll", "_Mtx_unlock", p_Mtx_unlock), F("msvcp140.dll", "?_Throw_Cpp_error@std@@YAXH@Z", p_Throw_Cpp_error),
        F("msvcp140.dll", "?_Xlength_error@std@@YAXPEBD@Z", p_Xlength_error), F("msvcp140.dll", "?_Xout_of_range@std@@YAXPEBD@Z", p_Xout_of_range),
        F("msvcp140.dll", "?_Xbad_alloc@std@@YAXXZ", p_Xbad_alloc),
        D("msvcp140.dll", "?_Id_cnt@id@locale@std@@0HA", localeIdCount),
        D("msvcp140.dll", "?id@?$numpunct@D@std@@2V0locale@2@A", numpunctCharId),
        D("msvcp140.dll", "?id@?$numpunct@_W@std@@2V0locale@2@A", numpunctWcharId),
    };
    return t;
}
#undef F
#undef D

} // namespace

void* standIn(const std::string& dll, const std::string& name) {
    const auto& t = table();
    if (auto it = t.find(dll + "!" + name); it != t.end()) return it->second;
    return nullptr;
}

void* resolveImport(const std::string& dll, const std::string& name) {
    if (dll == "vulkan-1.dll") {
        if (void* p = vulkanImport(name)) return p;
    } else if (void* p = standIn(dll, name)) {
        return p;
    }
    if (trapNames.size() >= size_t(kTraps)) return nullptr;
    trapNames.push_back(dll + "!" + name);
    LOG("no stand-in for %s!%s (a trap)\n", dll.c_str(), name.c_str());
    return traps[trapNames.size() - 1];
}

void setDllDirectory(const std::string& dir) {
    dllDirectory = dir;
    for (const char* n : {"kernel32.dll", "version.dll", "vulkan-1.dll", "msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll"})
        if (!systemModules().count(n)) systemModules()[n] = new SystemModule{n};
}

void* loadDll(const std::string& name) { return loadLibrary(name); }

std::string fromUtf16(const char16_t* s) { return utf16ToUtf8(s); }

} // namespace ffxvk
