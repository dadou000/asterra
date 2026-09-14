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
        return {
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_PIPELINE_STAGE_2_NONE,
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
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};

    case ResourceState::DepthRead:
        return {
            isDepth
                ? VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL
                : VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                VK_ACCESS_2_SHADER_READ_BIT};

    case ResourceState::ShaderResource:
        return {
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
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
                VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
            VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT |
                VK_ACCESS_2_UNIFORM_READ_BIT};

    case ResourceState::IndexBuffer:
        return {
            VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT,
            VK_ACCESS_2_INDEX_READ_BIT};

    case ResourceState::ShaderResource:
        return {
            VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
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
    const DeviceFunctions& functions)
    : device_(device),
      type_(type),
      pool_(pool),
      nativeCommandList_(nativeCommandList),
      functions_(&functions)
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
    VkRenderingAttachmentInfo* colorAttachment,
    VkRenderingAttachmentInfo* depthAttachment,
    const u32 width,
    const u32 height)
{
    EndRenderingIfActive();

    // Remember what's being bound (forcing loadOp to LOAD) so a later
    // CopyBuffer on this command list -- which cannot be recorded
    // inside a dynamic-rendering scope -- can pause and transparently
    // resume rendering around the copy without ever re-clearing.
    if (colorAttachment != nullptr)
    {
        pausedColorAttachment_ = *colorAttachment;
        pausedColorAttachment_->loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    }
    else
    {
        pausedColorAttachment_.reset();
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

    if (colorAttachment != nullptr)
    {
        renderingInfo.colorAttachmentCount = 1;
        renderingInfo.pColorAttachments = colorAttachment;
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
        pausedColorAttachment_.has_value()
            ? &*pausedColorAttachment_
            : nullptr,
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
    const ImageBarrierInfo source =
        firstUse
            ? ImageBarrierInfo{
                VK_IMAGE_LAYOUT_UNDEFINED,
                VK_PIPELINE_STAGE_2_NONE,
                VK_ACCESS_2_NONE}
            : ToImageBarrierInfo(before, isDepth);

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

    BeginRendering(
        &colorAttachment,
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

    BeginRendering(
        &colorAttachment,
        &depthAttachment,
        vulkanColor->Width(),
        vulkanColor->Height());
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
