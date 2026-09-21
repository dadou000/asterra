#include <orbit/render_graph/RenderGraph.hpp>

#include <array>
#include <cstddef>
#include <memory>
#include <string_view>
#include <vector>

namespace
{
class FakeBuffer final : public orbit::rhi::Buffer
{
public:
    explicit FakeBuffer(const orbit::rhi::BufferDesc& desc)
        : desc_(desc),
          storage_(static_cast<std::size_t>(desc.sizeBytes))
    {
    }

    orbit::u64 SizeBytes() const noexcept override
    {
        return desc_.sizeBytes;
    }

    orbit::rhi::BufferUsage Usage() const noexcept override
    {
        return desc_.usage;
    }

    orbit::rhi::MemoryUsage Memory() const noexcept override
    {
        return desc_.memory;
    }

    std::byte* Map() override
    {
        return storage_.data();
    }

    void Unmap() override
    {
    }

private:
    orbit::rhi::BufferDesc desc_{};
    std::vector<std::byte> storage_;
};

class FakeDevice final : public orbit::rhi::Device
{
public:
    orbit::rhi::Backend GetBackend() const noexcept override
    {
        return orbit::rhi::Backend::Vulkan;
    }

    std::string_view AdapterName() const noexcept override
    {
        return "RenderGraphTest";
    }

    const orbit::rhi::DeviceCapabilities&
    Capabilities() const noexcept override
    {
        return capabilities_;
    }

    std::unique_ptr<orbit::rhi::Queue> CreateQueue(
        orbit::rhi::QueueType) override
    {
        return {};
    }

    std::unique_ptr<orbit::rhi::Fence> CreateFence(
        orbit::u64) override
    {
        return {};
    }

    std::unique_ptr<orbit::rhi::CommandAllocator>
    CreateCommandAllocator(
        orbit::rhi::QueueType) override
    {
        return {};
    }

    std::unique_ptr<orbit::rhi::CommandList>
    CreateCommandList(
        orbit::rhi::CommandAllocator&) override
    {
        return {};
    }

    std::unique_ptr<orbit::rhi::Buffer> CreateBuffer(
        const orbit::rhi::BufferDesc& desc) override
    {
        return std::make_unique<FakeBuffer>(desc);
    }

    std::unique_ptr<orbit::rhi::Texture> CreateTexture(
        const orbit::rhi::TextureDesc&) override
    {
        return {};
    }

    std::unique_ptr<orbit::rhi::GraphicsPipeline>
    CreateGraphicsPipeline(
        const orbit::rhi::GraphicsPipelineDesc&) override
    {
        return {};
    }

    std::unique_ptr<orbit::rhi::ComputePipeline>
    CreateComputePipeline(
        const orbit::rhi::ComputePipelineDesc&) override
    {
        return {};
    }

    std::unique_ptr<orbit::rhi::Swapchain> CreateSwapchain(
        orbit::rhi::Queue&,
        const orbit::rhi::SwapchainDesc&) override
    {
        return {};
    }

    std::unique_ptr<orbit::rhi::TimestampQueryPool>
    CreateTimestampQueryPool(
        orbit::u32) override
    {
        return {};
    }

    orbit::f64 TimestampPeriodNanoseconds()
        const noexcept override
    {
        return 1.0;
    }

private:
    orbit::rhi::DeviceCapabilities capabilities_{};
};

class FakeCommandList final : public orbit::rhi::CommandList
{
public:
    struct BufferTransition
    {
        orbit::rhi::ResourceState before{};
        orbit::rhi::ResourceState after{};
    };

    orbit::rhi::QueueType Type() const noexcept override
    {
        return orbit::rhi::QueueType::Graphics;
    }

    void Reset(orbit::rhi::CommandAllocator&) override {}

    void Transition(
        orbit::rhi::Texture&,
        orbit::rhi::ResourceState,
        orbit::rhi::ResourceState) override
    {
    }

    void Transition(
        orbit::rhi::Buffer&,
        const orbit::rhi::ResourceState before,
        const orbit::rhi::ResourceState after) override
    {
        bufferTransitions.push_back({before, after});
    }

    void UavBarrier(orbit::rhi::Buffer&) override
    {
        ++bufferUavBarriers;
    }

    void UavBarrier(orbit::rhi::Texture&) override
    {
        ++textureUavBarriers;
    }

    void CopyBuffer(
        orbit::rhi::Buffer&,
        orbit::u64,
        orbit::rhi::Buffer&,
        orbit::u64,
        orbit::u64) override
    {
    }

    void CopyBufferToTexture(
        orbit::rhi::Buffer&,
        orbit::u64,
        orbit::rhi::Texture&) override
    {
    }

    void CopyTextureToBuffer(
        orbit::rhi::Texture&,
        orbit::rhi::Buffer&,
        orbit::u64) override
    {
    }

    void ClearColorTarget(
        orbit::rhi::Texture&,
        const orbit::rhi::ClearColor&) override
    {
    }

    void ClearDepthTarget(
        orbit::rhi::Texture&,
        orbit::f32) override
    {
    }

    void SetRenderTarget(orbit::rhi::Texture&) override {}

    void SetRenderTargets(
        orbit::rhi::Texture&,
        orbit::rhi::Texture&) override
    {
    }

    void SetRenderTargets(
        std::span<orbit::rhi::Texture* const>,
        orbit::rhi::Texture*) override
    {
    }

    void SetViewport(
        const orbit::rhi::Viewport&) override
    {
    }

    void SetScissor(
        const orbit::rhi::ScissorRect&) override
    {
    }

    void SetGraphicsPipeline(
        orbit::rhi::GraphicsPipeline&) override
    {
    }

    void SetGraphicsConstants(
        std::span<const orbit::u32>) override
    {
    }

    void SetGraphicsBuffer(
        orbit::u32,
        orbit::rhi::Buffer&) override
    {
    }

    void SetGraphicsTexture(
        orbit::u32,
        orbit::rhi::Texture&) override
    {
    }

    void SetComputePipeline(
        orbit::rhi::ComputePipeline&) override
    {
    }

    void SetComputeConstants(
        std::span<const orbit::u32>) override
    {
    }

    void SetComputeBuffer(
        orbit::u32,
        orbit::rhi::Buffer&) override
    {
    }

    void SetComputeStorageTexture(
        orbit::u32,
        orbit::rhi::Texture&) override
    {
    }

    void SetComputeTexture(
        orbit::u32,
        orbit::rhi::Texture&) override
    {
    }

    void Dispatch(
        orbit::u32,
        orbit::u32,
        orbit::u32) override
    {
    }

    void SetVertexBuffer(
        orbit::rhi::Buffer&,
        orbit::u32) override
    {
    }

    void SetIndexBuffer(
        orbit::rhi::Buffer&,
        orbit::rhi::IndexFormat) override
    {
    }

    void DrawIndexed(
        orbit::u32,
        orbit::u32,
        orbit::i32) override
    {
    }

    void Draw(
        orbit::u32,
        orbit::u32) override
    {
    }

    void ResetTimestampQueryPool(
        orbit::rhi::TimestampQueryPool&,
        orbit::u32,
        orbit::u32) override
    {
    }

    void WriteTimestamp(
        orbit::rhi::TimestampQueryPool&,
        orbit::u32) override
    {
    }

    void Close() override
    {
    }

    std::vector<BufferTransition> bufferTransitions;
    orbit::u32 bufferUavBarriers{0U};
    orbit::u32 textureUavBarriers{0U};
};
} // namespace

int main()
{
    using namespace orbit;
    using namespace orbit::render_graph;

    FakeDevice device;
    RenderGraph graph(device);

    const auto buffer =
        graph.CreateBuffer(
            "LightingScratch",
            {
                .sizeBytes = 4096U,
                .usage = rhi::BufferUsage::Structured,
                .memory = rhi::MemoryUsage::GpuOnly,
                .initialState = rhi::ResourceState::Common
            });

    std::vector<int> callbackOrder;

    graph.AddPass(
        "Write",
        {},
        {
            {
                .buffer = buffer,
                .state = rhi::ResourceState::UnorderedAccess,
                .access = Access::Write
            }
        },
        [&](rhi::CommandList&, const Resources& resources)
        {
            static_cast<void>(resources.Buffer(buffer));
            callbackOrder.push_back(1);
        });

    graph.AddPass(
        "Read",
        {},
        {
            {
                .buffer = buffer,
                .state = rhi::ResourceState::UnorderedAccess,
                .access = Access::Read
            }
        },
        [&](rhi::CommandList&, const Resources&)
        {
            callbackOrder.push_back(2);
        });

    graph.AddPass(
        "Rewrite",
        {},
        {
            {
                .buffer = buffer,
                .state = rhi::ResourceState::UnorderedAccess,
                .access = Access::Write
            }
        },
        [&](rhi::CommandList&, const Resources&)
        {
            callbackOrder.push_back(3);
        });

    graph.Compile();

    const auto names =
        graph.CompiledPassNames();

    if (names.size() != 3U ||
        names[0] != "Write" ||
        names[1] != "Read" ||
        names[2] != "Rewrite")
    {
        return 1;
    }

    FakeCommandList commands;
    graph.Execute(commands);

    if (callbackOrder !=
        std::vector<int>{1, 2, 3})
    {
        return 2;
    }

    if (commands.bufferTransitions.size() != 1U ||
        commands.bufferTransitions[0].before !=
            rhi::ResourceState::Common ||
        commands.bufferTransitions[0].after !=
            rhi::ResourceState::UnorderedAccess)
    {
        return 3;
    }

    // Write -> read and read -> write in the same UAV state each need an
    // explicit memory/execution dependency.
    if (commands.bufferUavBarriers != 2U)
    {
        return 4;
    }

    if (graph.Buffer(buffer).SizeBytes() != 4096U)
    {
        return 5;
    }

    return 0;
}
