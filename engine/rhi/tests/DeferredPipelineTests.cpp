#include <orbit/rhi/DeferredPipeline.hpp>

#include <algorithm>
#include <array>
#include <future>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace rhi = orbit::rhi;
using orbit::u8;
using orbit::u32;
using orbit::u64;
using orbit::f64;

namespace
{
void Check(const bool good)
{
    if (!good) throw std::runtime_error("Deferred pipeline contract failed.");
}

class TestGraphics final : public rhi::GraphicsPipeline
{
public:
    u32 PushConstantDwords() const noexcept override { return 0U; }
    u32 ShaderResourceBuffers() const noexcept override { return 0U; }
    u32 SampledTextures() const noexcept override { return 0U; }
    rhi::PrimitiveTopology Topology() const noexcept override
    {
        return rhi::PrimitiveTopology::TriangleList;
    }
};

class TestCompute final : public rhi::ComputePipeline
{
public:
    u32 PushConstantDwords() const noexcept override { return 0U; }
    u32 ShaderResourceBuffers() const noexcept override { return 0U; }
    u32 StorageTextures() const noexcept override { return 0U; }
    u32 SampledTextures() const noexcept override { return 0U; }
};

class TestDevice final : public rhi::Device
{
public:
    bool rejectFirstGraphics{true};
    u32 graphicsAttempts{0U};
    u32 computeAttempts{0U};
    std::vector<u8> seenVertex;
    std::vector<u8> seenPixel;
    std::vector<u8> seenCompute;
    std::vector<rhi::VertexAttribute> seenAttributes;
    std::shared_ptr<std::promise<std::unique_ptr<rhi::GraphicsPipeline>>>
        graphicsPromise;
    std::shared_ptr<std::promise<std::unique_ptr<rhi::ComputePipeline>>>
        computePromise;

    rhi::Backend GetBackend() const noexcept override { return rhi::Backend::Vulkan; }
    std::string_view AdapterName() const noexcept override { return "Mock"; }
    const rhi::DeviceCapabilities& Capabilities() const noexcept override
    {
        return capabilities_;
    }
    std::unique_ptr<rhi::Queue> CreateQueue(rhi::QueueType) override { return {}; }
    std::unique_ptr<rhi::Fence> CreateFence(u64) override { return {}; }
    std::unique_ptr<rhi::CommandAllocator> CreateCommandAllocator(
        rhi::QueueType) override { return {}; }
    std::unique_ptr<rhi::CommandList> CreateCommandList(
        rhi::CommandAllocator&) override { return {}; }
    std::unique_ptr<rhi::Buffer> CreateBuffer(
        const rhi::BufferDesc&) override { return {}; }
    std::unique_ptr<rhi::Texture> CreateTexture(
        const rhi::TextureDesc&) override { return {}; }
    std::unique_ptr<rhi::AccelerationStructure> CreateAabbAccelerationStructure(
        std::span<const rhi::AccelerationAabb>) override { return {}; }
    std::unique_ptr<rhi::GraphicsPipeline> CreateGraphicsPipeline(
        const rhi::GraphicsPipelineDesc&) override
    {
        throw std::logic_error("The render thread must not compile synchronously.");
    }
    std::unique_ptr<rhi::ComputePipeline> CreateComputePipeline(
        const rhi::ComputePipelineDesc&) override
    {
        throw std::logic_error("The render thread must not compile synchronously.");
    }
    std::optional<std::future<std::unique_ptr<rhi::GraphicsPipeline>>>
    TryCreateGraphicsPipelineAsync(const rhi::GraphicsPipelineDesc& desc) override
    {
        ++graphicsAttempts;
        if (rejectFirstGraphics)
        {
            rejectFirstGraphics = false;
            return std::nullopt;
        }
        seenVertex.assign(desc.vertexShader.data,
            desc.vertexShader.data + desc.vertexShader.size);
        seenPixel.assign(desc.pixelShader.data,
            desc.pixelShader.data + desc.pixelShader.size);
        seenAttributes.assign(desc.vertexAttributes.begin(),
                              desc.vertexAttributes.end());
        graphicsPromise = std::make_shared<
            std::promise<std::unique_ptr<rhi::GraphicsPipeline>>>();
        return graphicsPromise->get_future();
    }
    std::optional<std::future<std::unique_ptr<rhi::ComputePipeline>>>
    TryCreateComputePipelineAsync(const rhi::ComputePipelineDesc& desc) override
    {
        ++computeAttempts;
        seenCompute.assign(desc.computeShader.data,
            desc.computeShader.data + desc.computeShader.size);
        computePromise = std::make_shared<
            std::promise<std::unique_ptr<rhi::ComputePipeline>>>();
        return computePromise->get_future();
    }
    std::unique_ptr<rhi::Swapchain> CreateSwapchain(
        rhi::Queue&, const rhi::SwapchainDesc&) override { return {}; }
    std::unique_ptr<rhi::TimestampQueryPool> CreateTimestampQueryPool(u32)
        override { return {}; }
    f64 TimestampPeriodNanoseconds() const noexcept override { return 1.0; }
private:
    rhi::DeviceCapabilities capabilities_{};
};
}

int main()
{
    TestDevice device;
    std::unique_ptr<rhi::DeferredGraphicsPipeline> graphics;
    {
        std::array<u8, 4> vertex{1U, 2U, 3U, 4U};
        std::array<u8, 4> pixel{5U, 6U, 7U, 8U};
        std::vector<rhi::VertexAttribute> attributes{
            {2U, rhi::VertexFormat::Float3, 12U}};
        graphics = std::make_unique<rhi::DeferredGraphicsPipeline>(device,
            rhi::GraphicsPipelineDesc{
                .vertexShader = {vertex.data(), vertex.size()},
                .pixelShader = {pixel.data(), pixel.size()},
                .vertexAttributes = attributes});
        Check(device.graphicsAttempts == 1U);
        vertex.fill(0U);
        pixel.fill(0U);
        attributes.clear();
    }
    Check(graphics->Poll() == nullptr);
    Check(device.graphicsAttempts == 2U);
    Check((device.seenVertex == std::vector<u8>{1U, 2U, 3U, 4U}));
    Check((device.seenPixel == std::vector<u8>{5U, 6U, 7U, 8U}));
    Check(device.seenAttributes.size() == 1U);
    Check(device.seenAttributes[0].location == 2U);
    Check(graphics->Poll() == nullptr); // Never blocks on incomplete driver work.
    Check(device.graphicsAttempts == 2U); // No duplicate submissions.
    device.graphicsPromise->set_value(std::make_unique<TestGraphics>());
    Check(graphics->Poll() != nullptr);
    Check(device.graphicsAttempts == 2U);

    std::unique_ptr<rhi::DeferredComputePipeline> compute;
    {
        std::array<u8, 4> shader{11U, 12U, 13U, 14U};
        compute = std::make_unique<rhi::DeferredComputePipeline>(device,
            rhi::ComputePipelineDesc{.computeShader = {shader.data(), shader.size()}});
        shader.fill(0U);
    }
    Check(device.computeAttempts == 1U);
    Check((device.seenCompute == std::vector<u8>{11U, 12U, 13U, 14U}));
    Check(compute->Poll() == nullptr);
    device.computePromise->set_value(std::make_unique<TestCompute>());
    Check(compute->Poll() != nullptr);
    Check(device.computeAttempts == 1U);
    return 0;
}
