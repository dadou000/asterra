#include "VulkanObjects.hpp"

#include <stdexcept>

namespace orbit::rhi::vulkan::detail
{
ImageBarrierInfo ToImageBarrierInfo(
    const ResourceState state,
    const bool isDepth)
{
    switch (state)
    {
    case ResourceState::Common:
        return {
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_PIPELINE_STAGE_2_NONE,
            VK_ACCESS_2_NONE};

    case ResourceState::Present:
        // Stage must be COLOR_ATTACHMENT_OUTPUT, matching where the
        // swapchain's acquire/render-finished semaphores wait and
        // signal (see VulkanQueue::Submit) -- NONE here (the "obvious"
        // choice, since the presentation engine has no real access
        // mask) doesn't chain with either semaphore's stage, which
        // synchronization validation flags as a WRITE_AFTER_READ on
        // the following Present->RenderTarget transition and a
        // PRESENT_AFTER_WRITE on the preceding RenderTarget->Present
        // one. Access stays NONE: presenting genuinely has no
        // barrier-visible memory access of its own to declare.
        return {
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_NONE};

    case ResourceState::RenderTarget:
        return {
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT};

    case ResourceState::DepthWrite:
        return {
            isDepth
                ? VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL
                : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};

    case ResourceState::DepthRead:
        return {
            isDepth
                ? VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL
                : VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT |
                VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                VK_ACCESS_2_SHADER_READ_BIT};

    case ResourceState::ShaderResource:
        return {
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_READ_BIT};

    case ResourceState::UnorderedAccess:
        return {
            VK_IMAGE_LAYOUT_GENERAL,
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            VK_ACCESS_2_SHADER_READ_BIT |
                VK_ACCESS_2_SHADER_WRITE_BIT};

    case ResourceState::CopySource:
        return {
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT};

    case ResourceState::CopyDestination:
        return {
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT};

    case ResourceState::IndirectArgument:
        // Buffer-only state.
        return {VK_IMAGE_LAYOUT_UNDEFINED, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE};

    case ResourceState::VertexOrConstantBuffer:
    case ResourceState::IndexBuffer:
        // Buffer-only states; never used for a Texture in this
        // codebase (see BufferBarrierInfo below for their real
        // mapping), but handled for interface completeness.
        return {
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_PIPELINE_STAGE_2_NONE,
            VK_ACCESS_2_NONE};
    }

    throw std::runtime_error(
        "Orbit received an invalid RHI resource state.");
}

BufferBarrierInfo ToBufferBarrierInfo(const ResourceState state)
{
    switch (state)
    {
    case ResourceState::Common:
    case ResourceState::Present:
    case ResourceState::RenderTarget:
    case ResourceState::DepthWrite:
    case ResourceState::DepthRead:
        // Attachment-only states; never used for a Buffer in this
        // codebase.
        return {VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE};

    case ResourceState::VertexOrConstantBuffer:
        return {
            VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT |
                VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT |
                VK_ACCESS_2_UNIFORM_READ_BIT};

    case ResourceState::IndexBuffer:
        return {
            VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT,
            VK_ACCESS_2_INDEX_READ_BIT};

    case ResourceState::ShaderResource:
        return {
            VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_READ_BIT};

    case ResourceState::UnorderedAccess:
        return {
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            VK_ACCESS_2_SHADER_READ_BIT |
                VK_ACCESS_2_SHADER_WRITE_BIT};

    case ResourceState::CopySource:
        return {
            VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT};

    case ResourceState::CopyDestination:
        return {
            VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT};

    case ResourceState::IndirectArgument:
        return {
            VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT,
            VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT};
    }

    throw std::runtime_error(
        "Orbit received an invalid RHI resource state.");
}

VulkanCommandAllocator::VulkanCommandAllocator(
    const VkDevice device,
    const QueueType type,
    const VkCommandPool pool)
    : device_(device), type_(type), pool_(pool)
{
}

VulkanCommandAllocator::~VulkanCommandAllocator()
{
    if (pool_ != VK_NULL_HANDLE)
    {
        vkDestroyCommandPool(device_, pool_, nullptr);
    }
}

QueueType VulkanCommandAllocator::Type() const noexcept
{
    return type_;
}

void VulkanCommandAllocator::Reset()
{
    if (vkResetCommandPool(device_, pool_, 0) != VK_SUCCESS)
    {
        throw std::runtime_error(
            "Orbit failed to reset a Vulkan command pool.");
    }
}

VkCommandPool VulkanCommandAllocator::Native() const noexcept
{
    return pool_;
}

VulkanCommandList::VulkanCommandList(
    const VkDevice device,
    const QueueType type,
    const VkCommandPool pool,
    const VkCommandBuffer nativeCommandList,
    const DeviceFunctions& functions,
    const VkSampler defaultSampler)
    : device_(device),
      type_(type),
      pool_(pool),
      nativeCommandList_(nativeCommandList),
      functions_(&functions),
      defaultSampler_(defaultSampler)
{
}

QueueType VulkanCommandList::Type() const noexcept
{
    return type_;
}

void VulkanCommandList::Reset(CommandAllocator& allocator)
{
    auto* vulkanAllocator =
        dynamic_cast<VulkanCommandAllocator*>(&allocator);

    if (vulkanAllocator == nullptr ||
        vulkanAllocator->Type() != type_)
    {
        throw std::runtime_error(
            "Orbit cannot reset a command list with an incompatible "
            "allocator.");
    }

    // A VkCommandBuffer is permanently bound to the VkCommandPool it
    // was allocated from -- unlike a D3D12 command list, it cannot be
    // rebound to a *different* allocator's memory. Main.cpp (mirroring
    // normal D3D12 practice) keeps one long-lived CommandList object
    // and cycles it across one CommandAllocator per frame-in-flight,
    // so track one real command buffer per pool this list has ever
    // been Reset() onto, and switch which one nativeCommandList_
    // points at accordingly. This must NOT free a pool's buffer when
    // moving away from it: with N frames in flight the one from two
    // Reset()s ago may still be executing on the GPU (that's the
    // entire point of multiple frames in flight) even though the pool
    // we're switching *to* has already been fence-waited by the
    // caller -- vkFreeCommandBuffers on a still-pending buffer is
    // exactly the hazard this cache avoids. Every buffer is left for
    // VulkanCommandAllocator's own destructor (vkDestroyCommandPool)
    // to free implicitly, all at once, when that pool goes away.
    const VkCommandPool targetPool = vulkanAllocator->Native();

    VkCommandBuffer* cached = nullptr;
    for (auto& [pool, commandBuffer] : commandBuffersByPool_)
    {
        if (pool == targetPool)
        {
            cached = &commandBuffer;
            break;
        }
    }

    if (cached != nullptr)
    {
        // vkResetCommandPool (VulkanCommandAllocator::Reset(), always
        // called by the caller just before this) already reset every
        // buffer allocated from it, this one included.
        nativeCommandList_ = *cached;
    }
    else
    {
        VkCommandBufferAllocateInfo allocateInfo{};
        allocateInfo.sType =
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocateInfo.commandPool = targetPool;
        allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocateInfo.commandBufferCount = 1;

        if (vkAllocateCommandBuffers(
                device_, &allocateInfo, &nativeCommandList_) !=
            VK_SUCCESS)
        {
            throw std::runtime_error(
                "Orbit failed to allocate a Vulkan command buffer "
                "during reset.");
        }

        commandBuffersByPool_.emplace_back(
            targetPool, nativeCommandList_);
    }

    pool_ = targetPool;

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

    if (vkBeginCommandBuffer(nativeCommandList_, &beginInfo) !=
        VK_SUCCESS)
    {
        throw std::runtime_error(
            "Orbit failed to begin a Vulkan command buffer.");
    }

    activePipeline_ = nullptr;
    renderingActive_ = false;
}

void VulkanCommandList::EndRenderingIfActive()
{
    if (renderingActive_)
    {
        vkCmdEndRendering(nativeCommandList_);
        renderingActive_ = false;
    }
}

void VulkanCommandList::BeginRendering(
    const std::span<const VkRenderingAttachmentInfo> colorAttachments,
    VkRenderingAttachmentInfo* depthAttachment,
    const u32 width,
    const u32 height)
{
    EndRenderingIfActive();

    // Remember what's being bound (forcing loadOp to LOAD) so a later
    // CopyBuffer on this command list -- which cannot be recorded
    // inside a dynamic-rendering scope -- can pause and transparently
    // resume rendering around the copy without ever re-clearing.
    pausedColorAttachments_.assign(
        colorAttachments.begin(),
        colorAttachments.end());
    for (auto& attachment : pausedColorAttachments_)
    {
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    }

    if (depthAttachment != nullptr)
    {
        pausedDepthAttachment_ = *depthAttachment;
        pausedDepthAttachment_->loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    }
    else
    {
        pausedDepthAttachment_.reset();
    }

    pausedWidth_ = width;
    pausedHeight_ = height;

    VkRenderingInfo renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    renderingInfo.renderArea = {{0, 0}, {width, height}};
    renderingInfo.layerCount = 1;

    if (!colorAttachments.empty())
    {
        renderingInfo.colorAttachmentCount =
            static_cast<u32>(colorAttachments.size());
        renderingInfo.pColorAttachments =
            colorAttachments.data();
    }

    if (depthAttachment != nullptr)
    {
        renderingInfo.pDepthAttachment = depthAttachment;
    }

    vkCmdBeginRendering(nativeCommandList_, &renderingInfo);
    renderingActive_ = true;
}

bool VulkanCommandList::PauseRenderingIfActive()
{
    const bool wasRendering = renderingActive_;
    EndRenderingIfActive();
    return wasRendering;
}

void VulkanCommandList::ResumeRenderingIfPaused(const bool wasRendering)
{
    if (!wasRendering)
    {
        return;
    }

    BeginRendering(
        pausedColorAttachments_,
        pausedDepthAttachment_.has_value()
            ? &*pausedDepthAttachment_
            : nullptr,
        pausedWidth_,
        pausedHeight_);
}

void VulkanCommandList::Transition(
    Texture& texture,
    const ResourceState before,
    const ResourceState after)
{
    if (before == after)
    {
        return;
    }

    auto* vulkanTexture = dynamic_cast<VulkanTexture*>(&texture);
    if (vulkanTexture == nullptr)
    {
        throw std::runtime_error(
            "Orbit Vulkan received a texture from another backend.");
    }

    EndRenderingIfActive();

    const bool isDepth = vulkanTexture->IsDepth();
    const bool firstUse = !vulkanTexture->EverUsed();
    // Only the *layout* is forced here -- there's genuinely no prior
    // content to preserve on a never-before-used swapchain image, so
    // claiming its real one (from `before`) would lie about that.
    // The stage/access mask, however, must still come from `before`'s
    // real mapping: for a swapchain image that's specifically
    // ResourceState::Present, whose stage now matches where the
    // acquire semaphore's wait is declared (see ToImageBarrierInfo) --
    // hardcoding NONE here instead broke that chain on exactly a
    // swapchain image's first-ever use, which is the one case this
    // branch exists for, and synchronization validation flagged it as
    // a WRITE_AFTER_READ against vkAcquireNextImageKHR.
    const ImageBarrierInfo realSource = ToImageBarrierInfo(before, isDepth);
    const ImageBarrierInfo source =
        firstUse
            ? ImageBarrierInfo{
                VK_IMAGE_LAYOUT_UNDEFINED,
                realSource.stageMask,
                realSource.accessMask}
            : realSource;

    if (firstUse)
    {
        vulkanTexture->MarkUsed();
    }
    const ImageBarrierInfo destination =
        ToImageBarrierInfo(after, isDepth);

    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = source.stageMask;
    barrier.srcAccessMask = source.accessMask;
    barrier.dstStageMask = destination.stageMask;
    barrier.dstAccessMask = destination.accessMask;
    barrier.oldLayout = source.layout;
    barrier.newLayout = destination.layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = vulkanTexture->Native();
    barrier.subresourceRange = {
        isDepth
            ? static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_DEPTH_BIT)
            : static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_COLOR_BIT),
        0, 1, 0, 1};

    VkDependencyInfo dependencyInfo{};
    dependencyInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependencyInfo.imageMemoryBarrierCount = 1;
    dependencyInfo.pImageMemoryBarriers = &barrier;

    vkCmdPipelineBarrier2(nativeCommandList_, &dependencyInfo);
}

void VulkanCommandList::Transition(
    Buffer& buffer,
    const ResourceState before,
    const ResourceState after)
{
    if (before == after)
    {
        return;
    }

    auto* vulkanBuffer = dynamic_cast<VulkanBuffer*>(&buffer);
    if (vulkanBuffer == nullptr)
    {
        throw std::runtime_error(
            "Orbit Vulkan received a buffer from another backend.");
    }

    const bool wasRendering = PauseRenderingIfActive();

    const BufferBarrierInfo source = ToBufferBarrierInfo(before);
    const BufferBarrierInfo destination = ToBufferBarrierInfo(after);

    VkBufferMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
    barrier.srcStageMask = source.stageMask;
    barrier.srcAccessMask = source.accessMask;
    barrier.dstStageMask = destination.stageMask;
    barrier.dstAccessMask = destination.accessMask;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = vulkanBuffer->Native();
    barrier.offset = 0;
    barrier.size = VK_WHOLE_SIZE;

    VkDependencyInfo dependencyInfo{};
    dependencyInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependencyInfo.bufferMemoryBarrierCount = 1;
    dependencyInfo.pBufferMemoryBarriers = &barrier;

    vkCmdPipelineBarrier2(nativeCommandList_, &dependencyInfo);

    ResumeRenderingIfPaused(wasRendering);
}

void VulkanCommandList::UavBarrier(Buffer& buffer)
{
    auto* vulkanBuffer = dynamic_cast<VulkanBuffer*>(&buffer);
    if (vulkanBuffer == nullptr)
    {
        throw std::runtime_error(
            "Orbit Vulkan received a buffer from another backend.");
    }

    const bool wasRendering = PauseRenderingIfActive();

    VkBufferMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.srcAccessMask =
        VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.dstAccessMask =
        VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = vulkanBuffer->Native();
    barrier.offset = 0;
    barrier.size = VK_WHOLE_SIZE;

    VkDependencyInfo dependencyInfo{};
    dependencyInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependencyInfo.bufferMemoryBarrierCount = 1;
    dependencyInfo.pBufferMemoryBarriers = &barrier;

    vkCmdPipelineBarrier2(nativeCommandList_, &dependencyInfo);

    ResumeRenderingIfPaused(wasRendering);
}

void VulkanCommandList::UavBarrier(Texture& texture)
{
    auto* vulkanTexture = dynamic_cast<VulkanTexture*>(&texture);
    if (vulkanTexture == nullptr)
    {
        throw std::runtime_error(
            "Orbit Vulkan received a texture from another backend.");
    }

    const bool wasRendering = PauseRenderingIfActive();

    const bool isDepth = vulkanTexture->IsDepth();

    // A UAV texture is always in GENERAL layout while used as a storage
    // image (see ToImageBarrierInfo's UnorderedAccess case), so this is a
    // same-layout execution/memory dependency, not a transition.
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.srcAccessMask =
        VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    barrier.dstAccessMask =
        VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
        VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = vulkanTexture->Native();
    barrier.subresourceRange = {
        isDepth
            ? static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_DEPTH_BIT)
            : static_cast<VkImageAspectFlags>(VK_IMAGE_ASPECT_COLOR_BIT),
        0, 1, 0, 1};

    VkDependencyInfo dependencyInfo{};
    dependencyInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependencyInfo.imageMemoryBarrierCount = 1;
    dependencyInfo.pImageMemoryBarriers = &barrier;

    vkCmdPipelineBarrier2(nativeCommandList_, &dependencyInfo);

    ResumeRenderingIfPaused(wasRendering);
}

void VulkanCommandList::CopyBuffer(
    Buffer& source,
    const u64 sourceOffsetBytes,
    Buffer& destination,
    const u64 destinationOffsetBytes,
    const u64 sizeBytes)
{
    auto* vulkanSource = dynamic_cast<VulkanBuffer*>(&source);
    auto* vulkanDestination = dynamic_cast<VulkanBuffer*>(&destination);

    if (vulkanSource == nullptr || vulkanDestination == nullptr)
    {
        throw std::runtime_error(
            "Orbit Vulkan received a buffer from another backend.");
    }

    if (sizeBytes == 0)
    {
        return;
    }

    if (sourceOffsetBytes > source.SizeBytes() ||
        sizeBytes > source.SizeBytes() - sourceOffsetBytes ||
        destinationOffsetBytes > destination.SizeBytes() ||
        sizeBytes >
            destination.SizeBytes() - destinationOffsetBytes)
    {
        throw std::out_of_range(
            "Orbit buffer copy exceeds source or destination bounds.");
    }

    const bool wasRendering = PauseRenderingIfActive();

    VkBufferCopy2 region{};
    region.sType = VK_STRUCTURE_TYPE_BUFFER_COPY_2;
    region.srcOffset = sourceOffsetBytes;
    region.dstOffset = destinationOffsetBytes;
    region.size = sizeBytes;

    VkCopyBufferInfo2 copyInfo{};
    copyInfo.sType = VK_STRUCTURE_TYPE_COPY_BUFFER_INFO_2;
    copyInfo.srcBuffer = vulkanSource->Native();
    copyInfo.dstBuffer = vulkanDestination->Native();
    copyInfo.regionCount = 1;
    copyInfo.pRegions = &region;

    vkCmdCopyBuffer2(nativeCommandList_, &copyInfo);

    ResumeRenderingIfPaused(wasRendering);
}

void VulkanCommandList::CopyBufferToTexture(
    Buffer& source,
    const u64 sourceOffsetBytes,
    Texture& destination)
{
    auto* vulkanSource = dynamic_cast<VulkanBuffer*>(&source);
    auto* vulkanDestination = dynamic_cast<VulkanTexture*>(&destination);

    if (vulkanSource == nullptr || vulkanDestination == nullptr)
    {
        throw std::runtime_error(
            "Orbit Vulkan received a resource from another backend.");
    }

    const u32 bytesPerTexel =
        TextureFormatBytesPerTexel(
            vulkanDestination->Format());

    if (bytesPerTexel == 0U)
    {
        throw std::invalid_argument(
            "Orbit cannot determine texture upload texel size.");
    }

    const u64 sizeBytes =
        static_cast<u64>(vulkanDestination->Width()) *
        static_cast<u64>(vulkanDestination->Height()) *
        bytesPerTexel;

    if (sourceOffsetBytes > source.SizeBytes() ||
        sizeBytes > source.SizeBytes() - sourceOffsetBytes)
    {
        throw std::out_of_range(
            "Orbit buffer-to-texture copy exceeds the source buffer's "
            "bounds.");
    }

    const bool wasRendering = PauseRenderingIfActive();

    VkBufferImageCopy2 region{};
    region.sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2;
    region.bufferOffset = sourceOffsetBytes;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageExtent.width = vulkanDestination->Width();
    region.imageExtent.height = vulkanDestination->Height();
    region.imageExtent.depth = 1;

    VkCopyBufferToImageInfo2 copyInfo{};
    copyInfo.sType = VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2;
    copyInfo.srcBuffer = vulkanSource->Native();
    copyInfo.dstImage = vulkanDestination->Native();
    copyInfo.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    copyInfo.regionCount = 1;
    copyInfo.pRegions = &region;

    vkCmdCopyBufferToImage2(nativeCommandList_, &copyInfo);

    ResumeRenderingIfPaused(wasRendering);
}

void VulkanCommandList::CopyTextureToBuffer(
    Texture& source,
    Buffer& destination,
    const u64 destinationOffsetBytes)
{
    auto* vulkanSource =
        dynamic_cast<VulkanTexture*>(&source);
    auto* vulkanDestination =
        dynamic_cast<VulkanBuffer*>(&destination);

    if (vulkanSource == nullptr ||
        vulkanDestination == nullptr)
    {
        throw std::runtime_error(
            "Orbit Vulkan received a resource from another backend.");
    }

    if (source.Format() !=
            TextureFormat::RGBA8_UNorm &&
        source.Format() !=
            TextureFormat::RGBA16_Float)
    {
        throw std::invalid_argument(
            "Orbit texture readback supports RGBA8_UNorm and RGBA16_Float color targets.");
    }

    const u64 sizeBytes =
        static_cast<u64>(
            source.Width()) *
        static_cast<u64>(
            source.Height()) *
        TextureFormatBytesPerTexel(
            source.Format());

    if (destinationOffsetBytes >
            destination.SizeBytes() ||
        sizeBytes >
            destination.SizeBytes() -
                destinationOffsetBytes)
    {
        throw std::out_of_range(
            "Orbit texture-to-buffer copy exceeds the destination buffer bounds.");
    }

    const bool wasRendering =
        PauseRenderingIfActive();

    VkBufferImageCopy2 region{};
    region.sType =
        VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2;
    region.bufferOffset =
        destinationOffsetBytes;
    region.imageSubresource.aspectMask =
        VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageExtent.width =
        source.Width();
    region.imageExtent.height =
        source.Height();
    region.imageExtent.depth = 1;

    VkCopyImageToBufferInfo2 copyInfo{};
    copyInfo.sType =
        VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2;
    copyInfo.srcImage =
        vulkanSource->Native();
    copyInfo.srcImageLayout =
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    copyInfo.dstBuffer =
        vulkanDestination->Native();
    copyInfo.regionCount = 1;
    copyInfo.pRegions = &region;

    vkCmdCopyImageToBuffer2(
        nativeCommandList_,
        &copyInfo);

    ResumeRenderingIfPaused(
        wasRendering);
}

void VulkanCommandList::ClearColorTarget(
    Texture& texture,
    const ClearColor& color)
{
    auto* vulkanTexture = dynamic_cast<VulkanTexture*>(&texture);
    if (vulkanTexture == nullptr)
    {
        throw std::runtime_error(
            "Orbit Vulkan received a texture from another backend.");
    }

    // Deferred: Vulkan dynamic rendering's load-op-clear is the
    // correct way to express this, and applied the next time this
    // texture is bound -- see VulkanTexture::SetPendingClear and
    // SetRenderTarget(s) below.
    VkClearValue value{};
    value.color = {{color.red, color.green, color.blue, color.alpha}};
    vulkanTexture->SetPendingClear(value);
}

void VulkanCommandList::ClearDepthTarget(
    Texture& texture,
    const f32 depth)
{
    auto* vulkanTexture = dynamic_cast<VulkanTexture*>(&texture);
    if (vulkanTexture == nullptr)
    {
        throw std::runtime_error(
            "Orbit Vulkan received a texture from another backend.");
    }

    VkClearValue value{};
    value.depthStencil = {depth, 0};
    vulkanTexture->SetPendingClear(value);
}

void VulkanCommandList::SetRenderTarget(Texture& texture)
{
    auto* vulkanTexture = dynamic_cast<VulkanTexture*>(&texture);
    if (vulkanTexture == nullptr)
    {
        throw std::runtime_error(
            "Orbit Vulkan received a texture from another backend.");
    }

    const auto pendingClear = vulkanTexture->TakePendingClear();

    VkRenderingAttachmentInfo colorAttachment{};
    colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAttachment.imageView = vulkanTexture->View();
    colorAttachment.imageLayout =
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.loadOp =
        pendingClear.has_value()
            ? VK_ATTACHMENT_LOAD_OP_CLEAR
            : VK_ATTACHMENT_LOAD_OP_LOAD;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    if (pendingClear.has_value())
    {
        colorAttachment.clearValue = *pendingClear;
    }

    const std::array colorAttachments{
        colorAttachment
    };
    BeginRendering(
        colorAttachments,
        nullptr,
        vulkanTexture->Width(),
        vulkanTexture->Height());
}

void VulkanCommandList::SetRenderTargets(
    Texture& color,
    Texture& depth)
{
    auto* vulkanColor = dynamic_cast<VulkanTexture*>(&color);
    auto* vulkanDepth = dynamic_cast<VulkanTexture*>(&depth);

    if (vulkanColor == nullptr || vulkanDepth == nullptr)
    {
        throw std::runtime_error(
            "Orbit Vulkan received incompatible color/depth render "
            "targets.");
    }

    const auto pendingColorClear = vulkanColor->TakePendingClear();
    const auto pendingDepthClear = vulkanDepth->TakePendingClear();

    VkRenderingAttachmentInfo colorAttachment{};
    colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAttachment.imageView = vulkanColor->View();
    colorAttachment.imageLayout =
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.loadOp =
        pendingColorClear.has_value()
            ? VK_ATTACHMENT_LOAD_OP_CLEAR
            : VK_ATTACHMENT_LOAD_OP_LOAD;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    if (pendingColorClear.has_value())
    {
        colorAttachment.clearValue = *pendingColorClear;
    }

    VkRenderingAttachmentInfo depthAttachment{};
    depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depthAttachment.imageView = vulkanDepth->View();
    depthAttachment.imageLayout =
        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depthAttachment.loadOp =
        pendingDepthClear.has_value()
            ? VK_ATTACHMENT_LOAD_OP_CLEAR
            : VK_ATTACHMENT_LOAD_OP_LOAD;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    if (pendingDepthClear.has_value())
    {
        depthAttachment.clearValue = *pendingDepthClear;
    }

    const std::array colorAttachments{
        colorAttachment
    };
    BeginRendering(
        colorAttachments,
        &depthAttachment,
        vulkanColor->Width(),
        vulkanColor->Height());
}

void VulkanCommandList::SetRenderTargets(
    const std::span<Texture* const> colors,
    Texture* depth)
{
    if (colors.empty() || colors.size() > 4U)
    {
        throw std::invalid_argument(
            "Orbit Vulkan MRT requires between one and four color targets.");
    }

    std::array<VkRenderingAttachmentInfo, 4> nativeColors{};
    u32 width = 0U;
    u32 height = 0U;

    for (std::size_t index = 0; index < colors.size(); ++index)
    {
        auto* texture =
            dynamic_cast<VulkanTexture*>(colors[index]);
        if (texture == nullptr)
        {
            throw std::runtime_error(
                "Orbit Vulkan received an incompatible MRT color target.");
        }

        if (index == 0U)
        {
            width = texture->Width();
            height = texture->Height();
        }
        else if (texture->Width() != width ||
                 texture->Height() != height)
        {
            throw std::invalid_argument(
                "Orbit Vulkan MRT color targets must have equal extents.");
        }

        const auto pendingClear =
            texture->TakePendingClear();

        auto& attachment = nativeColors[index];
        attachment.sType =
            VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        attachment.imageView = texture->View();
        attachment.imageLayout =
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachment.loadOp =
            pendingClear.has_value()
                ? VK_ATTACHMENT_LOAD_OP_CLEAR
                : VK_ATTACHMENT_LOAD_OP_LOAD;
        attachment.storeOp =
            VK_ATTACHMENT_STORE_OP_STORE;

        if (pendingClear.has_value())
        {
            attachment.clearValue = *pendingClear;
        }
    }

    VkRenderingAttachmentInfo depthAttachment{};
    VkRenderingAttachmentInfo* depthPtr = nullptr;

    if (depth != nullptr)
    {
        auto* nativeDepth =
            dynamic_cast<VulkanTexture*>(depth);
        if (nativeDepth == nullptr)
        {
            throw std::runtime_error(
                "Orbit Vulkan received an incompatible MRT depth target.");
        }
        if (nativeDepth->Width() != width ||
            nativeDepth->Height() != height)
        {
            throw std::invalid_argument(
                "Orbit Vulkan MRT depth target must match color extents.");
        }

        const auto pendingDepthClear =
            nativeDepth->TakePendingClear();

        depthAttachment.sType =
            VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depthAttachment.imageView =
            nativeDepth->View();
        depthAttachment.imageLayout =
            VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depthAttachment.loadOp =
            pendingDepthClear.has_value()
                ? VK_ATTACHMENT_LOAD_OP_CLEAR
                : VK_ATTACHMENT_LOAD_OP_LOAD;
        depthAttachment.storeOp =
            VK_ATTACHMENT_STORE_OP_STORE;
        if (pendingDepthClear.has_value())
        {
            depthAttachment.clearValue =
                *pendingDepthClear;
        }
        depthPtr = &depthAttachment;
    }

    BeginRendering(
        std::span<const VkRenderingAttachmentInfo>(
            nativeColors.data(),
            colors.size()),
        depthPtr,
        width,
        height);
}

void VulkanCommandList::SetRenderTargetsReadOnlyDepth(
    const std::span<Texture* const> colors,
    Texture& depth)
{
    if (colors.empty() || colors.size() > 4U)
        throw std::invalid_argument("Orbit Vulkan read-only-depth MRT requires one to four color targets.");

    std::array<VkRenderingAttachmentInfo, 4> nativeColors{};
    u32 width=0U, height=0U;
    for(std::size_t i=0;i<colors.size();++i)
    {
        auto* texture=dynamic_cast<VulkanTexture*>(colors[i]);
        if(texture==nullptr) throw std::runtime_error("Orbit Vulkan received an incompatible MRT color target.");
        if(i==0U){width=texture->Width();height=texture->Height();}
        else if(texture->Width()!=width||texture->Height()!=height) throw std::invalid_argument("Orbit Vulkan MRT color targets must have equal extents.");
        const auto pending=texture->TakePendingClear();
        auto& a=nativeColors[i]; a.sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO; a.imageView=texture->View(); a.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        a.loadOp=pending.has_value()?VK_ATTACHMENT_LOAD_OP_CLEAR:VK_ATTACHMENT_LOAD_OP_LOAD; a.storeOp=VK_ATTACHMENT_STORE_OP_STORE; if(pending.has_value()) a.clearValue=*pending;
    }
    auto* nativeDepth=dynamic_cast<VulkanTexture*>(&depth);
    if(nativeDepth==nullptr) throw std::runtime_error("Orbit Vulkan received an incompatible read-only depth target.");
    if(nativeDepth->Width()!=width||nativeDepth->Height()!=height) throw std::invalid_argument("Orbit Vulkan read-only depth target must match color extents.");
    VkRenderingAttachmentInfo depthAttachment{}; depthAttachment.sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO; depthAttachment.imageView=nativeDepth->View(); depthAttachment.imageLayout=VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL; depthAttachment.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD; depthAttachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    BeginRendering(std::span<const VkRenderingAttachmentInfo>(nativeColors.data(),colors.size()),&depthAttachment,width,height);
}

void VulkanCommandList::SetViewport(const Viewport& viewport)
{
    // Vulkan's viewport Y axis points down with a positive height,
    // the opposite of D3D's; flipping via a negative height (core
    // since 1.1, VK_KHR_maintenance1) keeps every renderer's existing
    // top-left-origin, Y-down viewport math working unchanged.
    VkViewport nativeViewport{};
    nativeViewport.x = viewport.x;
    nativeViewport.y = viewport.y + viewport.height;
    nativeViewport.width = viewport.width;
    nativeViewport.height = -viewport.height;
    nativeViewport.minDepth = viewport.minDepth;
    nativeViewport.maxDepth = viewport.maxDepth;

    vkCmdSetViewport(nativeCommandList_, 0, 1, &nativeViewport);
}

void VulkanCommandList::SetScissor(const ScissorRect& rect)
{
    VkRect2D nativeRect{};
    nativeRect.offset = {rect.left, rect.top};
    nativeRect.extent = {
        static_cast<u32>(rect.right - rect.left),
        static_cast<u32>(rect.bottom - rect.top)};

    vkCmdSetScissor(nativeCommandList_, 0, 1, &nativeRect);
}

void VulkanCommandList::SetGraphicsPipeline(GraphicsPipeline& pipeline)
{
    auto* vulkanPipeline =
        dynamic_cast<VulkanGraphicsPipeline*>(&pipeline);

    if (vulkanPipeline == nullptr)
    {
        throw std::runtime_error(
            "Orbit Vulkan received a graphics pipeline from another "
            "backend.");
    }

    vkCmdBindPipeline(
        nativeCommandList_,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        vulkanPipeline->Native());

    activePipeline_ = vulkanPipeline;
}

void VulkanCommandList::SetGraphicsConstants(
    const std::span<const u32> dwords)
{
    if (activePipeline_ == nullptr)
    {
        throw std::runtime_error(
            "Orbit cannot bind graphics constants without an active "
            "pipeline.");
    }

    if (dwords.empty())
    {
        return;
    }

    if (dwords.size() > activePipeline_->PushConstantDwords())
    {
        throw std::runtime_error(
            "Orbit graphics constants exceed the active pipeline "
            "push constant range.");
    }

    vkCmdPushConstants(
        nativeCommandList_,
        activePipeline_->Layout(),
        VK_SHADER_STAGE_ALL_GRAPHICS,
        0,
        static_cast<u32>(dwords.size() * sizeof(u32)),
        dwords.data());
}

void VulkanCommandList::SetGraphicsBuffer(
    const u32 slot,
    Buffer& buffer)
{
    if (activePipeline_ == nullptr)
    {
        throw std::runtime_error(
            "Orbit cannot bind a graphics buffer without an active "
            "pipeline.");
    }

    if (slot >= activePipeline_->ShaderResourceBuffers())
    {
        throw std::out_of_range(
            "Orbit graphics SRV slot exceeds the active pipeline "
            "layout.");
    }

    auto* vulkanBuffer = dynamic_cast<VulkanBuffer*>(&buffer);
    if (vulkanBuffer == nullptr)
    {
        throw std::runtime_error(
            "Orbit Vulkan received a shader buffer from another "
            "backend.");
    }

    VkDescriptorBufferInfo bufferInfo{};
    bufferInfo.buffer = vulkanBuffer->Native();
    bufferInfo.offset = 0;
    bufferInfo.range = VK_WHOLE_SIZE;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstBinding = slot;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &bufferInfo;

    functions_->vkCmdPushDescriptorSetKHR(
        nativeCommandList_,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        activePipeline_->Layout(),
        0,
        1,
        &write);
}

void VulkanCommandList::SetGraphicsTexture(
    const u32 slot,
    Texture& texture)
{
    if (activePipeline_ == nullptr)
    {
        throw std::runtime_error(
            "Orbit cannot bind a graphics texture without an active "
            "pipeline.");
    }

    if (slot >= activePipeline_->SampledTextures())
    {
        throw std::out_of_range(
            "Orbit graphics texture slot exceeds the active pipeline "
            "layout.");
    }

    auto* vulkanTexture = dynamic_cast<VulkanTexture*>(&texture);
    if (vulkanTexture == nullptr)
    {
        throw std::runtime_error(
            "Orbit Vulkan received a texture from another backend.");
    }

    VkDescriptorImageInfo imageInfo{};
    imageInfo.sampler = defaultSampler_;
    imageInfo.imageView = vulkanTexture->View();
    imageInfo.imageLayout =
        texture.Format() == TextureFormat::D32_Float
            ? VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL
            : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    // Texture bindings sit right after the buffer bindings in the
    // descriptor set layout -- see GraphicsPipelineDesc::sampledTextures
    // and its construction in VulkanPipeline.cpp.
    write.dstBinding = activePipeline_->ShaderResourceBuffers() + slot;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageInfo;

    functions_->vkCmdPushDescriptorSetKHR(
        nativeCommandList_,
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        activePipeline_->Layout(),
        0,
        1,
        &write);
}

void VulkanCommandList::SetComputePipeline(ComputePipeline& pipeline)
{
    auto* vulkanPipeline =
        dynamic_cast<VulkanComputePipeline*>(&pipeline);

    if (vulkanPipeline == nullptr)
    {
        throw std::runtime_error(
            "Orbit Vulkan received a compute pipeline from another "
            "backend.");
    }

    vkCmdBindPipeline(
        nativeCommandList_,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        vulkanPipeline->Native());

    activeComputePipeline_ = vulkanPipeline;
}

void VulkanCommandList::SetComputeConstants(
    const std::span<const u32> dwords)
{
    if (activeComputePipeline_ == nullptr)
    {
        throw std::runtime_error(
            "Orbit cannot bind compute constants without an active "
            "pipeline.");
    }

    if (dwords.empty())
    {
        return;
    }

    if (dwords.size() > activeComputePipeline_->PushConstantDwords())
    {
        throw std::runtime_error(
            "Orbit compute constants exceed the active pipeline push "
            "constant range.");
    }

    vkCmdPushConstants(
        nativeCommandList_,
        activeComputePipeline_->Layout(),
        VK_SHADER_STAGE_COMPUTE_BIT,
        0,
        static_cast<u32>(dwords.size() * sizeof(u32)),
        dwords.data());
}

void VulkanCommandList::SetComputeBuffer(
    const u32 slot,
    Buffer& buffer)
{
    if (activeComputePipeline_ == nullptr)
    {
        throw std::runtime_error(
            "Orbit cannot bind a compute buffer without an active "
            "pipeline.");
    }

    if (slot >= activeComputePipeline_->ShaderResourceBuffers())
    {
        throw std::out_of_range(
            "Orbit compute SRV slot exceeds the active pipeline "
            "layout.");
    }

    auto* vulkanBuffer = dynamic_cast<VulkanBuffer*>(&buffer);
    if (vulkanBuffer == nullptr)
    {
        throw std::runtime_error(
            "Orbit Vulkan received a shader buffer from another "
            "backend.");
    }

    VkDescriptorBufferInfo bufferInfo{};
    bufferInfo.buffer = vulkanBuffer->Native();
    bufferInfo.offset = 0;
    bufferInfo.range = VK_WHOLE_SIZE;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstBinding = slot;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &bufferInfo;

    functions_->vkCmdPushDescriptorSetKHR(
        nativeCommandList_,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        activeComputePipeline_->Layout(),
        0,
        1,
        &write);
}

void VulkanCommandList::SetComputeStorageTexture(
    const u32 slot,
    Texture& texture)
{
    if (activeComputePipeline_ == nullptr)
    {
        throw std::runtime_error(
            "Orbit cannot bind a compute storage texture without an "
            "active pipeline.");
    }

    if (slot >= activeComputePipeline_->StorageTextures())
    {
        throw std::out_of_range(
            "Orbit compute storage texture slot exceeds the active "
            "pipeline layout.");
    }

    auto* vulkanTexture = dynamic_cast<VulkanTexture*>(&texture);
    if (vulkanTexture == nullptr)
    {
        throw std::runtime_error(
            "Orbit Vulkan received a texture from another backend.");
    }

    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageView = vulkanTexture->View();
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    // Storage images sit right after the buffer bindings -- see
    // ComputePipelineDesc::storageTextures and its layout construction
    // in VulkanPipeline.cpp.
    write.dstBinding =
        activeComputePipeline_->ShaderResourceBuffers() + slot;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    write.pImageInfo = &imageInfo;

    functions_->vkCmdPushDescriptorSetKHR(
        nativeCommandList_,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        activeComputePipeline_->Layout(),
        0,
        1,
        &write);
}

void VulkanCommandList::SetComputeTexture(
    const u32 slot,
    Texture& texture)
{
    if (activeComputePipeline_ == nullptr)
    {
        throw std::runtime_error(
            "Orbit cannot bind a compute texture without an active "
            "pipeline.");
    }

    if (slot >= activeComputePipeline_->SampledTextures())
    {
        throw std::out_of_range(
            "Orbit compute texture slot exceeds the active pipeline "
            "layout.");
    }

    auto* vulkanTexture = dynamic_cast<VulkanTexture*>(&texture);
    if (vulkanTexture == nullptr)
    {
        throw std::runtime_error(
            "Orbit Vulkan received a texture from another backend.");
    }

    VkDescriptorImageInfo imageInfo{};
    imageInfo.sampler = defaultSampler_;
    imageInfo.imageView = vulkanTexture->View();
    imageInfo.imageLayout =
        texture.Format() == TextureFormat::D32_Float
            ? VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL
            : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    // Sampled textures sit after the buffer and storage-image bindings --
    // see ComputePipelineDesc::sampledTextures.
    write.dstBinding =
        activeComputePipeline_->ShaderResourceBuffers() +
        activeComputePipeline_->StorageTextures() +
        slot;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageInfo;

    functions_->vkCmdPushDescriptorSetKHR(
        nativeCommandList_,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        activeComputePipeline_->Layout(),
        0,
        1,
        &write);
}

void VulkanCommandList::SetComputeAccelerationStructure(
    const u32 slot,
    AccelerationStructure& accelerationStructure)
{
    if (activeComputePipeline_ == nullptr)
    {
        throw std::runtime_error(
            "Orbit cannot bind an acceleration structure without an "
            "active compute pipeline.");
    }

    if (slot >= activeComputePipeline_->AccelerationStructures())
    {
        throw std::out_of_range(
            "Orbit compute acceleration-structure slot exceeds the "
            "active pipeline layout.");
    }

    auto* vulkanAccelerationStructure =
        dynamic_cast<VulkanAccelerationStructure*>(
            &accelerationStructure);

    if (vulkanAccelerationStructure == nullptr)
    {
        throw std::runtime_error(
            "Orbit Vulkan received an acceleration structure from "
            "another backend.");
    }

    const VkAccelerationStructureKHR native =
        vulkanAccelerationStructure->TopLevel();

    VkWriteDescriptorSetAccelerationStructureKHR
        accelerationInfo{};
    accelerationInfo.sType =
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
    accelerationInfo.accelerationStructureCount = 1U;
    accelerationInfo.pAccelerationStructures =
        &native;

    VkWriteDescriptorSet write{};
    write.sType =
        VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.pNext =
        &accelerationInfo;
    write.dstBinding =
        activeComputePipeline_->ShaderResourceBuffers() +
        activeComputePipeline_->StorageTextures() +
        activeComputePipeline_->SampledTextures() +
        slot;
    write.descriptorCount = 1U;
    write.descriptorType =
        VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;

    functions_->vkCmdPushDescriptorSetKHR(
        nativeCommandList_,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        activeComputePipeline_->Layout(),
        0,
        1,
        &write);
}

void VulkanCommandList::Dispatch(
    const u32 groupCountX,
    const u32 groupCountY,
    const u32 groupCountZ)
{
    if (activeComputePipeline_ == nullptr)
    {
        throw std::runtime_error(
            "Orbit cannot dispatch without an active compute pipeline.");
    }

    // vkCmdDispatch must not be recorded inside a dynamic-rendering scope
    // (same restriction as the barrier/copy commands PauseRenderingIfActive
    // already exists for -- see its comment) -- pause/resume around it for
    // correctness regardless of call-site ordering, even though every
    // current caller dispatches before the frame's first SetRenderTarget.
    const bool wasRendering = PauseRenderingIfActive();

    vkCmdDispatch(
        nativeCommandList_, groupCountX, groupCountY, groupCountZ);

    ResumeRenderingIfPaused(wasRendering);
}

void VulkanCommandList::SetVertexBuffer(
    Buffer& buffer,
    const u32 strideBytes)
{
    // Unlike D3D12, Vulkan bakes the vertex stride into the pipeline
    // (GraphicsPipelineDesc::vertexStrideBytes, see VulkanPipeline.cpp)
    // rather than taking it at bind time, since every pipeline in
    // this codebase only ever binds one fixed-layout vertex buffer;
    // strideBytes is only validated here, not otherwise used.
    auto* vulkanBuffer = dynamic_cast<VulkanBuffer*>(&buffer);
    if (vulkanBuffer == nullptr)
    {
        throw std::runtime_error(
            "Orbit Vulkan received a vertex buffer from another "
            "backend.");
    }

    if (strideBytes == 0)
    {
        throw std::invalid_argument(
            "Orbit vertex buffer stride cannot be zero.");
    }

    const VkBuffer nativeBuffer = vulkanBuffer->Native();
    constexpr VkDeviceSize offset = 0;

    vkCmdBindVertexBuffers(
        nativeCommandList_, 0, 1, &nativeBuffer, &offset);
}

void VulkanCommandList::SetIndexBuffer(
    Buffer& buffer,
    const IndexFormat format)
{
    auto* vulkanBuffer = dynamic_cast<VulkanBuffer*>(&buffer);
    if (vulkanBuffer == nullptr)
    {
        throw std::runtime_error(
            "Orbit Vulkan received an index buffer from another "
            "backend.");
    }

    const VkIndexType indexType =
        format == IndexFormat::UInt16
            ? VK_INDEX_TYPE_UINT16
            : VK_INDEX_TYPE_UINT32;

    vkCmdBindIndexBuffer(
        nativeCommandList_, vulkanBuffer->Native(), 0, indexType);
}

void VulkanCommandList::DrawIndexed(
    const u32 indexCount,
    const u32 firstIndex,
    const i32 vertexOffset)
{
    vkCmdDrawIndexed(
        nativeCommandList_, indexCount, 1, firstIndex, vertexOffset, 0);
}

void VulkanCommandList::Draw(
    const u32 vertexCount,
    const u32 firstVertex)
{
    vkCmdDraw(nativeCommandList_, vertexCount, 1, firstVertex, 0);
}

void VulkanCommandList::DrawIndirect(
    Buffer& argumentBuffer,
    const u64 argumentOffsetBytes)
{
    auto* vulkanBuffer = dynamic_cast<VulkanBuffer*>(&argumentBuffer);
    if (vulkanBuffer == nullptr)
    {
        throw std::runtime_error(
            "Orbit Vulkan received an indirect argument buffer from another backend.");
    }
    if (argumentBuffer.Usage() != BufferUsage::Indirect)
    {
        throw std::invalid_argument(
            "Orbit DrawIndirect requires BufferUsage::Indirect.");
    }
    if ((argumentOffsetBytes % 4U) != 0U ||
        argumentOffsetBytes + sizeof(VkDrawIndirectCommand) > argumentBuffer.SizeBytes())
    {
        throw std::out_of_range(
            "Orbit DrawIndirect argument range exceeds the supplied buffer.");
    }
    vkCmdDrawIndirect(
        nativeCommandList_,
        vulkanBuffer->Native(),
        static_cast<VkDeviceSize>(argumentOffsetBytes),
        1U,
        sizeof(VkDrawIndirectCommand));
}

void VulkanCommandList::ResetTimestampQueryPool(
    TimestampQueryPool& pool,
    const u32 firstQuery,
    const u32 count)
{
    auto* vulkanPool = dynamic_cast<VulkanTimestampQueryPool*>(&pool);
    if (vulkanPool == nullptr)
    {
        throw std::runtime_error(
            "Orbit Vulkan received a timestamp query pool from another "
            "backend.");
    }

    // vkCmdResetQueryPool must not be recorded inside a dynamic-rendering
    // scope (same restriction as barriers/copies -- see the note on
    // PauseRenderingIfActive above), but every call site in this
    // codebase resets right after Reset(), before the frame's first
    // SetRenderTarget(s), so this is never actually mid-render-pass in
    // practice; pause/resume isn't worth the complexity here.
    vkCmdResetQueryPool(
        nativeCommandList_, vulkanPool->Native(), firstQuery, count);
}

void VulkanCommandList::WriteTimestamp(
    TimestampQueryPool& pool,
    const u32 query)
{
    auto* vulkanPool = dynamic_cast<VulkanTimestampQueryPool*>(&pool);
    if (vulkanPool == nullptr)
    {
        throw std::runtime_error(
            "Orbit Vulkan received a timestamp query pool from another "
            "backend.");
    }

    // BOTTOM_OF_PIPE: the timestamp is written only once every command
    // submitted before this one has finished executing, which is what
    // you want for both ends of a "how long did this pass take" region
    // (TOP_OF_PIPE for the start mark would fire before earlier
    // in-flight work drains, understating a pass that stalls behind the
    // one before it).
    vkCmdWriteTimestamp2(
        nativeCommandList_,
        VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
        vulkanPool->Native(),
        query);
}

void VulkanCommandList::Close()
{
    EndRenderingIfActive();

    if (vkEndCommandBuffer(nativeCommandList_) != VK_SUCCESS)
    {
        throw std::runtime_error(
            "Orbit failed to close a Vulkan command buffer.");
    }
}

VkCommandBuffer VulkanCommandList::Native() const noexcept
{
    return nativeCommandList_;
}
} // namespace orbit::rhi::vulkan::detail
