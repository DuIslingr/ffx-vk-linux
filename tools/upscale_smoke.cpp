#include "upscale_smoke.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr uint32_t renderWidth = 128, renderHeight = 72, outputWidth = 256, outputHeight = 144;

void checked(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(operation) + ": Vulkan error " + std::to_string(result));
}

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageCreateInfo info{};
    FfxApiResource resource(uint32_t state) const {
        return ffxApiGetResourceVK(reinterpret_cast<void*>(image), ffxApiGetImageResourceDescriptionVK(image, info, 0), state);
    }
};

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
};

struct Smoke {
    VkDevice device;
    VkPhysicalDevice physicalDevice;
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    std::array<Image, 4> images;
    std::array<Buffer, 2> buffers;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;

    Smoke(VkDevice d, VkPhysicalDevice p) : device(d), physicalDevice(p) {
        vkGetPhysicalDeviceMemoryProperties(p, &memoryProperties);
    }
    ~Smoke() {
        // Includes early failures after a successful submit.
        vkDeviceWaitIdle(device);
        if (pool) vkDestroyCommandPool(device, pool, nullptr);
        for (auto& image : images) {
            if (image.image) vkDestroyImage(device, image.image, nullptr);
            if (image.memory) vkFreeMemory(device, image.memory, nullptr);
        }
        for (auto& buffer : buffers) {
            if (buffer.buffer) vkDestroyBuffer(device, buffer.buffer, nullptr);
            if (buffer.memory) vkFreeMemory(device, buffer.memory, nullptr);
        }
    }

    uint32_t memoryType(uint32_t mask, VkMemoryPropertyFlags flags) {
        for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i)
            if ((mask & (1u << i)) && (memoryProperties.memoryTypes[i].propertyFlags & flags) == flags) return i;
        throw std::runtime_error("no suitable Vulkan memory type");
    }

    void image(size_t index, VkFormat format, uint32_t width, uint32_t height, VkImageUsageFlags usage) {
        auto& image = images[index];
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &properties);
        VkFormatFeatureFlags required = 0;
        if (usage & VK_IMAGE_USAGE_SAMPLED_BIT) required |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        if (usage & VK_IMAGE_USAGE_STORAGE_BIT) required |= VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
        if ((properties.optimalTilingFeatures & required) != required) throw std::runtime_error("required image format unsupported");
        image.info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image.info.imageType = VK_IMAGE_TYPE_2D;
        image.info.format = format;
        image.info.extent = {width, height, 1};
        image.info.mipLevels = 1;
        image.info.arrayLayers = 1;
        image.info.samples = VK_SAMPLE_COUNT_1_BIT;
        image.info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image.info.usage = usage;
        image.info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        checked(vkCreateImage(device, &image.info, nullptr, &image.image), "vkCreateImage");
        VkMemoryRequirements requirements;
        vkGetImageMemoryRequirements(device, image.image, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        checked(vkAllocateMemory(device, &allocation, nullptr, &image.memory), "vkAllocateMemory(image)");
        checked(vkBindImageMemory(device, image.image, image.memory, 0), "vkBindImageMemory");
    }

    void buffer(size_t index, VkDeviceSize size, VkBufferUsageFlags usage) {
        auto& buffer = buffers[index];
        buffer.size = size;
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = size;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        checked(vkCreateBuffer(device, &info, nullptr, &buffer.buffer), "vkCreateBuffer");
        VkMemoryRequirements requirements;
        vkGetBufferMemoryRequirements(device, buffer.buffer, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        checked(vkAllocateMemory(device, &allocation, nullptr, &buffer.memory), "vkAllocateMemory(buffer)");
        checked(vkBindBufferMemory(device, buffer.buffer, buffer.memory, 0), "vkBindBufferMemory");
    }

    void begin() {
        checked(vkResetCommandBuffer(command, 0), "vkResetCommandBuffer");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        checked(vkBeginCommandBuffer(command, &begin), "vkBeginCommandBuffer");
    }

    void submit() {
        checked(vkEndCommandBuffer(command), "vkEndCommandBuffer");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        checked(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit");
        checked(vkQueueWaitIdle(queue), "vkQueueWaitIdle");
    }

    void transition(size_t index, VkImageLayout before, VkImageLayout after,
                    VkAccessFlags source, VkAccessFlags destination,
                    VkPipelineStageFlags sourceStage, VkPipelineStageFlags destinationStage) {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcAccessMask = source;
        barrier.dstAccessMask = destination;
        barrier.oldLayout = before;
        barrier.newLayout = after;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = images[index].image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(command, sourceStage, destinationStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }
};

float halfFloat(uint16_t half) {
    const uint32_t sign = uint32_t(half & 0x8000) << 16;
    const uint32_t exponent = (half >> 10) & 31;
    const uint32_t mantissa = half & 1023;
    if (exponent == 0) return std::copysign(std::ldexp(float(mantissa), -24), sign ? -1.0f : 1.0f);
    if (exponent == 31) return std::bit_cast<float>(sign | 0x7f800000u | (mantissa << 13));
    return std::bit_cast<float>(sign | ((exponent + 112) << 23) | (mantissa << 13));
}

bool checkOutput(const std::vector<uint16_t>& pixels, const char* outputPath) {
    std::array<double, 3> low{}, high{};
    float minimum = std::numeric_limits<float>::infinity(), maximum = -minimum;
    double error = 0;
    for (uint32_t y = 0; y < outputHeight; ++y) {
        for (uint32_t x = 0; x < outputWidth; ++x) {
            const size_t at = (size_t(y) * outputWidth + x) * 4;
            for (uint32_t c = 0; c < 4; ++c) {
                const float value = halfFloat(pixels[at + c]);
                if (!std::isfinite(value)) throw std::runtime_error("upscale produced nonfinite output");
                if (c < 3) minimum = std::min(minimum, value), maximum = std::max(maximum, value);
                if (c < 2) {
                    const float expected = 0.1f + 0.7f * float(c == 0 ? x : y) / float(c == 0 ? outputWidth - 1 : outputHeight - 1);
                    error += std::abs(value - expected);
                }
                if (c < 3 && x < outputWidth / 4) low[c] += value;
                if (c < 3 && x >= outputWidth * 3 / 4) high[c] += value;
            }
        }
    }
    const double count = outputWidth * outputHeight / 4;
    const double meanError = error / (outputWidth * outputHeight * 2);
    std::printf("upscale readback: %ux%u, RGB range %.6f..%.6f, gradient MAE %.6f, red left/right %.6f/%.6f\n",
                outputWidth, outputHeight, minimum, maximum, meanError, low[0] / count, high[0] / count);
    // A successful dispatch with an unwritten, constant, scrambled, or all-black target fails.
    if (maximum - minimum < 0.25f || high[0] / count - low[0] / count < 0.25 || meanError > 0.15)
        throw std::runtime_error("upscale output does not preserve the test pattern");
    if (outputPath) {
        FILE* file = std::fopen(outputPath, "wb");
        if (!file) throw std::runtime_error("cannot open PPM output path");
        bool ok = std::fprintf(file, "P6\n%u %u\n255\n", outputWidth, outputHeight) > 0;
        for (size_t at = 0; ok && at < pixels.size(); at += 4) {
            unsigned char rgb[3];
            for (size_t c = 0; c != 3; ++c) rgb[c] = static_cast<unsigned char>(std::lround(std::clamp(halfFloat(pixels[at + c]), 0.0f, 1.0f) * 255.0f));
            ok = std::fwrite(rgb, 1, 3, file) == 3;
        }
        if (std::fclose(file) != 0) ok = false;
        if (!ok) throw std::runtime_error("cannot write PPM output");
        std::printf("upscale output: %s\n", outputPath);
    }
    return true;
}

} // namespace

bool upscaleSmoke(VkDevice device, VkPhysicalDevice physicalDevice, uint32_t queueFamily,
                  ffxContext* context, const char* outputPath) {
    try {
        Smoke smoke(device, physicalDevice);
        vkGetDeviceQueue(device, queueFamily, 0, &smoke.queue);
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool.queueFamilyIndex = queueFamily;
        checked(vkCreateCommandPool(device, &pool, nullptr, &smoke.pool), "vkCreateCommandPool");
        VkCommandBufferAllocateInfo commands{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        commands.commandPool = smoke.pool;
        commands.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commands.commandBufferCount = 1;
        checked(vkAllocateCommandBuffers(device, &commands, &smoke.command), "vkAllocateCommandBuffers");
        const auto inputUsage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        smoke.image(0, VK_FORMAT_R32G32B32A32_SFLOAT, renderWidth, renderHeight, inputUsage);
        smoke.image(1, VK_FORMAT_R32_SFLOAT, renderWidth, renderHeight, inputUsage);
        smoke.image(2, VK_FORMAT_R16G16_SFLOAT, renderWidth, renderHeight, inputUsage);
        smoke.image(3, VK_FORMAT_R16G16B16A16_SFLOAT, outputWidth, outputHeight,
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        smoke.buffer(0, renderWidth * renderHeight * 4 * sizeof(float), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        smoke.buffer(1, outputWidth * outputHeight * 4 * sizeof(uint16_t), VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        void* mapped = nullptr;
        checked(vkMapMemory(device, smoke.buffers[0].memory, 0, smoke.buffers[0].size, 0, &mapped), "vkMapMemory(upload)");
        auto* color = static_cast<float*>(mapped);
        for (uint32_t y = 0; y < renderHeight; ++y) for (uint32_t x = 0; x < renderWidth; ++x) {
            const size_t at = (size_t(y) * renderWidth + x) * 4;
            color[at] = 0.1f + 0.7f * float(x) / float(renderWidth - 1);
            color[at + 1] = 0.1f + 0.7f * float(y) / float(renderHeight - 1);
            color[at + 2] = ((x / 16 + y / 12) & 1) ? 0.7f : 0.2f;
            color[at + 3] = 1.0f;
        }
        vkUnmapMemory(device, smoke.buffers[0].memory);
        smoke.begin();
        for (size_t i = 0; i < 4; ++i)
            smoke.transition(i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {renderWidth, renderHeight, 1};
        vkCmdCopyBufferToImage(smoke.command, smoke.buffers[0].buffer, smoke.images[0].image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkClearColorValue depth{}, motion{}, unwritten{};
        depth.float32[0] = 0.5f;
        for (float& component : unwritten.float32) component = -8.0f;
        vkCmdClearColorImage(smoke.command, smoke.images[1].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &depth, 1, &range);
        vkCmdClearColorImage(smoke.command, smoke.images[2].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &motion, 1, &range);
        vkCmdClearColorImage(smoke.command, smoke.images[3].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &unwritten, 1, &range);
        for (size_t i = 0; i < 3; ++i)
            smoke.transition(i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        smoke.transition(3, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                         VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        smoke.submit();
        for (uint32_t frame = 0; frame != 3; ++frame) {
            smoke.begin();
            VkMemoryBarrier history{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            history.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            history.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(smoke.command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 0, 1, &history, 0, nullptr, 0, nullptr);
            ffxDispatchDescUpscale dispatch{};
            dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
            dispatch.commandList = smoke.command;
            dispatch.color = smoke.images[0].resource(FFX_API_RESOURCE_STATE_COMPUTE_READ);
            dispatch.depth = smoke.images[1].resource(FFX_API_RESOURCE_STATE_COMPUTE_READ);
            dispatch.motionVectors = smoke.images[2].resource(FFX_API_RESOURCE_STATE_COMPUTE_READ);
            dispatch.output = smoke.images[3].resource(FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
            dispatch.renderSize = {renderWidth, renderHeight};
            dispatch.upscaleSize = {outputWidth, outputHeight};
            dispatch.motionVectorScale = {float(renderWidth), float(renderHeight)};
            dispatch.frameTimeDelta = 1000.0f / 60.0f;
            dispatch.preExposure = 1.0f;
            dispatch.reset = frame == 0;
            dispatch.cameraNear = 0.1f;
            dispatch.cameraFar = 100.0f;
            dispatch.cameraFovAngleVertical = 1.04719755f;
            dispatch.viewSpaceToMetersFactor = 1.0f;
            const ffxReturnCode_t result = ffxDispatch(context, &dispatch.header);
            if (result != FFX_API_RETURN_OK) throw std::runtime_error("ffxDispatch frame " + std::to_string(frame) + ": " + std::to_string(result));
            smoke.submit();
        }
        smoke.begin();
        smoke.transition(3, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        copy.imageExtent = {outputWidth, outputHeight, 1};
        vkCmdCopyImageToBuffer(smoke.command, smoke.images[3].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               smoke.buffers[1].buffer, 1, &copy);
        VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        host.buffer = smoke.buffers[1].buffer;
        host.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(smoke.command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &host, 0, nullptr);
        smoke.submit();
        checked(vkMapMemory(device, smoke.buffers[1].memory, 0, smoke.buffers[1].size, 0, &mapped), "vkMapMemory(readback)");
        std::vector<uint16_t> pixels(size_t(outputWidth) * outputHeight * 4);
        std::memcpy(pixels.data(), mapped, pixels.size() * sizeof(uint16_t));
        vkUnmapMemory(device, smoke.buffers[1].memory);
        return checkOutput(pixels, outputPath);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "upscale smoke: %s\n", error.what());
        return false;
    }
}
