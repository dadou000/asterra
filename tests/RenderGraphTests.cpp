    CreateCommandAllocator(orbit::rhi::QueueType) override
    {
        return nullptr;
    }

    [[nodiscard]] std::unique_ptr<orbit::rhi::CommandList>
    CreateCommandList(orbit::rhi::CommandAllocator&) override
    {
        return nullptr;
    }

    [[nodiscard]] std::unique_ptr<orbit::rhi::Buffer>
    CreateBuffer(const orbit::rhi::BufferDesc&) override
    {
        return nullptr;
    }

    [[nodiscard]] std::unique_ptr<orbit::rhi::Texture>
    CreateTexture(
        const orbit::rhi::TextureDesc& desc) override
    {
        ++textureCreates_;
        return std::make_unique<FakeTexture>(desc);
    }

    [[nodiscard]] std::unique_ptr<orbit::rhi::GraphicsPipeline>
    CreateGraphicsPipeline(
        const orbit::rhi::GraphicsPipelineDesc&) override
    {
        return nullptr;
    }

    [[nodiscard]] std::unique_ptr<orbit::rhi::ComputePipeline>
    CreateComputePipeline(
        const orbit::rhi::ComputePipelineDesc&) override
    {
        return nullptr;
    }

    [[nodiscard]] std::unique_ptr<orbit::rhi::Swapchain>
    CreateSwapchain(
        orbit::rhi::Queue&,
        const orbit::rhi::SwapchainDesc&) override
    {
        return nullptr;
    }

    [[nodiscard]] std::unique_ptr<orbit::rhi::TimestampQueryPool>
    CreateTimestampQueryPool(orbit::u32) override
    {
        return nullptr;
    }

    [[nodiscard]] orbit::f64
    TimestampPeriodNanoseconds() const noexcept override
    {
        return 1.0;
    }

    [[nodiscard]] orbit::u32 TextureCreates() const noexcept
    {
        return textureCreates_;
    }

private:
    orbit::rhi::DeviceCapabilities capabilities_{};
    orbit::u32 textureCreates_{0};
};

class FakeCommandList final : public orbit::rhi::CommandList
{
public:
    struct TransitionRecord
    {
        orbit::rhi::Texture* texture{};
        orbit::rhi::ResourceState before{};
        orbit::rhi::ResourceState after{};
    };

    [[nodiscard]] orbit::rhi::QueueType Type() const noexcept override
    {
        return orbit::rhi::QueueType::Graphics;
    }

    void Reset(orbit::rhi::CommandAllocator&) override {}

    void Transition(
        orbit::rhi::Texture& texture,
        orbit::rhi::ResourceState before,
        orbit::rhi::ResourceState after) override
    {
        transitions.push_back({
            .texture = &texture,
            .before = before,
            .after = after
        });
    }

    void Transition(
        orbit::rhi::Buffer&,
        orbit::rhi::ResourceState,
        orbit::rhi::ResourceState) override {}

    void UavBarrier(orbit::rhi::Buffer&) override {}
    void UavBarrier(orbit::rhi::Texture&) override {}

    void CopyBuffer(
        orbit::rhi::Buffer&,
        orbit::u64,
        orbit::rhi::Buffer&,
        orbit::u64,
        orbit::u64) override {}

    void CopyBufferToTexture(
        orbit::rhi::Buffer&,
        orbit::u64,
        orbit::rhi::Texture&) override {}

    void CopyTextureToBuffer(
        orbit::rhi::Texture&,
        orbit::rhi::Buffer&,
        orbit::u64) override {}

    void ClearColorTarget(
        orbit::rhi::Texture&,
        const orbit::rhi::ClearColor&) override {}

    void ClearDepthTarget(
        orbit::rhi::Texture&,
        orbit::f32) override {}

    void SetRenderTarget(
        orbit::rhi::Texture&) override {}

    void SetRenderTargets(
        orbit::rhi::Texture&,
        orbit::rhi::Texture&) override {}

    void SetViewport(
        const orbit::rhi::Viewport&) override {}

    void SetScissor(
        const orbit::rhi::ScissorRect&) override {}

    void SetGraphicsPipeline(
        orbit::rhi::GraphicsPipeline&) override {}

    void SetGraphicsConstants(
        std::span<const orbit::u32>) override {}

    void SetGraphicsBuffer(
        orbit::u32,
        orbit::rhi::Buffer&) override {}

    void SetGraphicsTexture(
        orbit::u32,
        orbit::rhi::Texture&) override {}

    void SetComputePipeline(
        orbit::rhi::ComputePipeline&) override {}

    void SetComputeConstants(
        std::span<const orbit::u32>) override {}

    void SetComputeBuffer(
        orbit::u32,
        orbit::rhi::Buffer&) override {}

    void SetComputeStorageTexture(
        orbit::u32,
        orbit::rhi::Texture&) override {}

    void SetComputeTexture(
        orbit::u32,
        orbit::rhi::Texture&) override {}

    void Dispatch(
        orbit::u32,
        orbit::u32,
        orbit::u32) override {}

    void SetVertexBuffer(
        orbit::rhi::Buffer&,
        orbit::u32) override {}

    void SetIndexBuffer(
        orbit::rhi::Buffer&,
        orbit::rhi::IndexFormat) override {}

    void DrawIndexed(
        orbit::u32,
        orbit::u32,
        orbit::i32) override {}

    void Draw(
        orbit::u32,
        orbit::u32) override {}

    void ResetTimestampQueryPool(
        orbit::rhi::TimestampQueryPool&,
        orbit::u32,
        orbit::u32) override {}

    void WriteTimestamp(
        orbit::rhi::TimestampQueryPool&,
        orbit::u32) override {}