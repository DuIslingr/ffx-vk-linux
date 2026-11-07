#include "adapter.h"
#include <amdgpu.h>
#include <xf86drm.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ffxvk {
namespace {
struct Adapter { uint64_t luid = 0; uint32_t family = 0, revision = 0; bool valid = false; };
thread_local Adapter selected;
struct Luid { uint32_t low; int32_t high; };
struct AdapterInfo { uint32_t handle; Luid luid; uint32_t sources; int32_t preciseRegions; };
struct EnumAdapters { uint32_t count; AdapterInfo adapters[16]; };
struct QueryAdapterInfo { uint32_t handle, type; void* data; uint32_t size; };
static_assert(sizeof(EnumAdapters) == 0x144);
static_assert(offsetof(QueryAdapterInfo, data) == 8);
constexpr int32_t invalid = int32_t(0xc000000du);
__attribute__((ms_abi)) int32_t enumerate(EnumAdapters* out) {
    if (!out) return invalid;
    std::memset(out, 0, sizeof(*out));
    if (selected.valid) {
        out->count = 1;
        out->adapters[0].handle = 1;
        std::memcpy(&out->adapters[0].luid, &selected.luid, 8);
    }
    return 0;
}
__attribute__((ms_abi)) int32_t queryAdapter(QueryAdapterInfo* query) {
    if (!query || !selected.valid || query->handle != 1 || query->type != 0 || !query->data || query->size < 0x260) return invalid;
    // AMD's legacy KMTQAITYPE_UMDRIVERPRIVATE request reads ASIC family and
    // external revision at these offsets. These are real kernel-reported values.
    std::memset(query->data, 0, query->size);
    std::memcpy(static_cast<char*>(query->data) + 48, &selected.family, 4);
    std::memcpy(static_cast<char*>(query->data) + 52, &selected.revision, 4);
    return 0;
}
}
void identifyDriverAdapter(VkPhysicalDevice physical, PFN_vkGetPhysicalDeviceProperties2 query,
                          VkPhysicalDeviceProperties2* properties) {
    VkPhysicalDeviceIDProperties* id = nullptr;
    for (auto* h = static_cast<VkBaseOutStructure*>(properties->pNext); h; h = h->pNext)
        if (h->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES) id = reinterpret_cast<VkPhysicalDeviceIDProperties*>(h);
    if (!id) return;
    selected = {};
    uint64_t key = 14695981039346656037ull;
    for (auto byte : id->deviceUUID) { key ^= byte; key *= 1099511628211ull; }
    // Only the DLL-facing properties receive a Windows-style adapter key.
    if (!id->deviceLUIDValid) {
        std::memcpy(id->deviceLUID, &key, VK_LUID_SIZE);
        id->deviceNodeMask = 1;
        id->deviceLUIDValid = VK_TRUE;
    }
    std::memcpy(&selected.luid, id->deviceLUID, 8);
    if (properties->properties.vendorID != 0x1002) return;
    VkPhysicalDeviceDrmPropertiesEXT drm{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 native{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &drm};
    query(physical, &native);
    if (!drm.hasRender) return;
    std::array<drmDevicePtr, 16> devices{};
    const int count = drmGetDevices2(0, devices.data(), devices.size());
    for (int i = 0; i < count; ++i) {
        auto* d = devices[i];
        if (!(d->available_nodes & (1 << DRM_NODE_RENDER))) continue;
        struct stat st{};
        const char* path = d->nodes[DRM_NODE_RENDER];
        if (stat(path, &st) || major(st.st_rdev) != drm.renderMajor || minor(st.st_rdev) != drm.renderMinor) continue;
        const int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        amdgpu_device_handle device = nullptr;
        uint32_t majorVersion = 0, minorVersion = 0;
        if (amdgpu_device_initialize(fd, &majorVersion, &minorVersion, &device) == 0) {
            amdgpu_gpu_info info{};
            if (amdgpu_query_gpu_info(device, &info) == 0) {
                selected.family = info.family_id;
                selected.revision = info.chip_external_rev;
                selected.valid = true;
                if (std::getenv("FFXVK_DEBUG")) std::fprintf(stderr, "ffx-vk-linux: DRM adapter %s family %x revision %x\n", path, selected.family, selected.revision);
            }
            amdgpu_device_deinitialize(device);
        }
        close(fd);
        break;
    }
    if (count > 0) drmFreeDevices(devices.data(), count);
}
void* adapterStandIn(const std::string& dll, const std::string& name) {
    if (dll != "gdi32.dll") return nullptr;
    if (name == "D3DKMTEnumAdapters") return reinterpret_cast<void*>(&enumerate);
    if (name == "D3DKMTQueryAdapterInfo") return reinterpret_cast<void*>(&queryAdapter);
    return nullptr;
}
}
