#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include <vma/vk_mem_alloc.h>

#include <orbit/rhi/Device.hpp>
#include <orbit/rhi/vulkan/RenderDocCapture.hpp>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace orbit::rhi::vulkan::detail
{
// The one push-descriptor-capable device function this codebase relies
// on: VK_KHR_push_descriptor is not promoted to Vulkan core (as of 1.3
// and 1.4), so its entry point is never exported by name from
// vulkan-1.lib and must be resolved per-device via vkGetDeviceProcAddr.
// Every other Vulkan call this backend makes (dynamic rendering,
// synchronization2, timeline semaphores) is core in the 1.3 baseline
// this device requires and links directly.
struct DeviceFunctions
{
    PFN_vkCmdPushDescriptorSetKHR vkCmdPushDescriptorSetKHR{nullptr};
};

// Maps an abstract ResourceState to how a *texture* attachment/image
// should be described for a pipeline barrier: image layout, the
// pipeline stage(s) that can be waiting on or producing it, and the
// memory access type(s) involved. See BufferBarrierInfo below for the
// buffer equivalent (buffers have no layout).
struct ImageBarrierInfo
{
    VkImageLayout layout{VK_IMAGE_LAYOUT_UNDEFINED};
    VkPipelineStageFlags2 stageMask{VK_PIPELINE_STAGE_2_NONE};
    VkAccessFlags2 accessMask{VK_ACCESS_2_NONE};
};

struct BufferBarrierInfo
{
    VkPipelineStageFlags2 stageMask{VK_PIPELINE_STAGE_2_NONE};
    VkAccessFlags2 accessMask{VK_ACCESS_2_NONE};
};

[[nodiscard]] ImageBarrierInfo ToImageBarrierInfo(
    ResourceState state,
    bool isDepth);

[[nodiscard]] BufferBarrierInfo ToBufferBarrierInfo(ResourceState state);

// Runs a one-off, blocking image layout transition on the graphics
// queue, used both for a freshly created VkImage (always born
// VK_IMAGE_LAYOUT_UNDEFINED in Vulkan) to reach TextureDesc::
// initialState the way D3D12's CreateCommittedResource does directly,
// and for pre-warming a swapchain image into PRESENT_SRC_KHR right
// after creation so the very first per-frame Transition(Present ->
// RenderTarget) call's claimed prior layout is actually true. See
// VulkanResources.cpp.
void TransitionNewImageBlocking(
    VkDevice device,
    u32 graphicsFamilyIndex,
    VkImage image,
    VkImageAspectFlags aspectMask,
    ResourceState targetState);

[[nodiscard]] VkFormat ToNativeVertexFormat(VertexFormat format);
[[nodiscard]] VkFormat ToNativeTextureFormat(TextureFormat format);
[[nodiscard]] VkCullModeFlags ToNativeCullMode(CullMode mode);
[[nodiscard]] VkPolygonMode ToNativeFillMode(FillMode mode);
[[nodiscard]] VkCompareOp ToNativeDepthCompare(DepthCompare compare);
[[nodiscard]] VkPrimitiveTopology ToNativeTopology(PrimitiveTopology topology);

class VulkanFence final : public Fence
{
public:
    VulkanFence(VkDevice device, VkSemaphore timelineSemaphore);
    ~VulkanFence() override;

    VulkanFence(const VulkanFence&) = delete;
    VulkanFence& operator=(const VulkanFence&) = delete;

    [[nodiscard]] u64 CompletedValue() const noexcept override;
    void Wait(u64 value) override;

    [[nodiscard]] VkSemaphore Native() const noexcept;

private:
    VkDevice device_{VK_NULL_HANDLE};
    VkSemaphore semaphore_{VK_NULL_HANDLE};
};

class VulkanSwapchain;

class VulkanQueue final : public Queue
{
public:
    VulkanQueue(
        VkDevice device,
        VkQueue nativeQueue,
        u32 familyIndex,
        QueueType type,
        const DeviceFunctions& functions);

    [[nodiscard]] QueueType Type() const noexcept override;
    void Submit(CommandList& commandList) override;
    void Signal(Fence& fence, u64 value) override;

    [[nodiscard]] VkQueue Native() const noexcept;
    [[nodiscard]] u32 FamilyIndex() const noexcept;
    [[nodiscard]] const DeviceFunctions& Functions() const noexcept;

    // Called by VulkanSwapchain immediately after acquiring an image,
    // so the *next* Submit() on this queue waits on the image-available
    // semaphore, signals the matching render-finished semaphore, and
    // signals imageAvailableRetired on completion -- see the "Swapchain
    // sync" note in VulkanSwapchain.cpp. Only one swapchain/present
    // pair is ever pending on a queue at a time in this application.
    void SetPendingSwapchainSync(
        VkSemaphore waitForImageAvailable,
        VkSemaphore signalRenderFinished,
        VkFence imageAvailableRetired);

private:
    VkDevice device_{VK_NULL_HANDLE};
    VkQueue nativeQueue_{VK_NULL_HANDLE};
    u32 familyIndex_{0};
    QueueType type_{QueueType::Graphics};
    const DeviceFunctions* functions_{nullptr};

    VkSemaphore pendingImageAvailable_{VK_NULL_HANDLE};
    VkSemaphore pendingRenderFinished_{VK_NULL_HANDLE};
    VkFence pendingImageAvailableRetired_{VK_NULL_HANDLE};
};

class VulkanBuffer final : public Buffer
{
public:
    VulkanBuffer(
        VmaAllocator allocator,
        VkBuffer nativeBuffer,
        VmaAllocation allocation,
        BufferDesc desc);
    ~VulkanBuffer() override;

    VulkanBuffer(const VulkanBuffer&) = delete;
    VulkanBuffer& operator=(const VulkanBuffer&) = delete;

    [[nodiscard]] u64 SizeBytes() const noexcept override;
    [[nodiscard]] BufferUsage Usage() const noexcept override;
    [[nodiscard]] MemoryUsage Memory() const noexcept override;

    [[nodiscard]] std::byte* Map() override;
    void Unmap() override;

    [[nodiscard]] VkBuffer Native() const noexcept;

private:
    VmaAllocator allocator_{nullptr};
    VkBuffer nativeBuffer_{VK_NULL_HANDLE};
    VmaAllocation allocation_{nullptr};
    BufferDesc desc_{};
    bool mapped_{false};
};

class VulkanTexture final : public Texture
{
public:
    // ownsResources is false for swapchain-provided images: the
    // swapchain owns the VkImage lifetime, this wrapper only owns the
    // VkImageView it creates for its own use.
    VulkanTexture(
        VkDevice device,
        VmaAllocator allocator,
        VkImage nativeImage,
        VmaAllocation allocation,
        VkImageView imageView,
        u32 width,
        u32 height,
        TextureFormat format,
        bool ownsImage);
    ~VulkanTexture() override;

    VulkanTexture(const VulkanTexture&) = delete;
    VulkanTexture& operator=(const VulkanTexture&) = delete;

    [[nodiscard]] u32 Width() const noexcept override;
    [[nodiscard]] u32 Height() const noexcept override;
    [[nodiscard]] TextureFormat Format() const noexcept override;

    [[nodiscard]] VkImage Native() const noexcept;
    [[nodiscard]] VkImageView View() const noexcept;
    [[nodiscard]] bool IsDepth() const noexcept;

    // A clear requested via ClearColorTarget/ClearDepthTarget is
    // deferred until the next time this texture is bound as a render
    // target: Vulkan dynamic rendering's load-op-clear is the correct
    // way to express "clear then draw," and every call site in this
    // codebase already clears immediately before binding (see
    // VulkanCommands.cpp).
    void SetPendingClear(const VkClearValue& value) noexcept;
    [[nodiscard]] std::optional<VkClearValue> TakePendingClear() noexcept;

    // A swapchain-provided image starts life owned by the presentation
    // engine, not this backend -- any use before it has actually been
    // acquired (including a layout-transition barrier) is a validation
    // error, so it cannot be pre-warmed into its claimed initial state
    // the way a regular CreateTexture() result is (see
    // TransitionNewImageBlocking). VulkanCommandList::Transition
    // consults this to treat such an image's *true* first-ever
    // transition as coming from UNDEFINED regardless of what
    // ResourceState the caller claims as "before".
    [[nodiscard]] bool EverUsed() const noexcept;
    void MarkUsed() noexcept;

private:
    VkDevice device_{VK_NULL_HANDLE};
    VmaAllocator allocator_{nullptr};
    VkImage nativeImage_{VK_NULL_HANDLE};
    VmaAllocation allocation_{nullptr};
    VkImageView imageView_{VK_NULL_HANDLE};
    u32 width_{};
    u32 height_{};
    TextureFormat format_{TextureFormat::RGBA8_UNorm};
    bool ownsImage_{true};
    std::optional<VkClearValue> pendingClear_{};
    bool everUsed_{true};
};

class VulkanTimestampQueryPool final : public TimestampQueryPool
{
public:
    VulkanTimestampQueryPool(
        VkDevice device,
        VkQueryPool pool,
        u32 count);
    ~VulkanTimestampQueryPool() override;

    VulkanTimestampQueryPool(const VulkanTimestampQueryPool&) = delete;
    VulkanTimestampQueryPool& operator=(
        const VulkanTimestampQueryPool&) = delete;

    [[nodiscard]] u32 Count() const noexcept override;

    [[nodiscard]] bool TryGetResults(
        u32 first,
        u32 count,
        u64* outTicks) const override;

    [[nodiscard]] VkQueryPool Native() const noexcept;

private:
    VkDevice device_{VK_NULL_HANDLE};
    VkQueryPool pool_{VK_NULL_HANDLE};
    u32 count_{0};
};

class VulkanGraphicsPipeline final : public GraphicsPipeline
{
public:
    VulkanGraphicsPipeline(
        VkDevice device,
        VkPipeline pipeline,
        VkPipelineLayout layout,
        VkDescriptorSetLayout descriptorSetLayout,
        u32 pushConstantDwords,
        u32 shaderResourceBuffers,
        PrimitiveTopology topology);
    ~VulkanGraphicsPipeline() override;

    VulkanGraphicsPipeline(const VulkanGraphicsPipeline&) = delete;
    VulkanGraphicsPipeline& operator=(const VulkanGraphicsPipeline&) = delete;

    [[nodiscard]] u32 PushConstantDwords() const noexcept override;
    [[nodiscard]] u32 ShaderResourceBuffers() const noexcept override;
    [[nodiscard]] PrimitiveTopology Topology() const noexcept override;

    [[nodiscard]] VkPipeline Native() const noexcept;
    [[nodiscard]] VkPipelineLayout Layout() const noexcept;

private:
    VkDevice device_{VK_NULL_HANDLE};
    VkPipeline pipeline_{VK_NULL_HANDLE};
    VkPipelineLayout layout_{VK_NULL_HANDLE};
    VkDescriptorSetLayout descriptorSetLayout_{VK_NULL_HANDLE};
    u32 pushConstantDwords_{0};
    u32 shaderResourceBuffers_{0};
    PrimitiveTopology topology_{PrimitiveTopology::TriangleList};
};

class VulkanCommandAllocator final : public CommandAllocator
{
public:
    VulkanCommandAllocator(
        VkDevice device,
        QueueType type,
        VkCommandPool pool);
    ~VulkanCommandAllocator() override;

    VulkanCommandAllocator(const VulkanCommandAllocator&) = delete;
    VulkanCommandAllocator& operator=(const VulkanCommandAllocator&) = delete;

    [[nodiscard]] QueueType Type() const noexcept override;
    void Reset() override;

    [[nodiscard]] VkCommandPool Native() const noexcept;

private:
    VkDevice device_{VK_NULL_HANDLE};
    QueueType type_{QueueType::Graphics};
    VkCommandPool pool_{VK_NULL_HANDLE};
};

class VulkanCommandList final : public CommandList
{
public:
    VulkanCommandList(
        VkDevice device,
        QueueType type,
        VkCommandPool pool,
        VkCommandBuffer nativeCommandList,
        const DeviceFunctions& functions);

    [[nodiscard]] QueueType Type() const noexcept override;

    void Reset(CommandAllocator& allocator) override;

    void Transition(
        Texture& texture,
        ResourceState before,
        ResourceState after) override;

    void Transition(
        Buffer& buffer,
        ResourceState before,
        ResourceState after) override;

    void CopyBuffer(
        Buffer& source,
        u64 sourceOffsetBytes,
        Buffer& destination,
        u64 destinationOffsetBytes,
        u64 sizeBytes) override;

    void ClearColorTarget(
        Texture& texture,
        const ClearColor& color) override;

    void ClearDepthTarget(
        Texture& texture,
        f32 depth) override;

    void SetRenderTarget(Texture& texture) override;

    void SetRenderTargets(
        Texture& color,
        Texture& depth) override;

    void SetViewport(const Viewport& viewport) override;
    void SetScissor(const ScissorRect& rect) override;

    void SetGraphicsPipeline(
        GraphicsPipeline& pipeline) override;

    void SetGraphicsConstants(
        std::span<const u32> dwords) override;

    void SetGraphicsBuffer(
        u32 slot,
        Buffer& buffer) override;

    void SetVertexBuffer(
        Buffer& buffer,
        u32 strideBytes) override;

    void SetIndexBuffer(
        Buffer& buffer,
        IndexFormat format) override;

    void DrawIndexed(
        u32 indexCount,
        u32 firstIndex,
        i32 vertexOffset) override;

    void Draw(
        u32 vertexCount,
        u32 firstVertex) override;

    void ResetTimestampQueryPool(
        TimestampQueryPool& pool,
        u32 firstQuery,
        u32 count) override;

    void WriteTimestamp(
        TimestampQueryPool& pool,
        u32 query) override;

    void Close() override;

    [[nodiscard]] VkCommandBuffer Native() const noexcept;

private:
    void EndRenderingIfActive();
    void BeginRendering(
        VkRenderingAttachmentInfo* colorAttachment,
        VkRenderingAttachmentInfo* depthAttachment,
        u32 width,
        u32 height);

    // Without VK_KHR_dynamic_rendering_local_read, *no*
    // vkCmdPipelineBarrier2 (buffer or image) or copy command can be
    // recorded inside a dynamic-rendering scope at all -- but the
    // renderers in this codebase freely interleave streaming buffer
    // uploads (a barrier pair around a copy) with draw calls against
    // render targets Main.cpp already bound before calling into them.
    // These two bracket such a non-draw command: pause rendering
    // (returning whether it had been active), then resume it
    // afterward with the same attachments (as a LOAD, never
    // re-clearing) rather than leaving it closed for the next draw
    // call to fail against.
    [[nodiscard]] bool PauseRenderingIfActive();
    void ResumeRenderingIfPaused(bool wasRendering);
    std::optional<VkRenderingAttachmentInfo> pausedColorAttachment_;
    std::optional<VkRenderingAttachmentInfo> pausedDepthAttachment_;
    u32 pausedWidth_{0};
    u32 pausedHeight_{0};

    VkDevice device_{VK_NULL_HANDLE};
    QueueType type_;
    VkCommandPool pool_{VK_NULL_HANDLE};
    VkCommandBuffer nativeCommandList_{VK_NULL_HANDLE};
    const DeviceFunctions* functions_{nullptr};
    VulkanGraphicsPipeline* activePipeline_{nullptr};
    bool renderingActive_{false};

    // One command buffer per pool this list has ever been Reset()
    // onto (see Reset()'s comment), never freed early -- destroying a
    // VkCommandPool implicitly frees every buffer allocated from it,
    // so ownership is left entirely to VulkanCommandAllocator's own
    // destructor rather than this list ever calling
    // vkFreeCommandBuffers itself.
    std::vector<std::pair<VkCommandPool, VkCommandBuffer>>
        commandBuffersByPool_;
};

class VulkanSwapchain final : public Swapchain
{
public:
    VulkanSwapchain(
        VkInstance instance,
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        VkSurfaceKHR surface,
        VkSwapchainKHR nativeSwapchain,
        VulkanQueue& presentQueue,
        const SwapchainDesc& desc,
        VkFormat format,
        VkColorSpaceKHR colorSpace);
    ~VulkanSwapchain() override;

    VulkanSwapchain(const VulkanSwapchain&) = delete;
    VulkanSwapchain& operator=(const VulkanSwapchain&) = delete;

    void Present(bool verticalSync) override;
    void Resize(u32 width, u32 height) override;

    [[nodiscard]] u32 Width() const noexcept override;
    [[nodiscard]] u32 Height() const noexcept override;
    [[nodiscard]] u32 BufferCount() const noexcept override;
    [[nodiscard]] u32 CurrentBackBufferIndex() const noexcept override;
    [[nodiscard]] Texture& CurrentBackBuffer() noexcept override;

private:
    void AcquireIfNeeded() const;
    void CreatePerImageResources();
    void DestroyPerImageResources();

    VkInstance instance_{VK_NULL_HANDLE};
    VkPhysicalDevice physicalDevice_{VK_NULL_HANDLE};
    VkDevice device_{VK_NULL_HANDLE};
    VkSurfaceKHR surface_{VK_NULL_HANDLE};
    VkSwapchainKHR nativeSwapchain_{VK_NULL_HANDLE};
    VkFormat format_{VK_FORMAT_UNDEFINED};
    VkColorSpaceKHR colorSpace_{VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    VulkanQueue* presentQueue_{nullptr};
    u32 width_{};
    u32 height_{};
    u32 bufferCount_{};

    std::vector<std::unique_ptr<VulkanTexture>> backBuffers_;
    std::vector<VkSemaphore> imageAvailableSemaphores_;
    std::vector<VkSemaphore> renderFinishedSemaphores_;
    // One per image-available semaphore slot (not per swapchain
    // image): signaled once the submission that consumed that
    // semaphore's wait has finished, so AcquireIfNeeded can block
    // reusing the semaphore until it's actually safe to. See the
    // comment on Submit() in VulkanQueue.cpp.
    std::vector<VkFence> imageAvailableFences_;

    // Mutable: acquisition is lazily performed the first time either
    // CurrentBackBufferIndex() or CurrentBackBuffer() is queried after
    // a Present(), to match the const, no-side-effect-looking shape
    // the abstract Swapchain interface exposes for those accessors.
    mutable u32 currentImageIndex_{0};
    mutable bool acquired_{false};
    mutable u32 frameSlot_{0};
};

class VulkanDevice final : public Device
{
public:
    VulkanDevice(
        VkInstance instance,
        VkPhysicalDevice physicalDevice,
        VkDevice nativeDevice,
        VmaAllocator allocator,
        u32 graphicsFamilyIndex,
        std::string adapterName,
        DeviceCapabilities capabilities,
        DeviceFunctions functions,
        bool validationEnabled,
        VkDebugUtilsMessengerEXT debugMessenger,
        std::unique_ptr<RenderDocCapture> renderDoc);
    ~VulkanDevice() override;

    VulkanDevice(const VulkanDevice&) = delete;
    VulkanDevice& operator=(const VulkanDevice&) = delete;

    [[nodiscard]] Backend GetBackend() const noexcept override;
    [[nodiscard]] std::string_view AdapterName() const noexcept override;
    [[nodiscard]] const DeviceCapabilities& Capabilities() const noexcept override;

    [[nodiscard]] std::unique_ptr<Queue> CreateQueue(QueueType type) override;
    [[nodiscard]] std::unique_ptr<Fence> CreateFence(u64 initialValue) override;
    [[nodiscard]] std::unique_ptr<CommandAllocator> CreateCommandAllocator(
        QueueType type) override;
    [[nodiscard]] std::unique_ptr<CommandList> CreateCommandList(
        CommandAllocator& allocator) override;

    [[nodiscard]] std::unique_ptr<Buffer> CreateBuffer(
        const BufferDesc& desc) override;

    [[nodiscard]] std::unique_ptr<Texture> CreateTexture(
        const TextureDesc& desc) override;

    [[nodiscard]] std::unique_ptr<GraphicsPipeline> CreateGraphicsPipeline(
        const GraphicsPipelineDesc& desc) override;

    [[nodiscard]] std::unique_ptr<Swapchain> CreateSwapchain(
        Queue& queue,
        const SwapchainDesc& desc) override;

    [[nodiscard]] std::unique_ptr<TimestampQueryPool>
    CreateTimestampQueryPool(u32 count) override;

    [[nodiscard]] f64 TimestampPeriodNanoseconds()
        const noexcept override;

    // Not part of the abstract Device interface -- RenderDoc integration
    // is inherently backend-specific. Null if the device wasn't created
    // with DeviceDesc::enableRenderDoc or RenderDoc wasn't found; see
    // orbit::rhi::vulkan::TriggerRenderDocCapture and friends, which are
    // the actual public entry points (they dynamic_cast down to this).
    [[nodiscard]] RenderDocCapture* GetRenderDocCapture() const noexcept;
    [[nodiscard]] VkInstance NativeInstance() const noexcept;

private:
    VkInstance instance_{VK_NULL_HANDLE};
    VkPhysicalDevice physicalDevice_{VK_NULL_HANDLE};
    VkDevice nativeDevice_{VK_NULL_HANDLE};
    VmaAllocator allocator_{nullptr};
    u32 graphicsFamilyIndex_{0};
    std::string adapterName_;
    DeviceCapabilities capabilities_{};
    DeviceFunctions functions_{};
    bool validationEnabled_{false};
    VkDebugUtilsMessengerEXT debugMessenger_{VK_NULL_HANDLE};
    std::unique_ptr<RenderDocCapture> renderDoc_;
};
} // namespace orbit::rhi::vulkan::detail
