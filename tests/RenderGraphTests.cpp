#include <orbit/render_graph/RenderGraph.hpp>
#include <orbit/render_view/RenderView.hpp>

#include <cassert>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
class FakeTexture final : public orbit::rhi::Texture
{
public:
    explicit FakeTexture(
        const orbit::rhi::TextureDesc& desc)
        : width_(desc.width),
          height_(desc.height),
          format_(desc.format)
    {
    }

    [[nodiscard]] orbit::u32 Width() const noexcept override
    {
        return width_;
    }

    [[nodiscard]] orbit::u32 Height() const noexcept override
    {
        return height_;
    }

    [[nodiscard]] orbit::rhi::TextureFormat
    Format() const noexcept override
    {
        return format_;
    }

private:
    orbit::u32 width_{0};
    orbit::u32 height_{0};
    orbit::rhi::TextureFormat format_{};
};

class FakeDevice final : public orbit::rhi::Device
{
public:
    [[nodiscard]] orbit::rhi::Backend
    GetBackend() const noexcept override
    {
        return orbit::rhi::Backend::Vulkan;
    }

    [[nodiscard]] std::string_view
    AdapterName() const noexcept override
    {
        return "Fake";
    }

    [[nodiscard]] const orbit::rhi::DeviceCapabilities&
    Capabilities() const noexcept override
    {
        return capabilities_;
    }

    [[nodiscard]] std::unique_ptr<orbit::rhi::Queue>
    CreateQueue(orbit::rhi::QueueType) override
    {
        return nullptr;
    }

    [[nodiscard]] std::unique_ptr<orbit::rhi::Fence>
    CreateFence(orbit::u64) override
    {
        return nullptr;
    }

    [[nodiscard]] std::unique_ptr<orbit::rhi::CommandAllocator>
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

    void Close() override {}

    std::vector<TransitionRecord> transitions;
};
} // namespace

int main()
{
    FakeDevice device;

    orbit::render_view::RenderView viewA(
        device,
        {.width = 640, .height = 360});

    orbit::render_view::RenderView viewB(
        device,
        {.width = 320, .height = 240});

    assert(viewA.Width() == 640);
    assert(viewB.Width() == 320);
    assert(
        &viewA.Color() !=
        &viewB.Color());

    // Three persistent targets per view.
    assert(device.TextureCreates() == 6);

    viewB.Resize(800, 600);

    assert(viewB.Width() == 800);
    assert(viewB.Height() == 600);
    assert(device.TextureCreates() == 9);

    orbit::render_graph::RenderGraph graph(
        device);

    const auto targetsA =
        viewA.Import(graph, "ViewA");
    const auto targetsB =
        viewB.Import(graph, "ViewB");

    int viewARenders = 0;
    int viewBRenders = 0;
    int samples = 0;

    graph.AddPass(
        "ViewA.Scene",
        {
            {
                .texture = targetsA.color,
                .state =
                    orbit::rhi::ResourceState::
                        RenderTarget,
                .access =
                    orbit::render_graph::Access::
                        Write
            }
        },
        [&viewARenders](
            orbit::rhi::CommandList&,
            const orbit::render_graph::Resources&)
        {
            ++viewARenders;
        });

    graph.AddPass(
        "ViewB.Scene",
        {
            {
                .texture = targetsB.color,
                .state =
                    orbit::rhi::ResourceState::
                        RenderTarget,
                .access =
                    orbit::render_graph::Access::
                        Write
            }
        },
        [&viewBRenders](
            orbit::rhi::CommandList&,
            const orbit::render_graph::Resources&)
        {
            ++viewBRenders;
        });

    graph.AddPass(
        "ViewA.Sample",
        {
            {
                .texture = targetsA.color,
                .state =
                    orbit::rhi::ResourceState::
                        ShaderResource,
                .access =
                    orbit::render_graph::Access::
                        Read
            }
        },
        [&samples](
            orbit::rhi::CommandList&,
            const orbit::render_graph::Resources&)
        {
            ++samples;
        });

    graph.AddPass(
        "ViewB.Sample",
        {
            {
                .texture = targetsB.color,
                .state =
                    orbit::rhi::ResourceState::
                        ShaderResource,
                .access =
                    orbit::render_graph::Access::
                        Read
            }
        },
        [&samples](
            orbit::rhi::CommandList&,
            const orbit::render_graph::Resources&)
        {
            ++samples;
        });

    graph.Compile();

    const auto order =
        graph.CompiledPassNames();

    assert(order.size() == 4);

    FakeCommandList commands;
    graph.Execute(commands);

    assert(viewARenders == 1);
    assert(viewBRenders == 1);
    assert(samples == 2);

    // Each color starts ShaderResource, becomes RenderTarget for its
    // scene, then returns to ShaderResource for UI/compositing.
    assert(commands.transitions.size() == 4);

    return 0;
}
