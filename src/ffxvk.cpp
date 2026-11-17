// AMD's FSR API exported to Linux code: each call entered on a thread with its TEB, the Linux
// function pointers in its descriptions swapped for bridged ones (restored after), and passed to
// the DLL's own entry point (Microsoft x64 calling convention).
#include "ffx_vk_linux.h"

#include "pe.h"
#include "teb.h"
#include "vk_thunks.h"
#include "win32.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

// ---- FSR 4's model (ffxvkFsr4Model): 0 the GPU's own, 1 FSR 4 Lite
// The driver DLL settles on a model the first time it is asked which GPU it runs on, so each model has its own instance
// of the DLL (the second one loaded when its model is first asked for), told its own GPU family (adapter.cpp). Without
// the driver DLL, the SDK's upscaler DLL supplies FSR 4 itself: it asks which GPU it runs on per adapter key and holds
// both models, so Lite's questions get an adapter key and a GPU family of their own (adapter.cpp) and one instance serves.
std::atomic<int> wantedModel{0};
bool modelChosen = false;        // by ffxvkFsr4Model (else $FFXVK_FSR4_MODEL when the DLLs load)
std::string driverFile;          // the driver DLL (absolute; empty: none)
bool driverLoaded[2] = {};

int modelNamed(const char* name) { return name && std::strcmp(name, "lite") == 0 ? 1 : 0; }

// the driver DLL's instance for `model` (loadMutex held); false: `error` says why
bool loadDriver(int model) {
    if (driverLoaded[model]) return true;
    if (driverFile.empty()) return error = "no driver DLL (amdvkffx64.dll): no FSR 4", false;
    ffxvk::setModel(model);   // (the instance's first question about the GPU may come as it starts)
    void* module = driverLoaded[!model] ? ffxvk::loadDllInstance(driverFile) : ffxvk::loadDll(driverFile);
    ffxvk::setModel(0);
    void* update = nullptr;
    for (auto* image : ffxvk::loadedImages())
        if (image->base == module) update = image->exportOf("UpdateFfxApiProvider");
    if (!update) return error = "cannot load the driver provider from " + driverFile, false;
    ffxvk::setDriverProvider(model, update);
    driverLoaded[model] = true;
    return true;
}

// the thread's model for the length of one call into the DLLs
struct ModelScope {
    explicit ModelScope(int model) { ffxvk::setModel(model); }
    ~ModelScope() { ffxvk::setModel(0); }
};

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

// ---- which upscaler a context is (for vk_thunks' per-version changes and FSR 4's model)
// The context a description chain will make: the version it names (ffxOverrideVersion), else the SDK's first choice
// (the first of its version list), as the major version of its name ("4.1.2": 4). 0: not an upscaler, or unknown.
// `version`: the id the SDK must be asked for instead (0: as the program asked) - with both instances of the driver DLL
// in use the SDK lists FSR 4 twice, the second instance's under an id of its own (vk_thunks.cpp, msUpdateProvider), and
// would take whichever comes first.
struct Upscaler { int major = 0; VkPhysicalDevice device = VK_NULL_HANDLE; int model = 0; uint64_t version = 0; };
std::mutex upscalerMutex;
std::vector<std::pair<ffxContext, Upscaler>> upscalers;   // the live contexts

// the second instance's version as the SDK lists it (the driver's own bit 31 aside)
bool isSecond(uint64_t id) {
    const uint64_t second = ffxvk::secondProviderId();
    return second && (id | 0x80000000ull) == (second | 0x80000000ull);
}
// what the program is told in its place: the first instance's FSR 4 (the same version; set when a context is made)
uint64_t firstVersionId = 0;
const char* firstVersionName = nullptr;

// the SDK's versions for a kind of context, the second instance's included (false: the query failed)
struct Versions { uint64_t ids[32]{}; const char* names[32]{}; uint64_t count = 0; };
bool versionsOf(const VkDeviceContext* device, uint64_t createDescType, Versions& out) {
    VkDeviceContext copy{};   // (its function pointer is swapped for the query, the program's left alone)
    if (device) copy = *device;
    ffxQueryDescGetVersions v{};
    v.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    v.createDescType = createDescType;
    v.device = device ? &copy : nullptr;
    out.count = 32;
    v.outputCount = &out.count;
    v.versionIds = out.ids;
    v.versionNames = out.names;
    Swaps s;
    s.chain(&v.header);
    return entries.query(nullptr, &v.header) == FFX_API_RETURN_OK && out.count <= 32;
}

Upscaler upscalerOf(ffxCreateContextDescHeader* desc, int model) {
    VkDeviceContext* device = nullptr;
    uint64_t wanted = 0;
    bool upscale = false;
    for (ffxApiHeader* h = desc; h; h = h->pNext) {
        if (h->type == FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK) device = &reinterpret_cast<ffxCreateBackendVKDesc*>(h)->deviceContext;
        else if (h->type == FFX_API_DESC_TYPE_OVERRIDE_VERSION) wanted = reinterpret_cast<ffxOverrideVersion*>(h)->versionId;
        else if (h->type == FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE) upscale = true;
    }
    Upscaler u;
    u.model = model;
    if (!upscale || !device || !device->vkPhysicalDevice) return u;
    Versions v;
    if (!versionsOf(device, FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, v) || !v.count) return u;
    // the program's choice among the versions it knows of (the second instance's isn't one)
    uint64_t id = 0, second = 0;
    const char* name = nullptr;
    for (uint64_t i = 0; i < v.count; ++i) {
        if (isSecond(v.ids[i])) { second = v.ids[i]; continue; }
        if (!name && (!wanted || v.ids[i] == wanted)) id = v.ids[i], name = v.names[i];
    }
    u.major = name ? std::atoi(name) : 0;
    u.device = device->vkPhysicalDevice;
    if (u.major == 4 && second) {
        firstVersionId = id;
        firstVersionName = name;
        u.version = model == ffxvk::firstDriverModel() ? id : second;
    }
    return u;
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
    if (!modelChosen) wantedModel = modelNamed(std::getenv("FFXVK_FSR4_MODEL"));
    if (driverPath || std::filesystem::exists(driver, ec)) {
        driverFile = std::filesystem::absolute(driver).string();
        if (!loadDriver(wantedModel)) {
            entries = {};
            driverFile.clear();
            return 4;
        }
    }
    return 0;
}

const char* ffxvkError(void) { return error.c_str(); }

int ffxvkFsr4Model(const char* model) {
    std::lock_guard<std::mutex> lock(loadMutex);
    const int m = modelNamed(model);
    if (entries.createContext && !driverFile.empty()) {   // (before the DLLs load: remembered for then; without the
        ffxvk::ensureThread();                             // driver DLL, the SDK's DLL holds both models)
        if (!loadDriver(m)) return 1;
    }
    modelChosen = true;
    wantedModel = m;
    return 0;
}

ffxReturnCode_t ffxCreateContext(ffxContext* context, ffxCreateContextDescHeader* desc, const ffxAllocationCallbacks* memCb) {
    if (!ready()) return FFX_API_RETURN_ERROR;
    ffxvk::ensureThread();
    const int model = wantedModel;
    ModelScope modelScope(model);
    const Upscaler upscaler = upscalerOf(desc, model);
    UpscalerScope scope(upscaler);
    // the version named for the SDK where it must be (see Upscaler): in the program's own override, else in one of ours
    // put behind the first description; both as they were after the call
    ffxOverrideVersion ours{};
    ffxOverrideVersion* theirs = nullptr;
    uint64_t theirVersion = 0;
    if (upscaler.version && desc) {
        for (ffxApiHeader* h = desc; h && !theirs; h = h->pNext)
            if (h->type == FFX_API_DESC_TYPE_OVERRIDE_VERSION) theirs = reinterpret_cast<ffxOverrideVersion*>(h);
        if (theirs) {
            theirVersion = theirs->versionId;
            theirs->versionId = upscaler.version;
        } else {
            ours.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
            ours.header.pNext = desc->pNext;
            ours.versionId = upscaler.version;
            desc->pNext = &ours.header;
        }
    }
    ffxReturnCode_t result;
    {
        Swaps s;
        s.chain(desc);
        result = entries.createContext(context, desc, bridgeAlloc(memCb));
    }
    if (theirs) theirs->versionId = theirVersion;
    else if (ours.header.type) desc->pNext = ours.header.pNext;
    if (result == FFX_API_RETURN_OK && context && (upscaler.major || upscaler.model)) {
        std::lock_guard<std::mutex> lock(upscalerMutex);
        upscalers.push_back({*context, upscaler});
    }
    return result;
}

ffxReturnCode_t ffxDestroyContext(ffxContext* context, const ffxAllocationCallbacks* memCb) {
    if (!ready()) return FFX_API_RETURN_ERROR;
    ffxvk::ensureThread();
    ModelScope modelScope(upscalerOf(context).model);
    if (context) {
        std::lock_guard<std::mutex> lock(upscalerMutex);
        for (auto it = upscalers.begin(); it != upscalers.end(); ++it) if (it->first == *context) { upscalers.erase(it); break; }
    }
    return entries.destroyContext(context, bridgeAlloc(memCb));
}

ffxReturnCode_t ffxConfigure(ffxContext* context, const ffxConfigureDescHeader* desc) {
    if (!ready()) return FFX_API_RETURN_ERROR;
    ffxvk::ensureThread();
    ModelScope modelScope(context && *context ? upscalerOf(context).model : wantedModel.load());
    Swaps s;
    s.chain(const_cast<ffxConfigureDescHeader*>(desc));
    return entries.configure(context, desc);
}

ffxReturnCode_t ffxQuery(ffxContext* context, ffxQueryDescHeader* desc) {
    if (!ready()) return FFX_API_RETURN_ERROR;
    ffxvk::ensureThread();
    ModelScope modelScope(context && *context ? upscalerOf(context).model : wantedModel.load());
    // The version list: the second instance's FSR 4 left out (the program picks the model with ffxvkFsr4Model, not by
    // version). Asked for here with room for all, as the program's own count is one short of the SDK's.
    if (desc && desc->type == FFX_API_QUERY_DESC_TYPE_GET_VERSIONS && ffxvk::firstDriverModel() >= 0 && driverLoaded[!ffxvk::firstDriverModel()]) {
        auto* q = reinterpret_cast<ffxQueryDescGetVersions*>(desc);
        Versions v;
        if (q->outputCount && versionsOf(static_cast<const VkDeviceContext*>(q->device), q->createDescType, v)) {
            const uint64_t room = q->versionIds || q->versionNames ? *q->outputCount : 0;
            uint64_t n = 0;
            for (uint64_t i = 0; i < v.count; ++i) {
                if (isSecond(v.ids[i])) continue;
                if (n < room) {
                    if (q->versionIds) q->versionIds[n] = v.ids[i];
                    if (q->versionNames) q->versionNames[n] = v.names[i];
                }
                ++n;
            }
            *q->outputCount = q->versionIds || q->versionNames ? std::min(n, room) : n;
            return FFX_API_RETURN_OK;
        }
    }
    ffxReturnCode_t result;
    {
        Swaps s;
        s.chain(desc);
        result = entries.query(context, desc);
    }
    // a context of the second instance: the version the program knows it by
    if (result == FFX_API_RETURN_OK && desc && desc->type == FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION) {
        auto* q = reinterpret_cast<ffxQueryGetProviderVersion*>(desc);
        if (isSecond(q->versionId) && firstVersionId) q->versionId = firstVersionId, q->versionName = firstVersionName;
    }
    return result;
}

ffxReturnCode_t ffxDispatch(ffxContext* context, const ffxDispatchDescHeader* desc) {
    if (!ready()) return FFX_API_RETURN_ERROR;
    ffxvk::ensureThread();
    const Upscaler upscaler = upscalerOf(context);
    ModelScope modelScope(upscaler.model);
    UpscalerScope scope(upscaler);   // (images the DLL makes on a first dispatch)
    Swaps s;
    s.chain(const_cast<ffxDispatchDescHeader*>(desc));
    return entries.dispatch(context, desc);
}

} // extern "C"
