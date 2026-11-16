// AMD's FSR API exported to Linux code: each call entered on a thread with its TEB, the Linux
// function pointers in its descriptions swapped for bridged ones (restored after), and passed to
// the DLL's own entry point (Microsoft x64 calling convention).
#include "ffx_vk_linux.h"

#include "pe.h"
#include "teb.h"
#include "vk_thunks.h"
#include "win32.h"

#include <cstdio>
#include <cstdlib>
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

// ---- which upscaler a context is (for vk_thunks' per-version changes)
// The context a description chain will make: the version it names (ffxOverrideVersion), else the SDK's first choice
// (the first of its version list), as the major version of its name ("4.1.2": 4). 0: not an upscaler, or unknown.
struct Upscaler { int major = 0; VkPhysicalDevice device = VK_NULL_HANDLE; };
std::mutex upscalerMutex;
std::vector<std::pair<ffxContext, Upscaler>> upscalers;   // the live contexts

Upscaler upscalerOf(ffxCreateContextDescHeader* desc) {
    VkDeviceContext* device = nullptr;
    uint64_t wanted = 0;
    bool upscale = false;
    for (ffxApiHeader* h = desc; h; h = h->pNext) {
        if (h->type == FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK) device = &reinterpret_cast<ffxCreateBackendVKDesc*>(h)->deviceContext;
        else if (h->type == FFX_API_DESC_TYPE_OVERRIDE_VERSION) wanted = reinterpret_cast<ffxOverrideVersion*>(h)->versionId;
        else if (h->type == FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE) upscale = true;
    }
    if (!upscale || !device || !device->vkPhysicalDevice) return {};
    uint64_t ids[16]{};
    const char* names[16]{};
    uint64_t count = 16;
    VkDeviceContext copy = *device;   // (its function pointer is swapped for the query, the program's left alone)
    ffxQueryDescGetVersions v{};
    v.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    v.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    v.device = &copy;
    v.outputCount = &count;
    v.versionIds = ids;
    v.versionNames = names;
    {
        Swaps s;
        s.chain(&v.header);
        if (entries.query(nullptr, &v.header) != FFX_API_RETURN_OK || !count || count > 16) return {};
    }
    const char* name = names[0];
    if (wanted) {
        name = nullptr;
        for (uint64_t i = 0; i < count; ++i) if (ids[i] == wanted) name = names[i];
    }
    return {name ? std::atoi(name) : 0, device->vkPhysicalDevice};
}

// the thread's upscaler for the length of one call into the DLL
struct UpscalerScope {
    explicit UpscalerScope(const Upscaler& u) { ffxvk::setUpscaler(u.major, u.device); }
    ~UpscalerScope() { ffxvk::setUpscaler(0, VK_NULL_HANDLE); }
};

Upscaler upscalerOf(ffxContext* context) {
    if (!context) return {};
    std::lock_guard<std::mutex> lock(upscalerMutex);
    for (const auto& [c, u] : upscalers) if (c == *context) return u;
    return {};
}

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
    // Keep the SDK in charge of provider enumeration, support checks and context routing.
    // The optional Windows driver component supplies FSR 4 through the SDK's own hook.
    const char* driverPath = std::getenv("FFXVK_DRIVER_DLL");
    std::filesystem::path driver = driverPath ? std::filesystem::path(driverPath)
                                            : std::filesystem::path(directory) / "amdvkffx64.dll";
    std::error_code ec;
    if (driverPath || std::filesystem::exists(driver, ec)) {
        void* module = ffxvk::loadDll(std::filesystem::absolute(driver).string());
        void* update = nullptr;
        for (auto* image : ffxvk::loadedImages())
            if (image->base == module) update = image->exportOf("UpdateFfxApiProvider");
        if (!update) {
            entries = {};
            return error = "cannot load the driver provider from " + driver.string(), 4;
        }
        ffxvk::setDriverProvider(update);
    }
    return 0;
}

const char* ffxvkError(void) { return error.c_str(); }

ffxReturnCode_t ffxCreateContext(ffxContext* context, ffxCreateContextDescHeader* desc, const ffxAllocationCallbacks* memCb) {
    if (!ready()) return FFX_API_RETURN_ERROR;
    ffxvk::ensureThread();
    const Upscaler upscaler = upscalerOf(desc);
    UpscalerScope scope(upscaler);
    Swaps s;
    s.chain(desc);
    const ffxReturnCode_t result = entries.createContext(context, desc, bridgeAlloc(memCb));
    if (result == FFX_API_RETURN_OK && context && upscaler.major) {
        std::lock_guard<std::mutex> lock(upscalerMutex);
        upscalers.push_back({*context, upscaler});
    }
    return result;
}

ffxReturnCode_t ffxDestroyContext(ffxContext* context, const ffxAllocationCallbacks* memCb) {
    if (!ready()) return FFX_API_RETURN_ERROR;
    ffxvk::ensureThread();
    if (context) {
        std::lock_guard<std::mutex> lock(upscalerMutex);
        for (auto it = upscalers.begin(); it != upscalers.end(); ++it) if (it->first == *context) { upscalers.erase(it); break; }
    }
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
    UpscalerScope scope(upscalerOf(context));   // (images the DLL makes on a first dispatch)
    Swaps s;
    s.chain(const_cast<ffxDispatchDescHeader*>(desc));
    return entries.dispatch(context, desc);
}

} // extern "C"
