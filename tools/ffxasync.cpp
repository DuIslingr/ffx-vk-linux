// ffxasync: what running FSR on a compute-only queue (asynchronous compute) gains over running it on the graphics queue,
// as a function of how much other GPU work a frame has.
//
// Each frame: a load (ffxasync_load.comp: a stand-in for a game's rendering, `iterations` rounds of arithmetic per
// render-resolution pixel) writes the colour image, FSR upscales it, and a light "post" pass touches the output (a stand-in
// for post-processing and the interface after the upscale).
//   serial: load(i), FSR(i), post(i) in order on the graphics queue - how programs run FSR today.
//   async:  FSR(i) on a compute-only queue while the graphics queue goes on with load(i+1); post(i), queued after load(i+1),
//           waits for FSR(i).
//           Inputs and outputs are double-buffered, the images shared by both queue families (VK_SHARING_MODE_CONCURRENT),
//           the order kept by timeline semaphores. A frame of latency more than serial.
// Prints, for each load: the load alone, FSR alone, serial and async milliseconds per frame (GPU-bound throughput over
// `frames` frames), and the gain.
//
//   ffxasync <AMD's DLL directory> [--size 3840x2160] [--ratio 1.5] [--frames 300] [--loads 0.5,1,2,4,8] [--fsr 3|4]
//            [--output rgba16f|rgba8|r11g11b10]   the output image's format (rgba16f: an HDR pipeline's; FSR 4's last pass
//                                                  costs more with it on RADV unless FSR's own images are linear)
#include "ffx_vk_linux.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

const uint32_t kLoadShader[] = {
#include "ffxasync_load.inc"
};
const uint32_t kRasterShader[] = {
#include "ffxasync_raster.inc"
};

void checked(VkResult r, const char* what) {
    if (r != VK_SUCCESS) throw std::runtime_error(std::string(what) + ": Vulkan error " + std::to_string(r));
}

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    FfxApiResource resource(uint32_t state) const {
        return ffxApiGetResourceVK(reinterpret_cast<void*>(image), ffxApiGetImageResourceDescriptionVK(image, info, 0), state);
    }
};

struct Bench {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t graphicsFamily = 0, computeFamily = 0;
    VkQueue graphics = VK_NULL_HANDLE, compute = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory{};
    uint32_t renderW = 0, renderH = 0, outW = 0, outH = 0;
    std::array<Image, 2> colour, depth, motion, output;
    VkCommandPool graphicsPool = VK_NULL_HANDLE, computePool = VK_NULL_HANDLE;
    std::array<VkCommandBuffer, 2> loadCmd{}, postCmd{}, fsrCmd{}, serialCmd{};
    VkSemaphore loaded = VK_NULL_HANDLE, upscaled = VK_NULL_HANDLE, posted = VK_NULL_HANDLE;   // timeline: frame numbers
    uint64_t frame = 0;   // frames submitted so far (the semaphores' values continue across runs)
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, 2> colourSet{}, outputSet{};
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline load = VK_NULL_HANDLE;
    ffxContext context = nullptr;
    // --load raster: the frame's work a depth-only pass (full-screen triangles, `iterations` of them) - fill-bound, the shader
    // cores mostly free - besides one light compute pass writing the colour; --load compute: `iterations` rounds of arithmetic
    // per pixel, every shader core busy
    bool raster = true;
    Image sceneDepth;
    VkPipelineLayout rasterLayout = VK_NULL_HANDLE;
    VkPipeline rasterPipeline = VK_NULL_HANDLE;

    uint32_t memoryType(uint32_t mask, VkMemoryPropertyFlags flags) {
        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
            if ((mask & (1u << i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags) return i;
        throw std::runtime_error("no memory type");
    }

    void makeImage(Image& image, VkFormat format, uint32_t w, uint32_t h, VkImageUsageFlags usage, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
        const uint32_t families[] = {graphicsFamily, computeFamily};
        image.info.imageType = VK_IMAGE_TYPE_2D;
        image.info.format = format;
        image.info.extent = {w, h, 1};
        image.info.mipLevels = 1;
        image.info.arrayLayers = 1;
        image.info.samples = VK_SAMPLE_COUNT_1_BIT;
        image.info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image.info.usage = usage;
        image.info.sharingMode = VK_SHARING_MODE_CONCURRENT;   // (both queue families: no ownership transfers)
        image.info.queueFamilyIndexCount = 2;
        image.info.pQueueFamilyIndices = families;
        checked(vkCreateImage(device, &image.info, nullptr, &image.image), "vkCreateImage");
        image.info.queueFamilyIndexCount = 0, image.info.pQueueFamilyIndices = nullptr;
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(device, image.image, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        checked(vkAllocateMemory(device, &ai, nullptr, &image.memory), "vkAllocateMemory");
        checked(vkBindImageMemory(device, image.image, image.memory, 0), "vkBindImageMemory");
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = image.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format;
        vi.subresourceRange = {aspect, 0, 1, 0, 1};
        checked(vkCreateImageView(device, &vi, nullptr, &image.view), "vkCreateImageView");
    }

    static void barrier(VkCommandBuffer cmd, const Image& image, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst,
                        VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = src, b.dstAccessMask = dst, b.oldLayout = from, b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image.image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
    }

    static void begin(VkCommandBuffer cmd) {
        checked(vkResetCommandBuffer(cmd, 0), "vkResetCommandBuffer");
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        checked(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer");
    }

    void submit(VkQueue queue, VkCommandBuffer cmd, VkSemaphore wait1, uint64_t value1, VkSemaphore wait2, uint64_t value2,
                VkSemaphore signal, uint64_t signalValue) {
        checked(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");
        VkSemaphore waits[2];
        uint64_t waitValues[2];
        VkPipelineStageFlags stages[2] = {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT};
        uint32_t n = 0;
        if (wait1) waits[n] = wait1, waitValues[n++] = value1;
        if (wait2) waits[n] = wait2, waitValues[n++] = value2;
        VkTimelineSemaphoreSubmitInfo ti{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        ti.waitSemaphoreValueCount = n, ti.pWaitSemaphoreValues = waitValues;
        const uint32_t signals = signal && signalValue ? 1 : 0;   // (value 0: nothing signalled)
        ti.signalSemaphoreValueCount = signals, ti.pSignalSemaphoreValues = &signalValue;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO, &ti};
        si.waitSemaphoreCount = n, si.pWaitSemaphores = waits, si.pWaitDstStageMask = stages;
        si.commandBufferCount = 1, si.pCommandBuffers = &cmd;
        si.signalSemaphoreCount = signals, si.pSignalSemaphores = &signal;
        checked(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit");
    }

    void hostWait(VkSemaphore s, uint64_t value) {
        if (!value) return;
        VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        wi.semaphoreCount = 1, wi.pSemaphores = &s, wi.pValues = &value;
        checked(vkWaitSemaphores(device, &wi, ~0ull), "vkWaitSemaphores");
    }

    void recordLoad(VkCommandBuffer cmd, uint32_t slot, uint32_t iterations, uint32_t groupsX = 0, uint32_t groupsY = 0) {
        // (the colour back to GENERAL for the load: FSR read it in SHADER_READ_ONLY two frames ago)
        barrier(cmd, colour[slot], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT,
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        const bool drawn = raster && !groupsX;
        const uint32_t rounds = drawn ? 1 : iterations;   // (raster: one light compute pass writes the colour)
        if (rounds) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, load);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &colourSet[slot], 0, nullptr);
            vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &rounds);
            vkCmdDispatch(cmd, groupsX ? groupsX : (renderW + 7) / 8, groupsY ? groupsY : (renderH + 7) / 8, 1);
        }
        if (drawn && iterations) {
            VkImageMemoryBarrier d{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            d.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            d.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            d.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, d.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            d.srcQueueFamilyIndex = d.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            d.image = sceneDepth.image;
            d.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
            const VkPipelineStageFlags tests = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
            vkCmdPipelineBarrier(cmd, tests, tests, 0, 0, nullptr, 0, nullptr, 1, &d);
            VkRenderingAttachmentInfo depthAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            depthAttachment.imageView = sceneDepth.view;
            depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
            ri.renderArea = {{0, 0}, {renderW, renderH}};
            ri.layerCount = 1;
            ri.pDepthAttachment = &depthAttachment;
            vkCmdBeginRendering(cmd, &ri);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rasterPipeline);
            VkViewport viewport{0, 0, float(renderW), float(renderH), 0, 1};
            VkRect2D scissor{{0, 0}, {renderW, renderH}};
            vkCmdSetViewport(cmd, 0, 1, &viewport);
            vkCmdSetScissor(cmd, 0, 1, &scissor);
            vkCmdDraw(cmd, 3, iterations, 0, 0);
            vkCmdEndRendering(cmd);
        }
        barrier(cmd, colour[slot], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }

    void recordFsr(VkCommandBuffer cmd, uint32_t slot, bool reset) {
        VkMemoryBarrier m{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        m.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        m.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &m, 0, nullptr, 0, nullptr);
        ffxDispatchDescUpscale d{};
        d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        d.commandList = cmd;
        d.color = colour[slot].resource(FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.depth = depth[slot].resource(FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.motionVectors = motion[slot].resource(FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.output = output[slot].resource(FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        d.renderSize = {renderW, renderH};
        d.upscaleSize = {outW, outH};
        d.motionVectorScale = {float(renderW), float(renderH)};
        d.frameTimeDelta = 1000.0f / 60.0f;
        d.preExposure = 1.0f;
        d.reset = reset;
        d.cameraNear = 0.1f, d.cameraFar = 100.0f, d.cameraFovAngleVertical = 1.0472f, d.viewSpaceToMetersFactor = 1.0f;
        const ffxReturnCode_t r = ffxDispatch(&context, &d.header);
        if (r != FFX_API_RETURN_OK) throw std::runtime_error("ffxDispatch: " + std::to_string(r));
    }

    void recordPost(VkCommandBuffer cmd, uint32_t slot) {
        // (one round of the load's arithmetic over the output: post-processing / the interface)
        VkMemoryBarrier m{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        m.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        m.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &m, 0, nullptr, 0, nullptr);
        const uint32_t one = 1;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, load);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &outputSet[slot], 0, nullptr);
        vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &one);
        vkCmdDispatch(cmd, (outW + 7) / 8, (outH + 7) / 8, 1);
    }

    // milliseconds per frame over `frames` frames (after a few to warm up); `fsr` off: the load alone; `async`: FSR on the
    // compute queue
    double run(uint32_t iterations, uint32_t frames, bool fsr, bool async) {
        const uint32_t warm = 20;
        std::chrono::steady_clock::time_point start;
        const uint64_t first = frame;
        uint64_t pending = 0;   // (async: the frame whose post waits for the next round's load to be queued first)
        auto submitPost = [&](uint64_t f) {
            const uint32_t slot = uint32_t(f & 1);
            begin(postCmd[slot]);
            recordPost(postCmd[slot], slot);
            submit(graphics, postCmd[slot], upscaled, f, nullptr, 0, posted, f);
        };
        for (uint32_t i = 0; i < frames + warm; ++i) {
            if (i == warm) {   // (the warm-up drained: the clock starts with the GPU idle)
                if (pending) submitPost(pending), pending = 0;
                hostWait(posted, frame);
                start = std::chrono::steady_clock::now();
            }
            const uint64_t f = ++frame;
            const uint32_t slot = uint32_t(f & 1);
            // the slot's command buffers and images free: its frame two before posted
            hostWait(posted, f >= 2 ? f - 2 : 0);
            if (!async) {
                VkCommandBuffer cmd = serialCmd[slot];
                begin(cmd);
                recordLoad(cmd, slot, iterations);
                if (fsr) recordFsr(cmd, slot, f == first + 1);
                recordPost(cmd, slot);
                submit(graphics, cmd, nullptr, 0, nullptr, 0, posted, f);   // (two frames in flight, as async)
                continue;
            }
            // graphics: load(f) - its colour image last read by FSR(f-2)
            begin(loadCmd[slot]);
            recordLoad(loadCmd[slot], slot, iterations);
            submit(graphics, loadCmd[slot], upscaled, f >= 2 ? f - 2 : 0, nullptr, 0, loaded, f);
            // compute: FSR(f) after load(f), its output last read by post(f-2)
            begin(fsrCmd[slot]);
            recordFsr(fsrCmd[slot], slot, f == first + 1);
            submit(compute, fsrCmd[slot], loaded, f, posted, f >= 2 ? f - 2 : 0, upscaled, f);
            // graphics: post(f-1), queued after load(f) - a queue runs its submissions in order, so the graphics queue does
            // load(f) while the compute queue finishes FSR(f-1), then post(f-1); FSR(f) overlaps post(f-1) and load(f+1)
            if (pending) submitPost(pending);
            pending = f;
        }
        if (pending) submitPost(pending);
        hostWait(posted, frame);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        if (!async)   // (the other two semaphores brought to this frame's number, for a later async run's waits)
            for (VkSemaphore s : {loaded, upscaled}) {
                VkSemaphoreSignalInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, nullptr, s, frame};
                checked(vkSignalSemaphore(device, &si), "vkSignalSemaphore");
            }
        return ms / frames;
    }

    // --concurrency: whether the two queues' work runs at the same time at all - a narrow load (8x8 workgroups: a few % of
    // the GPU, long-running, as FSR's small passes) twice per frame, both on the graphics queue (one after the other) or one
    // on each queue (no dependency between them). Milliseconds per frame.
    double concurrency(uint32_t iterations, uint32_t frames, bool twoQueues) {
        std::chrono::steady_clock::time_point start;
        for (uint32_t i = 0; i < frames + 20; ++i) {
            if (i == 20) hostWait(posted, frame), hostWait(upscaled, frame), start = std::chrono::steady_clock::now();
            const uint64_t f = ++frame;
            const uint32_t slot = uint32_t(f & 1);
            hostWait(posted, f >= 2 ? f - 2 : 0);
            hostWait(upscaled, f >= 2 ? f - 2 : 0);
            begin(loadCmd[slot]);
            recordLoad(loadCmd[slot], 0, iterations, 8, 8);
            if (!twoQueues) recordLoad(loadCmd[slot], 1, iterations, 8, 8);
            submit(graphics, loadCmd[slot], nullptr, 0, nullptr, 0, posted, f);
            begin(fsrCmd[slot]);
            if (twoQueues) recordLoad(fsrCmd[slot], 1, iterations, 8, 8);
            submit(compute, fsrCmd[slot], nullptr, 0, nullptr, 0, upscaled, f);
        }
        hostWait(posted, frame), hostWait(upscaled, frame);
        VkSemaphoreSignalInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, nullptr, loaded, frame};
        checked(vkSignalSemaphore(device, &si), "vkSignalSemaphore");
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / frames;
    }
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "ffxasync <SDK DLL directory> [--size WxH] [--ratio 1.5] [--frames 300] [--loads 0.5,1,2,4,8] [--fsr 3|4] [--output rgba16f|rgba8|r11g11b10]\n");
        return 2;
    }
    uint32_t outW = 3840, outH = 2160, frames = 300;
    float ratio = 1.5f;
    int fsrMajor = 0;
    bool concurrencyTest = false, rasterLoad = true;
    VkFormat outputFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    std::vector<double> loads = {0.5, 1, 2, 4, 8};
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        const char* next = i + 1 < argc ? argv[i + 1] : nullptr;
        if (a == "--size" && next) std::sscanf(next, "%ux%u", &outW, &outH), ++i;
        else if (a == "--ratio" && next) ratio = float(std::atof(next)), ++i;
        else if (a == "--frames" && next) frames = uint32_t(std::atoi(next)), ++i;
        else if (a == "--fsr" && next) fsrMajor = std::atoi(next), ++i;
        else if (a == "--concurrency") concurrencyTest = true;
        else if (a == "--output" && next) {
            const std::string f = next;
            outputFormat = f == "rgba8" ? VK_FORMAT_R8G8B8A8_UNORM : f == "r11g11b10" ? VK_FORMAT_B10G11R11_UFLOAT_PACK32 : VK_FORMAT_R16G16B16A16_SFLOAT;
            ++i;
        }
        else if (a == "--load" && next) rasterLoad = std::string(next) != "compute", ++i;
        else if (a == "--loads" && next) {
            loads.clear();
            for (const char* p = next; *p;) { loads.push_back(std::atof(p)); while (*p && *p != ',') ++p; if (*p) ++p; }
            ++i;
        }
    }
    Bench b;
    try {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &app;
        checked(vkCreateInstance(&ici, nullptr, &b.instance), "vkCreateInstance");
        uint32_t n = 0;
        vkEnumeratePhysicalDevices(b.instance, &n, nullptr);
        std::vector<VkPhysicalDevice> gpus(n);
        vkEnumeratePhysicalDevices(b.instance, &n, gpus.data());
        for (auto g : gpus) {
            VkPhysicalDeviceProperties p;
            vkGetPhysicalDeviceProperties(g, &p);
            if (p.apiVersion >= VK_API_VERSION_1_3 && p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { b.gpu = g; break; }
        }
        if (!b.gpu && n) b.gpu = gpus[0];
        if (!b.gpu) throw std::runtime_error("no GPU");
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(b.gpu, &props);
        vkGetPhysicalDeviceMemoryProperties(b.gpu, &b.memory);
        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(b.gpu, &nq, nullptr);
        std::vector<VkQueueFamilyProperties> qf(nq);
        vkGetPhysicalDeviceQueueFamilyProperties(b.gpu, &nq, qf.data());
        bool haveGraphics = false, haveCompute = false;
        for (uint32_t f = 0; f < nq; ++f) {
            if (!haveGraphics && (qf[f].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qf[f].queueFlags & VK_QUEUE_COMPUTE_BIT)) b.graphicsFamily = f, haveGraphics = true;
            if (!haveCompute && (qf[f].queueFlags & VK_QUEUE_COMPUTE_BIT) && !(qf[f].queueFlags & VK_QUEUE_GRAPHICS_BIT)) b.computeFamily = f, haveCompute = true;
        }
        if (!haveGraphics || !haveCompute) throw std::runtime_error("needs a graphics and a compute-only queue family");
        // every core feature of 1.1-1.3 the GPU has (as AMD's probe and ffxquery), the FSR extensions where present
        VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
        VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &f12};
        VkPhysicalDeviceFeatures2 feats{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f11};
        uint32_t ne = 0;
        vkEnumerateDeviceExtensionProperties(b.gpu, nullptr, &ne, nullptr);
        std::vector<VkExtensionProperties> exts(ne);
        vkEnumerateDeviceExtensionProperties(b.gpu, nullptr, &ne, exts.data());
        auto has = [&](const char* e) { return std::any_of(exts.begin(), exts.end(), [&](auto& x) { return !std::strcmp(x.extensionName, e); }); };
        std::vector<const char*> extensions;
        VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR derivatives{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_KHR};
        VkPhysicalDeviceCoherentMemoryFeaturesAMD coherent{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COHERENT_MEMORY_FEATURES_AMD};
        if (has(VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME)) extensions.push_back(VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME), derivatives.pNext = f13.pNext, f13.pNext = &derivatives;
        if (has(VK_AMD_DEVICE_COHERENT_MEMORY_EXTENSION_NAME)) extensions.push_back(VK_AMD_DEVICE_COHERENT_MEMORY_EXTENSION_NAME), coherent.pNext = f13.pNext, f13.pNext = &coherent;
        vkGetPhysicalDeviceFeatures2(b.gpu, &feats);
        feats.features.robustBufferAccess = VK_FALSE;
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queues[2] = {{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}, {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}};
        queues[0].queueFamilyIndex = b.graphicsFamily, queues[0].queueCount = 1, queues[0].pQueuePriorities = &priority;
        queues[1].queueFamilyIndex = b.computeFamily, queues[1].queueCount = 1, queues[1].pQueuePriorities = &priority;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &feats};
        dci.queueCreateInfoCount = 2, dci.pQueueCreateInfos = queues;
        dci.enabledExtensionCount = uint32_t(extensions.size()), dci.ppEnabledExtensionNames = extensions.data();
        checked(vkCreateDevice(b.gpu, &dci, nullptr, &b.device), "vkCreateDevice");
        vkGetDeviceQueue(b.device, b.graphicsFamily, 0, &b.graphics);
        vkGetDeviceQueue(b.device, b.computeFamily, 0, &b.compute);

        // AMD's DLLs; the provider: FSR `--fsr` if given, else the newest
        if (ffxvkLoad(argv[1]) != 0) throw std::runtime_error(std::string("ffxvkLoad: ") + ffxvkError());
        VkDeviceContext dc{b.device, b.gpu, vkGetDeviceProcAddr};
        uint64_t count = 0;
        ffxQueryDescGetVersions v{};
        v.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
        v.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        v.device = &dc;
        v.outputCount = &count;
        if (ffxQuery(nullptr, &v.header) != FFX_API_RETURN_OK || !count) throw std::runtime_error("no FSR provider");
        std::vector<uint64_t> ids(count);
        std::vector<const char*> names(count);
        v.versionIds = ids.data(), v.versionNames = names.data();
        ffxQuery(nullptr, &v.header);
        uint64_t chosen = 0;
        const char* chosenName = "?";
        for (uint64_t i = 0; i < count; ++i)
            if (names[i] && (!fsrMajor || names[i][0] == '0' + fsrMajor)) { chosen = ids[i], chosenName = names[i]; break; }
        if (!chosen) throw std::runtime_error("no FSR " + std::to_string(fsrMajor) + " provider");
        b.outW = outW, b.outH = outH;
        b.renderW = uint32_t(float(outW) / ratio), b.renderH = uint32_t(float(outH) / ratio);
        ffxCreateContextDescUpscaleVersion version{};
        version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
        version.version = FFX_UPSCALER_VERSION;
        ffxOverrideVersion over{};
        over.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
        over.header.pNext = &version.header;
        over.versionId = chosen;
        ffxCreateBackendVKDesc backend{};
        backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
        backend.header.pNext = &over.header;
        backend.deviceContext = dc;
        ffxCreateContextDescUpscale up{};
        up.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        up.header.pNext = &backend.header;
        up.maxRenderSize = {b.renderW, b.renderH};
        up.maxUpscaleSize = {outW, outH};
        if (ffxCreateContext(&b.context, &up.header, nullptr) != FFX_API_RETURN_OK) throw std::runtime_error("ffxCreateContext");
        std::printf("%s; FSR %s; %ux%u -> %ux%u; queue families %u (graphics) and %u (compute only)\n", props.deviceName, chosenName,
                    b.renderW, b.renderH, outW, outH, b.graphicsFamily, b.computeFamily);

        // images (both slots), command buffers, semaphores, the load pipeline
        for (uint32_t s = 0; s < 2; ++s) {
            b.makeImage(b.colour[s], VK_FORMAT_R16G16B16A16_SFLOAT, b.renderW, b.renderH, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
            b.makeImage(b.depth[s], VK_FORMAT_R32_SFLOAT, b.renderW, b.renderH, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
            b.makeImage(b.motion[s], VK_FORMAT_R16G16_SFLOAT, b.renderW, b.renderH, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
            b.makeImage(b.output[s], outputFormat, outW, outH, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        }
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pi.queueFamilyIndex = b.graphicsFamily;
        checked(vkCreateCommandPool(b.device, &pi, nullptr, &b.graphicsPool), "vkCreateCommandPool");
        pi.queueFamilyIndex = b.computeFamily;
        checked(vkCreateCommandPool(b.device, &pi, nullptr, &b.computePool), "vkCreateCommandPool");
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, ai.commandBufferCount = 2;
        ai.commandPool = b.graphicsPool;
        checked(vkAllocateCommandBuffers(b.device, &ai, b.loadCmd.data()), "vkAllocateCommandBuffers");
        checked(vkAllocateCommandBuffers(b.device, &ai, b.postCmd.data()), "vkAllocateCommandBuffers");
        checked(vkAllocateCommandBuffers(b.device, &ai, b.serialCmd.data()), "vkAllocateCommandBuffers");
        ai.commandPool = b.computePool;
        checked(vkAllocateCommandBuffers(b.device, &ai, b.fsrCmd.data()), "vkAllocateCommandBuffers");
        VkSemaphoreTypeCreateInfo ti{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        ti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &ti};
        for (VkSemaphore* s : {&b.loaded, &b.upscaled, &b.posted}) checked(vkCreateSemaphore(b.device, &si, nullptr, s), "vkCreateSemaphore");
        VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = 1, li.pBindings = &binding;
        checked(vkCreateDescriptorSetLayout(b.device, &li, nullptr, &b.setLayout), "vkCreateDescriptorSetLayout");
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4};
        VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpi.maxSets = 4, dpi.poolSizeCount = 1, dpi.pPoolSizes = &size;
        checked(vkCreateDescriptorPool(b.device, &dpi, nullptr, &b.descriptorPool), "vkCreateDescriptorPool");
        for (uint32_t s = 0; s < 2; ++s)
            for (auto [set, image] : {std::pair{&b.colourSet[s], &b.colour[s]}, std::pair{&b.outputSet[s], &b.output[s]}}) {
                VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
                dai.descriptorPool = b.descriptorPool, dai.descriptorSetCount = 1, dai.pSetLayouts = &b.setLayout;
                checked(vkAllocateDescriptorSets(b.device, &dai, set), "vkAllocateDescriptorSets");
                VkDescriptorImageInfo info{VK_NULL_HANDLE, image->view, VK_IMAGE_LAYOUT_GENERAL};
                VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                w.dstSet = *set, w.descriptorCount = 1, w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, w.pImageInfo = &info;
                vkUpdateDescriptorSets(b.device, 1, &w, 0, nullptr);
            }
        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 4};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1, pli.pSetLayouts = &b.setLayout, pli.pushConstantRangeCount = 1, pli.pPushConstantRanges = &push;
        checked(vkCreatePipelineLayout(b.device, &pli, nullptr, &b.pipelineLayout), "vkCreatePipelineLayout");
        VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        smi.codeSize = sizeof kLoadShader, smi.pCode = kLoadShader;
        VkShaderModule module;
        checked(vkCreateShaderModule(b.device, &smi, nullptr, &module), "vkCreateShaderModule");
        VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cpi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr};
        cpi.layout = b.pipelineLayout;
        checked(vkCreateComputePipelines(b.device, VK_NULL_HANDLE, 1, &cpi, nullptr, &b.load), "vkCreateComputePipelines");
        vkDestroyShaderModule(b.device, module, nullptr);
        b.raster = rasterLoad;
        {   // the raster load: depth only, a full-screen triangle per instance, every fragment written (compare ALWAYS)
            b.makeImage(b.sceneDepth, VK_FORMAT_D32_SFLOAT, b.renderW, b.renderH, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
            VkPipelineLayoutCreateInfo rli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            checked(vkCreatePipelineLayout(b.device, &rli, nullptr, &b.rasterLayout), "vkCreatePipelineLayout");
            VkShaderModuleCreateInfo vmi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            vmi.codeSize = sizeof kRasterShader, vmi.pCode = kRasterShader;
            VkShaderModule vertex;
            checked(vkCreateShaderModule(b.device, &vmi, nullptr, &vertex), "vkCreateShaderModule");
            VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vertex, "main", nullptr};
            VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
            ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
            vp.viewportCount = 1, vp.scissorCount = 1;
            VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
            rs.polygonMode = VK_POLYGON_MODE_FILL, rs.cullMode = VK_CULL_MODE_NONE, rs.lineWidth = 1.0f;
            VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
            ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
            ds.depthTestEnable = VK_TRUE, ds.depthWriteEnable = VK_TRUE, ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
            VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
            const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
            VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
            dy.dynamicStateCount = 2, dy.pDynamicStates = dyn;
            const VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;
            VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
            rendering.depthAttachmentFormat = depthFormat;
            VkGraphicsPipelineCreateInfo gpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering};
            gpi.stageCount = 1, gpi.pStages = &stage;
            gpi.pVertexInputState = &vin, gpi.pInputAssemblyState = &ia, gpi.pViewportState = &vp, gpi.pRasterizationState = &rs;
            gpi.pMultisampleState = &ms, gpi.pDepthStencilState = &ds, gpi.pColorBlendState = &cb, gpi.pDynamicState = &dy;
            gpi.layout = b.rasterLayout;
            checked(vkCreateGraphicsPipelines(b.device, VK_NULL_HANDLE, 1, &gpi, nullptr, &b.rasterPipeline), "vkCreateGraphicsPipelines");
            vkDestroyShaderModule(b.device, vertex, nullptr);
        }

        // the images' first layouts and contents (on the graphics queue)
        {
            VkCommandBuffer cmd = b.serialCmd[0];
            Bench::begin(cmd);
            VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkClearColorValue grey{}, half{}, zero{};
            for (float& c : grey.float32) c = 0.4f;
            half.float32[0] = 0.5f;
            for (uint32_t s = 0; s < 2; ++s) {
                for (Image* im : {&b.colour[s], &b.depth[s], &b.motion[s], &b.output[s]})
                    Bench::barrier(cmd, *im, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                                   VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                vkCmdClearColorImage(cmd, b.colour[s].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &grey, 1, &range);
                vkCmdClearColorImage(cmd, b.depth[s].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &half, 1, &range);
                vkCmdClearColorImage(cmd, b.motion[s].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
                vkCmdClearColorImage(cmd, b.output[s].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
                for (Image* im : {&b.colour[s], &b.depth[s], &b.motion[s]})
                    Bench::barrier(cmd, *im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                                   VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
                Bench::barrier(cmd, b.output[s], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                               VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
            }
            b.submit(b.graphics, cmd, nullptr, 0, nullptr, 0, b.posted, 0);   // (value 0: no change, the GPU's work done below)
            checked(vkQueueWaitIdle(b.graphics), "vkQueueWaitIdle");
        }

        if (concurrencyTest) {
            for (double target : loads) {
                const uint32_t iterations = uint32_t(std::max(1.0, std::round(target * 1000.0)));   // (narrow: ~1 µs an iteration)
                const double one = b.concurrency(iterations, frames, false), two = b.concurrency(iterations, frames, true);
                std::printf("two loads of ~%.1f ms: one queue %.3f ms, two queues %.3f ms per frame (%s)\n", target, one, two,
                            two < one * 0.85 ? "they overlap" : "no overlap");
            }
            vkDeviceWaitIdle(b.device);
            ffxDestroyContext(&b.context, nullptr);
            return 0;
        }
        // the load's cost per iteration (the load alone, no FSR), then each load: alone, FSR alone, serial, async
        const double base = b.run(0, frames, false, false);
        const double per = (b.run(400, frames, false, false) - base) / 400.0;
        const double fsrAlone = b.run(0, frames, true, false) - base;
        std::printf("load: %s; per frame: FSR %.3f ms on its own (the load: %.4f ms per %s; the frame's fixed part %.3f ms)\n",
                    rasterLoad ? "raster (depth-only full-screen passes: fill-bound, shader cores mostly free)" : "compute (arithmetic on every pixel: every shader core busy)",
                    fsrAlone, per, rasterLoad ? "full-screen pass" : "round", base);
        std::printf("%-12s %10s %10s %10s %8s\n", "load", "serial", "async", "saved", "gain");
        for (double target : loads) {
            const uint32_t iterations = uint32_t(std::max(1.0, std::round(target / std::max(per, 1e-6))));
            const double alone = b.run(iterations, frames, false, false);
            const double serial = b.run(iterations, frames, true, false);
            const double async = b.run(iterations, frames, true, true);
            std::printf("%5.2f ms%5s %8.3f ms %8.3f ms %7.3f ms %7.1f%%\n", alone, "", serial, async, serial - async, 100.0 * (serial / async - 1.0));
        }
        vkDeviceWaitIdle(b.device);
        ffxDestroyContext(&b.context, nullptr);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ffxasync: %s\n", e.what());
        if (b.device) vkDeviceWaitIdle(b.device);
        return 1;
    }
}
