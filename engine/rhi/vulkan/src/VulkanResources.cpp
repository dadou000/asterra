#include "VulkanObjects.hpp"

#include <stdexcept>
#include <utility>

namespace orbit::rhi::vulkan::detail
{
namespace
{
[[nodiscard]] VkBufferUsageFlags ToNativeBufferUsage(const BufferUsage usage)
{
    // Every buffer additionally gets TRANSFER_SRC/DST since CopyBuffer
    // works on arbitrary source/destination buffers regardless of
    // their primary usage (mirrors D3D12's committed resources, which
    // impose no such restriction either).
    VkBufferUsageFlags flags =
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    switch (usage)
    {
    case BufferUsage::Generic:
        flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        break;
    case BufferUsage::Vertex:
        flags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        break;
    case BufferUsage::Index:
        flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        break;
    case BufferUsage::Constant:
        flags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        break;
    case BufferUsage::Structured:
        // Bound via SetGraphicsBuffer -> vkCmdPushDescriptorSetKHR as
        // a storage buffer (see VulkanCommands.cpp); this is the
        // Vulkan analogue of the D3D12 root-descriptor SRV binding
        // every ByteAddressBuffer/StructuredBuffer in this codebase
        // uses today.
        flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        break;
    }

    return flags;
}
} // namespace

// See the declaration in VulkanObjects.hpp for why this exists.
void TransitionNewImageBlocking(
    const VkDevice device,
    const u32 graphicsFamilyIndex,
    const VkImage image,
    const VkImageAspectFlags aspectMask,
    const ResourceState targetState)
{
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, graphicsFamilyIndex, 0, &queue);

    VkCommandPoolCreateInfo poolCreateInfo{};
    poolCreateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolCreateInfo.flags =
        VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolCreateInfo.queueFamilyIndex = graphicsFamilyIndex;

    VkCommandPool pool = VK_NULL_HANDLE;
    if (vkCreateCommandPool(
            device, &poolCreateInfo, nullptr, &pool) != VK_SUCCESS)
    {
        throw std::runtime_error(
            "Orbit failed to create a transient Vulkan command pool.");
    }

    VkCommandBufferAllocateInfo allocateInfo{};
    allocateInfo.sType =
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocateInfo.commandPool = pool;
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device, &allocateInfo, &commandBuffer);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(commandBuffer, &beginInfo);

    const bool isDepth =
        (aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
    const ImageBarrierInfo destination =
        ToImageBarrierInfo(targetState, isDepth);

    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_NONE;
    barrier.dstStageMask = destination.stageMask;
    barrier.dstAccessMask = destination.accessMask;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = destination.layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {
        aspectMask, 0, 1, 0, 1
    };

    VkDependencyInfo dependencyInfo{};
    dependencyInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependencyInfo.imageMemoryBarrierCount = 1;
    dependencyInfo.pImageMemoryBarriers = &barrier;

    vkCmdPipelineBarrier2(commandBuffer, &dependencyInfo);
    vkEndCommandBuffer(commandBuffer);

    VkCommandBufferSubmitInfo commandBufferInfo{};
    commandBufferInfo.sType =
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    commandBufferInfo.commandBuffer = commandBuffer;

    VkSubmitInfo2 submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submitInfo.commandBufferInfoCount = 1;
    submitInfo.pCommandBufferInfos = &commandBufferInfo;

    vkQueueSubmit2(queue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);

    vkDestroyCommandPool(device, pool, nullptr);
}

VkFormat ToNativeTextureFormat(const TextureFormat format)
{
    switch (format)
    {
    case TextureFormat::RGBA8_UNorm:
        return VK_FORMAT_R8G8B8A8_UNORM;
    case TextureFormat::D32_Float:
        return VK_FORMAT_D32_SFLOAT;
    case TextureFormat::R16_Float:
        return VK_FORMAT_R16_SFLOAT;
    case TextureFormat::RG16_Float:
        return VK_FORMAT_R16G16_SFLOAT;
    case TextureFormat::RGBA16_Float:
        return VK_FORMAT_R16G16B16A16_SFLOAT;
    case TextureFormat::R16_UInt:
        return VK_FORMAT_R16_UINT;
    case TextureFormat::R32_Float:
        return VK_FORMAT_R32_SFLOAT;
    case TextureFormat::RG32_Float:
        return VK_FORMAT_R32G32_SFLOAT;
    }

    throw std::invalid_argument(
        "Orbit received an invalid texture format.");
}

VulkanBuffer::VulkanBuffer(
    const VmaAllocator allocator,
    const VkBuffer nativeBuffer,
    const VmaAllocation allocation,
    const BufferDesc desc)
    : allocator_(allocator),
      nativeBuffer_(nativeBuffer),
      allocation_(allocation),
      desc_(desc)
{
}

VulkanBuffer::~VulkanBuffer()
{
    if (mapped_)
    {
        vmaUnmapMemory(allocator_, allocation_);
    }

    if (nativeBuffer_ != VK_NULL_HANDLE)
    {
        vmaDestroyBuffer(allocator_, nativeBuffer_, allocation_);
    }
}

u64 VulkanBuffer::SizeBytes() const noexcept
{
    return desc_.sizeBytes;
}

BufferUsage VulkanBuffer::Usage() const noexcept
{
    return desc_.usage;
}

MemoryUsage VulkanBuffer::Memory() const noexcept
{
    return desc_.memory;
}

std::byte* VulkanBuffer::Map()
{
    if (desc_.memory == MemoryUsage::GpuOnly)
    {
        throw std::runtime_error(
            "Orbit cannot map a GPU-only Vulkan buffer.");
    }

    if (mapped_)
    {
        throw std::runtime_error(
            "Orbit Vulkan buffer is already mapped.");
    }

    void* data = nullptr;
    if (vmaMapMemory(allocator_, allocation_, &data) != VK_SUCCESS)
    {
        throw std::runtime_error(
            "Orbit failed to map a Vulkan buffer.");
    }

    if (desc_.memory == MemoryUsage::HostReadback)
    {
        if (vmaInvalidateAllocation(
                allocator_,
                allocation_,
                0,
                VK_WHOLE_SIZE) !=
            VK_SUCCESS)
        {
            vmaUnmapMemory(
                allocator_,
                allocation_);
            throw std::runtime_error(
                "Orbit failed to invalidate a Vulkan readback buffer.");
        }
    }

    mapped_ = true;
    return static_cast<std::byte*>(data);
}

void VulkanBuffer::Unmap()
{
    if (!mapped_)
    {
        return;
    }

    vmaUnmapMemory(allocator_, allocation_);
    mapped_ = false;
}

VkBuffer VulkanBuffer::Native() const noexcept
{
    return nativeBuffer_;
}

VulkanTexture::VulkanTexture(
    const VkDevice device,
    const VmaAllocator allocator,
    const VkImage nativeImage,
    const VmaAllocation allocation,
    const VkImageView imageView,
    const u32 width,
    const u32 height,
    const TextureFormat format,
    const bool ownsImage)
    : device_(device),
      allocator_(allocator),
      nativeImage_(nativeImage),
      allocation_(allocation),
      imageView_(imageView),
      width_(width),
      height_(height),
      format_(format),
      ownsImage_(ownsImage),
      // Regular (ownsImage=true) textures are already pre-warmed into
      // their real initial layout by TransitionNewImageBlocking
      // before this constructor runs; a swapchain-provided
      // (ownsImage=false) image starts truly UNDEFINED and unacquired
      // -- see EverUsed()'s declaration in VulkanObjects.hpp.
      everUsed_(ownsImage)
{
}

VulkanTexture::~VulkanTexture()
{
    if (imageView_ != VK_NULL_HANDLE)
    {
        vkDestroyImageView(device_, imageView_, nullptr);
    }

    if (ownsImage_ && nativeImage_ != VK_NULL_HANDLE)
    {
        vmaDestroyImage(allocator_, nativeImage_, allocation_);
    }
}

u32 VulkanTexture::Width() const noexcept
{
    return width_;
}

u32 VulkanTexture::Height() const noexcept
{
    return height_;
}

TextureFormat VulkanTexture::Format() const noexcept
{
    return format_;
}

VkImage VulkanTexture::Native() const noexcept
{
    return nativeImage_;
}

VkImageView VulkanTexture::View() const noexcept
{
    return imageView_;
}

bool VulkanTexture::IsDepth() const noexcept
{
    return format_ == TextureFormat::D32_Float;
}

void VulkanTexture::SetPendingClear(const VkClearValue& value) noexcept
{
    pendingClear_ = value;
}

std::optional<VkClearValue> VulkanTexture::TakePendingClear() noexcept
{
    const auto value = pendingClear_;
    pendingClear_.reset();
    return value;
}

bool VulkanTexture::EverUsed() const noexcept
{
    return everUsed_;
}

void VulkanTexture::MarkUsed() noexcept
{
    everUsed_ = true;
}

std::unique_ptr<Buffer> VulkanDevice::CreateBuffer(const BufferDesc& desc)
{
    if (desc.sizeBytes == 0)
    {
        throw std::invalid_argument(
            "Orbit cannot create a zero-sized buffer.");
    }

    VkBufferCreateInfo bufferCreateInfo{};
    bufferCreateInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferCreateInfo.size = desc.sizeBytes;
    bufferCreateInfo.usage = ToNativeBufferUsage(desc.usage);
    bufferCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocationCreateInfo{};
    allocationCreateInfo.usage = VMA_MEMORY_USAGE_AUTO;

    if (desc.memory == MemoryUsage::HostVisible)
    {
        allocationCreateInfo.flags =
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
    }
    else if (desc.memory == MemoryUsage::HostReadback)
    {
        allocationCreateInfo.flags =
            VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
        allocationCreateInfo.usage =
            VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
    }

    VkBuffer nativeBuffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;

    if (vmaCreateBuffer(
            allocator_,
            &bufferCreateInfo,
            &allocationCreateInfo,
            &nativeBuffer,
            &allocation,
            nullptr) != VK_SUCCESS)
    {
        throw std::runtime_error(
            "Orbit failed to create a Vulkan buffer.");
    }

    return std::make_unique<VulkanBuffer>(
        allocator_, nativeBuffer, allocation, desc);
}

std::unique_ptr<Texture> VulkanDevice::CreateTexture(
    const TextureDesc& desc)
{
    if (desc.width == 0 || desc.height == 0)
    {
        throw std::invalid_argument(
            "Orbit cannot create a zero-sized texture.");
    }

    const VkFormat nativeFormat =
        ToNativeTextureFormat(desc.format);

    const bool isDepth = desc.format == TextureFormat::D32_Float;

    VkImageCreateInfo imageCreateInfo{};
    imageCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageCreateInfo.imageType = VK_IMAGE_TYPE_2D;
    imageCreateInfo.format = nativeFormat;
    imageCreateInfo.extent = {desc.width, desc.height, 1};
    imageCreateInfo.mipLevels = 1;
    imageCreateInfo.arrayLayers = 1;
    imageCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageCreateInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    // TRANSFER_DST + SAMPLED are harmless to add unconditionally for
    // every texture (attachments were never sampled before this, so
    // existing render targets/depth buffers are unaffected) and let
    // CopyBufferToTexture and SetGraphicsTexture work on any texture
    // without a new TextureDesc field to opt in.
    imageCreateInfo.usage =
        (isDepth
             ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
             : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
        VK_IMAGE_USAGE_SAMPLED_BIT;

    // Opt-in, unlike the above: storage-image support isn't universally
    // free on every format/tiling combination, and it signals real UAV
    // intent (a compute shader imageLoad/imageStore-ing this texture) --
    // see TextureDesc::allowUnorderedAccess.
    if (desc.allowUnorderedAccess)
    {
        imageCreateInfo.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    }
    imageCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocationCreateInfo{};
    allocationCreateInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    VkImage nativeImage = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;

    if (vmaCreateImage(
            allocator_,
            &imageCreateInfo,
            &allocationCreateInfo,
            &nativeImage,
            &allocation,
            nullptr) != VK_SUCCESS)
    {
        throw std::runtime_error(
            "Orbit failed to create a Vulkan texture.");
    }

    const VkImageAspectFlags aspectMask =
        isDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;

    VkImageViewCreateInfo viewCreateInfo{};
    viewCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewCreateInfo.image = nativeImage;
    viewCreateInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewCreateInfo.format = nativeFormat;
    viewCreateInfo.subresourceRange = {
        aspectMask, 0, 1, 0, 1
    };

    VkImageView imageView = VK_NULL_HANDLE;
    if (vkCreateImageView(
            nativeDevice_, &viewCreateInfo, nullptr, &imageView) !=
        VK_SUCCESS)
    {
        vmaDestroyImage(allocator_, nativeImage, allocation);
        throw std::runtime_error(
            "Orbit failed to create a Vulkan texture view.");
    }

    TransitionNewImageBlocking(
        nativeDevice_,
        graphicsFamilyIndex_,
        nativeImage,
        aspectMask,
        desc.initialState);

    return std::make_unique<VulkanTexture>(
        nativeDevice_,
        allocator_,
        nativeImage,
        allocation,
        imageView,
        desc.width,
        desc.height,
        desc.format,
        true);
}

VulkanTimestampQueryPool::VulkanTimestampQueryPool(
    const VkDevice device, const VkQueryPool pool, const u32 count)
    : device_(device), pool_(pool), count_(count)
{
}

VulkanTimestampQueryPool::~VulkanTimestampQueryPool()
{
    if (pool_ != VK_NULL_HANDLE)
    {
        vkDestroyQueryPool(device_, pool_, nullptr);
    }
}

u32 VulkanTimestampQueryPool::Count() const noexcept
{
    return count_;
}

VkQueryPool VulkanTimestampQueryPool::Native() const noexcept
{
    return pool_;
}

bool VulkanTimestampQueryPool::TryGetResults(
    const u32 first, const u32 count, u64* const outTicks) const
{
    // No WAIT bit: the caller already knows (via its own frame fence)
    // that the writing work has completed, so a query that isn't ready
    // here means it was simply never written this frame (e.g. a pass
    // that didn't run) rather than something worth blocking on.
    const VkResult result = vkGetQueryPoolResults(
        device_,
        pool_,
        first,
        count,
        static_cast<std::size_t>(count) * sizeof(u64),
        outTicks,
        sizeof(u64),
        VK_QUERY_RESULT_64_BIT);

    return result == VK_SUCCESS;
}

std::unique_ptr<TimestampQueryPool>
VulkanDevice::CreateTimestampQueryPool(const u32 count)
{
    if (count == 0)
    {
        throw std::invalid_argument(
            "Orbit cannot create a zero-size timestamp query pool.");
    }

    VkQueryPoolCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    createInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
    createInfo.queryCount = count;

    VkQueryPool pool = VK_NULL_HANDLE;
    if (vkCreateQueryPool(
            nativeDevice_, &createInfo, nullptr, &pool) != VK_SUCCESS)
    {
        throw std::runtime_error(
            "Orbit failed to create a Vulkan timestamp query pool.");
    }

    return std::make_unique<VulkanTimestampQueryPool>(
        nativeDevice_, pool, count);
}

f64 VulkanDevice::TimestampPeriodNanoseconds() const noexcept
{
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physicalDevice_, &properties);
    return static_cast<f64>(properties.limits.timestampPeriod);
}
} // namespace orbit::rhi::vulkan::detail
