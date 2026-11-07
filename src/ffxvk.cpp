// AMD's FSR API exported to Linux code: each call entered on a thread with its TEB, the Linux
// function pointers in its descriptions swapped for bridged ones (restored after), and passed to
// the DLL's own entry point (Microsoft x64 calling convention).
#include "ffx_vk_linux.h"

#include "pe.h"
#include "teb.h"
#include "vk_thunks.h"
#include "win32.h"

#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace {

using MsCreateContext = ffxReturnCode_t __attribute__((ms_abi)) (*)(ffxContext*, ffxCreateContextDescHeader*, const ffxAllocationCallbacks*);
using MsDestroyContext = ffxReturnCode_t __attribute__((ms_abi)) (*)(ffxContext*, const ffxAllocationCallbacks*);
using MsConfigure = ffxReturnCode_t __attribute__((ms_abi)) (*)(ffxContext*, const ffxConfigureDescHeader*);
using MsQuery = ffxReturnCode_t __attribute__((ms_abi)) (*)(ffxContext*, ffxQueryDescHeader*);
using MsDispatch = ffxReturnCode_t __attribute__((ms_abi)) (*)(ffxContext*, const ffxDispatchDescHeader*);

struct Entries {
    MsCreateContext createContext = nullptr;
    MsDestroyContext destroyContext = nullptr;
    MsConfigure configure = nullptr;
    MsQuery query = nullptr;
    MsDispatch dispatch = nullptr;
} entries;
std::string error;
std::mutex loadMutex;

// ---- the bridges

// the program's message callback, called with the DLL's UTF-16 text as wchar_t
ffxApiMessage appMessage = nullptr;
__attribute__((ms_abi)) void msMessage(uint32_t type, const char16_t* text) {
    if (!appMessage) return;
    const std::string s = ffxvk::fromUtf16(text);
    std::wstring w(s.size(), L'\0');
    const size_t n = std::mbstowcs(w.data(), s.c_str(), w.size());
    w.resize(n == size_t(-1) ? 0 : n);
    appMessage(type, w.c_str());
}

// the program's allocation callbacks (one set at a time)
ffxAllocationCallbacks appAlloc{};
__attribute__((ms_abi)) void* msAlloc(void*, uint64_t size) { return appAlloc.alloc(appAlloc.pUserData, size); }
__attribute__((ms_abi)) void msDealloc(void*, void* p) { appAlloc.dealloc(appAlloc.pUserData, p); }
const ffxAllocationCallbacks* bridgeAlloc(const ffxAllocationCallbacks* cb) {
    static ffxAllocationCallbacks ms;
    if (!cb) return nullptr;
    appAlloc = *cb;
    ms.pUserData = nullptr;
    ms.alloc = reinterpret_cast<ffxAlloc>(&msAlloc);
    ms.dealloc = reinterpret_cast<ffxDealloc>(&msDealloc);
    return &ms;
}

// each Linux function pointer in a description chain swapped for its bridge, put back after
struct Swaps {
    std::vector<std::pair<void**, void*>> undo;
    void swap(void** at, void* to) {
        undo.push_back({at, *at});
        *at = to;
    }
    void deviceContext(VkDeviceContext* d) {
        if (d && d->vkDeviceProcAddr)
            swap(reinterpret_cast<void**>(&d->vkDeviceProcAddr), reinterpret_cast<void*>(ffxvk::msGetDeviceProcAddrFor(d->vkDeviceProcAddr)));
    }
    void chain(ffxApiHeader* h) {
        for (; h; h = h->pNext) {
            switch (h->type) {
            case FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK:
                deviceContext(&reinterpret_cast<ffxCreateBackendVKDesc*>(h)->deviceContext);
                break;
            case FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK_DEPRECATED: {
                auto* d = reinterpret_cast<ffxCreateBackendVKDesc_DEPRECATED*>(h);
                if (d->vkDeviceProcAddr)
                    swap(reinterpret_cast<void**>(&d->vkDeviceProcAddr), reinterpret_cast<void*>(ffxvk::msGetDeviceProcAddrFor(d->vkDeviceProcAddr)));
                break;
            }
            case FFX_API_QUERY_DESC_TYPE_GET_VERSIONS:
                deviceContext(static_cast<VkDeviceContext*>(reinterpret_cast<ffxQueryDescGetVersions*>(h)->device));
                break;
            case FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE: {
                auto* d = reinterpret_cast<ffxCreateContextDescUpscale*>(h);
                if (d->fpMessage) appMessage = d->fpMessage, swap(reinterpret_cast<void**>(&d->fpMessage), reinterpret_cast<void*>(&msMessage));
                break;
            }
            case FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG: {
                auto* d = reinterpret_cast<ffxConfigureDescGlobalDebug*>(h);
                if (d->fpMessage) appMessage = d->fpMessage, swap(reinterpret_cast<void**>(&d->fpMessage), reinterpret_cast<void*>(&msMessage));
                break;
            }
            case FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG1: {
                auto* d = reinterpret_cast<ffxConfigureDescGlobalDebug1*>(h);
                if (d->fpMessage) appMessage = d->fpMessage, swap(reinterpret_cast<void**>(&d->fpMessage), reinterpret_cast<void*>(&msMessage));
                break;
            }
            default:
                break;
            }
        }
    }
    ~Swaps() {
        for (auto it = undo.rbegin(); it != undo.rend(); ++it) *it->first = it->second;
    }
};

bool ready() {
    if (entries.createContext) return true;
    const char* dir = std::getenv("FFXVK_DLL_DIR"); // (unloaded: the directory this names, if any)
    return dir && ffxvkLoad(dir) == 0;
}

} // namespace

extern "C" {

int ffxvkLoad(const char* directory) {
    std::lock_guard<std::mutex> lock(loadMutex);
    if (entries.createContext) return 0;
    if (!directory) return error = "no directory given", 1;
    ffxvk::setDllDirectory(directory);
    ffxvk::ensureThread();
    void* dll = nullptr;
    for (const char* name : {"amd_fidelityfx_loader_vk.dll", "amd_fidelityfx_upscaler_vk.dll"}) {
        std::error_code ec;
        if (!std::filesystem::exists(std::filesystem::path(directory) / name, ec)) continue;
        if ((dll = ffxvk::loadDll(name))) break;
    }
    if (!dll) return error = std::string("AMD's Vulkan DLLs aren't loadable from ") + directory, 2;
    ffxvk::PeImage* img = nullptr;
    for (ffxvk::PeImage* i : ffxvk::loadedImages())
        if (i->base == dll) img = i;
    entries.createContext = reinterpret_cast<MsCreateContext>(img->exportOf("ffxCreateContext"));
    entries.destroyContext = reinterpret_cast<MsDestroyContext>(img->exportOf("ffxDestroyContext"));
    entries.configure = reinterpret_cast<MsConfigure>(img->exportOf("ffxConfigure"));
    entries.query = reinterpret_cast<MsQuery>(img->exportOf("ffxQuery"));
    entries.dispatch = reinterpret_cast<MsDispatch>(img->exportOf("ffxDispatch"));
    if (!entries.createContext || !entries.destroyContext || !entries.configure || !entries.query || !entries.dispatch) {
        entries = {};
        return error = img->path + " doesn't export AMD's FSR API", 3;
    }
    return 0;
}

const char* ffxvkError(void) { return error.c_str(); }

ffxReturnCode_t ffxCreateContext(ffxContext* context, ffxCreateContextDescHeader* desc, const ffxAllocationCallbacks* memCb) {
    if (!ready()) return FFX_API_RETURN_ERROR;
    ffxvk::ensureThread();
    Swaps s;
    s.chain(desc);
    return entries.createContext(context, desc, bridgeAlloc(memCb));
}

ffxReturnCode_t ffxDestroyContext(ffxContext* context, const ffxAllocationCallbacks* memCb) {
    if (!ready()) return FFX_API_RETURN_ERROR;
    ffxvk::ensureThread();
    return entries.destroyContext(context, bridgeAlloc(memCb));
}

ffxReturnCode_t ffxConfigure(ffxContext* context, const ffxConfigureDescHeader* desc) {
    if (!ready()) return FFX_API_RETURN_ERROR;
    ffxvk::ensureThread();
    Swaps s;
    s.chain(const_cast<ffxConfigureDescHeader*>(desc));
    return entries.configure(context, desc);
}

ffxReturnCode_t ffxQuery(ffxContext* context, ffxQueryDescHeader* desc) {
    if (!ready()) return FFX_API_RETURN_ERROR;
    ffxvk::ensureThread();
    Swaps s;
    s.chain(desc);
    return entries.query(context, desc);
}

ffxReturnCode_t ffxDispatch(ffxContext* context, const ffxDispatchDescHeader* desc) {
    if (!ready()) return FFX_API_RETURN_ERROR;
    ffxvk::ensureThread();
    Swaps s;
    s.chain(const_cast<ffxDispatchDescHeader*>(desc));
    return entries.dispatch(context, desc);
}

} // extern "C"
