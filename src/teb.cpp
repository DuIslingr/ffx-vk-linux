#include "teb.h"

#include "pe.h"

#include <asm/prctl.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace ffxvk {

namespace {

// x86-64 TEB offsets (NT_TIB first)
constexpr size_t kStackBase = 0x08, kStackLimit = 0x10, kSelf = 0x30, kProcessId = 0x40, kThreadId = 0x48, kTlsPointer = 0x58,
                 kPeb = 0x60, kLastError = 0x68, kTebSize = 0x2000;
constexpr size_t kMaxTls = 64;

std::mutex tlsMutex;
std::vector<PeImage*> tlsImages;

uint8_t* processBlock() { // (the PEB: zeroed - BeingDebugged 0 -, nothing in it read by these DLLs)
    static uint8_t* peb = static_cast<uint8_t*>(std::calloc(1, 0x1000));
    return peb;
}

struct Teb {
    uint8_t* block = nullptr;
    void** tls = nullptr;
    size_t tlsMade = 0; // (the DLLs' copies made so far)
};
thread_local Teb teb;

} // namespace

int registerTls(PeImage* img) {
    std::lock_guard<std::mutex> lock(tlsMutex);
    if (tlsImages.size() >= kMaxTls) return -1;
    tlsImages.push_back(img);
    return int(tlsImages.size() - 1);
}

void ensureThread() {
    if (!teb.block) {
        teb.block = static_cast<uint8_t*>(std::calloc(1, kTebSize));
        teb.tls = static_cast<void**>(std::calloc(kMaxTls, sizeof(void*)));
        uint8_t* b = teb.block;
        // the stack's bounds: __chkstk probes down to StackLimit
        uintptr_t low = 0, high = ~uintptr_t(0);
        pthread_attr_t attr;
        if (pthread_getattr_np(pthread_self(), &attr) == 0) {
            void* addr = nullptr;
            size_t size = 0;
            if (pthread_attr_getstack(&attr, &addr, &size) == 0) low = reinterpret_cast<uintptr_t>(addr), high = low + size;
            pthread_attr_destroy(&attr);
        }
        *reinterpret_cast<uintptr_t*>(b + kStackBase) = high;
        *reinterpret_cast<uintptr_t*>(b + kStackLimit) = low;
        *reinterpret_cast<uint8_t**>(b + kSelf) = b;
        *reinterpret_cast<uint64_t*>(b + kProcessId) = uint64_t(getpid());
        *reinterpret_cast<uint64_t*>(b + kThreadId) = uint64_t(syscall(SYS_gettid));
        *reinterpret_cast<void***>(b + kTlsPointer) = teb.tls;
        *reinterpret_cast<uint8_t**>(b + kPeb) = processBlock();
        if (syscall(SYS_arch_prctl, ARCH_SET_GS, b) != 0) {
            std::fprintf(stderr, "ffx-vk-linux: the GS base can't be set\n");
            std::abort();
        }
    }
    // the TLS copies of DLLs loaded since this thread last entered
    std::lock_guard<std::mutex> lock(tlsMutex);
    for (; teb.tlsMade < tlsImages.size(); ++teb.tlsMade) {
        const PeImage* img = tlsImages[teb.tlsMade];
        const size_t n = img->tlsTemplate.size() + img->tlsZeroFill;
        auto* copy = static_cast<uint8_t*>(std::calloc(1, n ? n : 16));
        if (!img->tlsTemplate.empty()) std::memcpy(copy, img->tlsTemplate.data(), img->tlsTemplate.size());
        teb.tls[teb.tlsMade] = copy;
    }
}

uint32_t& lastError() {
    ensureThread();
    return *reinterpret_cast<uint32_t*>(teb.block + kLastError);
}

} // namespace ffxvk
