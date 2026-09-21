#include "VulkanObjects.hpp"

#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

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

VulkanAccelerationStructure::VulkanAccelerationStructure(
    const VkDevice device,
    const VmaAllocator allocator,
    const DeviceFunctions& functions,
    const VkAccelerationStructureKHR bottomLevel,
    const VkAccelerationStructureKHR topLevel,
    const VkBuffer bottomLevelBuffer,
    const VmaAllocation bottomLevelAllocation,
    const VkBuffer topLevelBuffer,
    const VmaAllocation topLevelAllocation,
    const VkBuffer aabbBuffer,
    const VmaAllocation aabbAllocation,
    const VkBuffer instanceBuffer,
    const VmaAllocation instanceAllocation,
    const u32 primitiveCount)
    : device_(device),
      allocator_(allocator),
      functions_(&functions),
      bottomLevel_(bottomLevel),
      topLevel_(topLevel),
      bottomLevelBuffer_(bottomLevelBuffer),
      bottomLevelAllocation_(bottomLevelAllocation),
      topLevelBuffer_(topLevelBuffer),
      topLevelAllocation_(topLevelAllocation),
      aabbBuffer_(aabbBuffer),
      aabbAllocation_(aabbAllocation),
      instanceBuffer_(instanceBuffer),
      instanceAllocation_(instanceAllocation),
      primitiveCount_(primitiveCount)
{
}

VulkanAccelerationStructure::~VulkanAccelerationStructure()
{
    if (functions_ != nullptr &&
        functions_->vkDestroyAccelerationStructureKHR != nullptr)
    {
        if (topLevel_ != VK_NULL_HANDLE)
        {
            functions_->vkDestroyAccelerationStructureKHR(
                device_,
                topLevel_,
                nullptr);
        }

        if (bottomLevel_ != VK_NULL_HANDLE)
        {
            functions_->vkDestroyAccelerationStructureKHR(
                device_,
                bottomLevel_,
                nullptr);
        }
    }

    if (topLevelBuffer_ != VK_NULL_HANDLE)
    {
        vmaDestroyBuffer(
            allocator_,
            topLevelBuffer_,
            topLevelAllocation_);
    }

    if (bottomLevelBuffer_ != VK_NULL_HANDLE)
    {
        vmaDestroyBuffer(
            allocator_,
            bottomLevelBuffer_,
            bottomLevelAllocation_);
    }

    if (instanceBuffer_ != VK_NULL_HANDLE)
    {
        vmaDestroyBuffer(
            allocator_,
            instanceBuffer_,
            instanceAllocation_);
    }

    if (aabbBuffer_ != VK_NULL_HANDLE)
    {
        vmaDestroyBuffer(
            allocator_,
            aabbBuffer_,
            aabbAllocation_);
    }
}

u32 VulkanAccelerationStructure::PrimitiveCount() const noexcept
{
    return primitiveCount_;
}

VkAccelerationStructureKHR
VulkanAccelerationStructure::TopLevel() const noexcept
{
    return topLevel_;
}

std::unique_ptr<AccelerationStructure>
VulkanDevice::CreateAabbAccelerationStructure(
    const std::span<const AccelerationAabb> aabbs)
{
    if (!capabilities_.accelerationStructures)
    {
        throw std::runtime_error(
            "Orbit cannot create an acceleration structure on this "
            "device: acceleration structures are unsupported.");
    }

    if (aabbs.empty())
    {
        throw std::invalid_argument(
            "Orbit cannot create an empty AABB acceleration structure.");
    }

    if (aabbs.size() >
        static_cast<std::size_t>(
            std::numeric_limits<u32>::max()))
    {
        throw std::overflow_error(
            "Orbit AABB acceleration structure exceeds the 32-bit "
            "primitive-count contract.");
    }

    const u32 primitiveCount =
        static_cast<u32>(aabbs.size());

    if (functions_.vkCreateAccelerationStructureKHR == nullptr ||
        functions_.vkDestroyAccelerationStructureKHR == nullptr ||
        functions_.vkGetAccelerationStructureBuildSizesKHR == nullptr ||
        functions_.vkCmdBuildAccelerationStructuresKHR == nullptr ||
        functions_.vkGetAccelerationStructureDeviceAddressKHR == nullptr)
    {
        throw std::runtime_error(
            "Orbit acceleration-structure entry points are unavailable.");
    }

    struct OwnedBuffer
    {
        VkBuffer buffer{VK_NULL_HANDLE};
        VmaAllocation allocation{nullptr};
    };

    const auto createBuffer =
        [this](
            const VkDeviceSize size,
            const VkBufferUsageFlags usage,
            const bool hostVisible)
        {
            VkBufferCreateInfo info{};
            info.sType =
                VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            info.size = size;
            info.usage = usage;
            info.sharingMode =
                VK_SHARING_MODE_EXCLUSIVE;

            VmaAllocationCreateInfo allocationInfo{};
            allocationInfo.usage =
                hostVisible
                    ? VMA_MEMORY_USAGE_AUTO_PREFER_HOST
                    : VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

            if (hostVisible)
            {
                allocationInfo.flags =
                    VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
            }

            OwnedBuffer result;

            if (vmaCreateBuffer(
                    allocator_,
                    &info,
                    &allocationInfo,
                    &result.buffer,
                    &result.allocation,
                    nullptr) != VK_SUCCESS)
            {
                throw std::runtime_error(
                    "Orbit failed to allocate an acceleration-structure "
                    "support buffer.");
            }

            return result;
        };

    const auto deviceAddress =
        [this](const VkBuffer buffer)
        {
            VkBufferDeviceAddressInfo info{};
            info.sType =
                VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
            info.buffer = buffer;
            return vkGetBufferDeviceAddress(
                nativeDevice_,
                &info);
        };

    std::vector<VkAabbPositionsKHR>
        nativeAabbs;
    nativeAabbs.reserve(aabbs.size());

    for (const auto& source : aabbs)
    {
        nativeAabbs.push_back({
            .minX = source.minimum.x,
            .minY = source.minimum.y,
            .minZ = source.minimum.z,
            .maxX = source.maximum.x,
            .maxY = source.maximum.y,
            .maxZ = source.maximum.z
        });
    }

    OwnedBuffer aabbBuffer{};
    OwnedBuffer bottomBuffer{};
    OwnedBuffer instanceBuffer{};
    OwnedBuffer topBuffer{};
    OwnedBuffer scratchBuffer{};

    VkAccelerationStructureKHR bottomLevel =
        VK_NULL_HANDLE;
    VkAccelerationStructureKHR topLevel =
        VK_NULL_HANDLE;

    const auto cleanup =
        [&]()
        {
            if (topLevel != VK_NULL_HANDLE)
            {
                functions_.vkDestroyAccelerationStructureKHR(
                    nativeDevice_,
                    topLevel,
                    nullptr);
                topLevel = VK_NULL_HANDLE;
            }

            if (bottomLevel != VK_NULL_HANDLE)
            {
                functions_.vkDestroyAccelerationStructureKHR(
                    nativeDevice_,
                    bottomLevel,
                    nullptr);
                bottomLevel = VK_NULL_HANDLE;
            }

            for (auto* buffer : {
                     &scratchBuffer,
                     &topBuffer,
                     &instanceBuffer,
                     &bottomBuffer,
                     &aabbBuffer})
            {
                if (buffer->buffer != VK_NULL_HANDLE)
                {
                    vmaDestroyBuffer(
                        allocator_,
                        buffer->buffer,
                        buffer->allocation);
                    buffer->buffer =
                        VK_NULL_HANDLE;
                    buffer->allocation =
                        nullptr;
                }
            }
        };

    try
    {
        aabbBuffer =
            createBuffer(
                nativeAabbs.size() *
                    sizeof(VkAabbPositionsKHR),
                VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                true);

        void* mappedAabbs = nullptr;
        if (vmaMapMemory(
                allocator_,
                aabbBuffer.allocation,
                &mappedAabbs) != VK_SUCCESS)
        {
            throw std::runtime_error(
                "Orbit failed to map AABB acceleration input.");
        }

        std::memcpy(
            mappedAabbs,
            nativeAabbs.data(),
            nativeAabbs.size() *
                sizeof(VkAabbPositionsKHR));

        vmaFlushAllocation(
            allocator_,
            aabbBuffer.allocation,
            0,
            VK_WHOLE_SIZE);
        vmaUnmapMemory(
            allocator_,
            aabbBuffer.allocation);

        VkAccelerationStructureGeometryAabbsDataKHR
            aabbData{};
        aabbData.sType =
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR;
        aabbData.data.deviceAddress =
            deviceAddress(
                aabbBuffer.buffer);
        aabbData.stride =
            sizeof(VkAabbPositionsKHR);

        VkAccelerationStructureGeometryKHR
            bottomGeometry{};
        bottomGeometry.sType =
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        bottomGeometry.geometryType =
            VK_GEOMETRY_TYPE_AABBS_KHR;
        bottomGeometry.flags =
            VK_GEOMETRY_OPAQUE_BIT_KHR;
        bottomGeometry.geometry.aabbs =
            aabbData;

        VkAccelerationStructureBuildGeometryInfoKHR
            bottomBuild{};
        bottomBuild.sType =
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        bottomBuild.type =
            VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        bottomBuild.flags =
            VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        bottomBuild.mode =
            VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        bottomBuild.geometryCount = 1U;
        bottomBuild.pGeometries =
            &bottomGeometry;

        VkAccelerationStructureBuildSizesInfoKHR
            bottomSizes{};
        bottomSizes.sType =
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;

        functions_.vkGetAccelerationStructureBuildSizesKHR(
            nativeDevice_,
            VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
            &bottomBuild,
            &primitiveCount,
            &bottomSizes);

        bottomBuffer =
            createBuffer(
                bottomSizes.accelerationStructureSize,
                VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                false);

        VkAccelerationStructureCreateInfoKHR
            bottomCreate{};
        bottomCreate.sType =
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
        bottomCreate.buffer =
            bottomBuffer.buffer;
        bottomCreate.size =
            bottomSizes.accelerationStructureSize;
        bottomCreate.type =
            VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;

        if (functions_.vkCreateAccelerationStructureKHR(
                nativeDevice_,
                &bottomCreate,
                nullptr,
                &bottomLevel) != VK_SUCCESS)
        {
            throw std::runtime_error(
                "Orbit failed to create the proxy BLAS.");
        }

        VkAccelerationStructureDeviceAddressInfoKHR
            bottomAddressInfo{};
        bottomAddressInfo.sType =
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
        bottomAddressInfo.accelerationStructure =
            bottomLevel;

        const VkDeviceAddress bottomAddress =
            functions_.vkGetAccelerationStructureDeviceAddressKHR(
                nativeDevice_,
                &bottomAddressInfo);

        VkAccelerationStructureInstanceKHR
            instance{};
        instance.transform.matrix[0][0] = 1.0F;
        instance.transform.matrix[1][1] = 1.0F;
        instance.transform.matrix[2][2] = 1.0F;
        instance.instanceCustomIndex = 0U;
        instance.mask = 0xFFU;
        instance.instanceShaderBindingTableRecordOffset = 0U;
        instance.flags =
            VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        instance.accelerationStructureReference =
            bottomAddress;

        instanceBuffer =
            createBuffer(
                sizeof(instance),
                VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                true);

        void* mappedInstance = nullptr;
        if (vmaMapMemory(
                allocator_,
                instanceBuffer.allocation,
                &mappedInstance) != VK_SUCCESS)
        {
            throw std::runtime_error(
                "Orbit failed to map TLAS instance input.");
        }

        std::memcpy(
            mappedInstance,
            &instance,
            sizeof(instance));
        vmaFlushAllocation(
            allocator_,
            instanceBuffer.allocation,
            0,
            VK_WHOLE_SIZE);
        vmaUnmapMemory(
            allocator_,
            instanceBuffer.allocation);

        VkAccelerationStructureGeometryInstancesDataKHR
            instanceData{};
        instanceData.sType =
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
        instanceData.arrayOfPointers =
            VK_FALSE;
        instanceData.data.deviceAddress =
            deviceAddress(
                instanceBuffer.buffer);

        VkAccelerationStructureGeometryKHR
            topGeometry{};
        topGeometry.sType =
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        topGeometry.geometryType =
            VK_GEOMETRY_TYPE_INSTANCES_KHR;
        topGeometry.geometry.instances =
            instanceData;

        constexpr u32 oneInstance = 1U;

        VkAccelerationStructureBuildGeometryInfoKHR
            topBuild{};
        topBuild.sType =
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        topBuild.type =
            VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        topBuild.flags =
            VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        topBuild.mode =
            VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        topBuild.geometryCount = 1U;
        topBuild.pGeometries =
            &topGeometry;

        VkAccelerationStructureBuildSizesInfoKHR
            topSizes{};
        topSizes.sType =
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;

        functions_.vkGetAccelerationStructureBuildSizesKHR(
            nativeDevice_,
            VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
            &topBuild,
            &oneInstance,
            &topSizes);

        topBuffer =
            createBuffer(
                topSizes.accelerationStructureSize,
                VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                false);

        VkAccelerationStructureCreateInfoKHR
            topCreate{};
        topCreate.sType =
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
        topCreate.buffer =
            topBuffer.buffer;
        topCreate.size =
            topSizes.accelerationStructureSize;
        topCreate.type =
            VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;

        if (functions_.vkCreateAccelerationStructureKHR(
                nativeDevice_,
                &topCreate,
                nullptr,
                &topLevel) != VK_SUCCESS)
        {
            throw std::runtime_error(
                "Orbit failed to create the proxy TLAS.");
        }

        VkPhysicalDeviceAccelerationStructurePropertiesKHR
            accelerationProperties{};
        accelerationProperties.sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;

        VkPhysicalDeviceProperties2 properties2{};
        properties2.sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        properties2.pNext =
            &accelerationProperties;

        vkGetPhysicalDeviceProperties2(
            physicalDevice_,
            &properties2);

        const VkDeviceSize scratchAlignment =
            std::max<VkDeviceSize>(
                accelerationProperties.
                    minAccelerationStructureScratchOffsetAlignment,
                1U);

        const VkDeviceSize scratchSize =
            std::max(
                bottomSizes.buildScratchSize,
                topSizes.buildScratchSize);

        scratchBuffer =
            createBuffer(
                scratchSize +
                    scratchAlignment -
                    1U,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                    VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                false);

        const VkDeviceAddress scratchBase =
            deviceAddress(
                scratchBuffer.buffer);

        const VkDeviceAddress scratchAddress =
            (scratchBase +
             scratchAlignment - 1U) &
            ~(scratchAlignment - 1U);

        bottomBuild.dstAccelerationStructure =
            bottomLevel;
        bottomBuild.scratchData.deviceAddress =
            scratchAddress;

        topBuild.dstAccelerationStructure =
            topLevel;
        topBuild.scratchData.deviceAddress =
            scratchAddress;

        VkAccelerationStructureBuildRangeInfoKHR
            bottomRange{};
        bottomRange.primitiveCount =
            primitiveCount;

        VkAccelerationStructureBuildRangeInfoKHR
            topRange{};
        topRange.primitiveCount = 1U;

        const VkAccelerationStructureBuildRangeInfoKHR*
            bottomRanges[] = {
                &bottomRange
            };
        const VkAccelerationStructureBuildRangeInfoKHR*
            topRanges[] = {
                &topRange
            };

        VkQueue queue = VK_NULL_HANDLE;
        vkGetDeviceQueue(
            nativeDevice_,
            graphicsFamilyIndex_,
            0U,
            &queue);

        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType =
            VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags =
            VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        poolInfo.queueFamilyIndex =
            graphicsFamilyIndex_;

        VkCommandPool pool =
            VK_NULL_HANDLE;

        if (vkCreateCommandPool(
                nativeDevice_,
                &poolInfo,
                nullptr,
                &pool) != VK_SUCCESS)
        {
            throw std::runtime_error(
                "Orbit failed to create the proxy AS build command pool.");
        }

        VkCommandBufferAllocateInfo commandInfo{};
        commandInfo.sType =
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        commandInfo.commandPool =
            pool;
        commandInfo.level =
            VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commandInfo.commandBufferCount =
            1U;

        VkCommandBuffer command =
            VK_NULL_HANDLE;

        if (vkAllocateCommandBuffers(
                nativeDevice_,
                &commandInfo,
                &command) != VK_SUCCESS)
        {
            vkDestroyCommandPool(
                nativeDevice_,
                pool,
                nullptr);
            throw std::runtime_error(
                "Orbit failed to allocate the proxy AS build command buffer.");
        }

        VkCommandBufferBeginInfo begin{};
        begin.sType =
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags =
            VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

        vkBeginCommandBuffer(
            command,
            &begin);

        functions_.vkCmdBuildAccelerationStructuresKHR(
            command,
            1U,
            &bottomBuild,
            bottomRanges);

        VkMemoryBarrier2 barrier{};
        barrier.sType =
            VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        barrier.srcStageMask =
            VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        barrier.srcAccessMask =
            VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        barrier.dstStageMask =
            VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        barrier.dstAccessMask =
            VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;

        VkDependencyInfo dependency{};
        dependency.sType =
            VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.memoryBarrierCount = 1U;
        dependency.pMemoryBarriers =
            &barrier;

        vkCmdPipelineBarrier2(
            command,
            &dependency);

        functions_.vkCmdBuildAccelerationStructuresKHR(
            command,
            1U,
            &topBuild,
            topRanges);

        vkEndCommandBuffer(command);

        VkCommandBufferSubmitInfo submitCommand{};
        submitCommand.sType =
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        submitCommand.commandBuffer =
            command;

        VkSubmitInfo2 submit{};
        submit.sType =
            VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit.commandBufferInfoCount =
            1U;
        submit.pCommandBufferInfos =
            &submitCommand;

        if (vkQueueSubmit2(
                queue,
                1U,
                &submit,
                VK_NULL_HANDLE) != VK_SUCCESS)
        {
            vkDestroyCommandPool(
                nativeDevice_,
                pool,
                nullptr);
            throw std::runtime_error(
                "Orbit failed to submit proxy AS build commands.");
        }

        vkQueueWaitIdle(queue);

        vkDestroyCommandPool(
            nativeDevice_,
            pool,
            nullptr);

        vmaDestroyBuffer(
            allocator_,
            scratchBuffer.buffer,
            scratchBuffer.allocation);
        scratchBuffer = {};

        return std::make_unique<
            VulkanAccelerationStructure>(
                nativeDevice_,
                allocator_,
                functions_,
                bottomLevel,
                topLevel,
                bottomBuffer.buffer,
                bottomBuffer.allocation,
                topBuffer.buffer,
                topBuffer.allocation,
                aabbBuffer.buffer,
                aabbBuffer.allocation,
                instanceBuffer.buffer,
                instanceBuffer.allocation,
                primitiveCount);
    }
    catch (...)
    {
        cleanup();
        throw;
    }
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
