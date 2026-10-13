#include <orbit/lighting/TerrainSunShadow.hpp>
#include <orbit/lighting/SunVisibilityShader.hpp>
#include <orbit/rhi/vulkan/VulkanBackend.hpp>
#include <orbit/shader/dxc/DxcShaderCompiler.hpp>

#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <string>

namespace
{
class Ridge final : public orbit::terrain::TerrainSource
{
public:
    orbit::terrain::TerrainSample Sample(const orbit::terrain::TerrainQuery& q) const noexcept override
    {
        const double t = (q.unitDirection.y - 0.03) / 0.003;
        return {.elevationMeters = q.unitDirection.x > 0.9 ? 4000.0 * std::exp(-t * t) : 0.0};
    }
    orbit::u64 Revision() const noexcept override { return 1U; }
};
}

int main()
{
    using namespace orbit;
    constexpr u32 count = 64U;
    try
    {
        auto device = rhi::vulkan::CreateDevice();
        const shader::dxc::DxcShaderCompiler compiler;
        Ridge source;
        const auto field = lighting::BuildTerrainSunShadowField(source, 1.0e6, 2048U, 1024U);
        auto terrain = lighting::UploadTerrainSunShadowField(*device, field);
        std::array<math::Float4, count> points{};
        for (u32 i = 0U; i < count; ++i)
            points[i] = {1000500.0F + static_cast<f32>(i) * 60.0F, -200000.0F, 0.0F, 0.001F};
        auto inputs = device->CreateBuffer({.sizeBytes = sizeof(points),
            .usage = rhi::BufferUsage::Structured, .memory = rhi::MemoryUsage::HostVisible,
            .initialState = rhi::ResourceState::ShaderResource});
        std::memcpy(inputs->Map(), points.data(), sizeof(points));
        inputs->Unmap();
        auto outputs = device->CreateBuffer({.sizeBytes = count * sizeof(f32),
            .usage = rhi::BufferUsage::Structured, .memory = rhi::MemoryUsage::GpuOnly,
            .initialState = rhi::ResourceState::UnorderedAccess});
        auto readback = device->CreateBuffer({.sizeBytes = count * sizeof(f32),
            .usage = rhi::BufferUsage::Generic, .memory = rhi::MemoryUsage::HostReadback,
            .initialState = rhi::ResourceState::CopyDestination});
        const auto shaderCode = compiler.Compile({
            .source = std::string(lighting::kSunVisibilityShader) + R"(
[[vk::binding(0, 0)]] ByteAddressBuffer terrain;
[[vk::binding(1, 0)]] StructuredBuffer<float4> points;
[[vk::binding(2, 0)]] RWStructuredBuffer<float> result;
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const float4 p = points[id.x];
    result[id.x] = TerrainSunVisibility(terrain, p.xyz, float3(0,1,0), 1.0e6, p.w);
}
)", .entryPoint = "main", .stage = shader::Stage::Compute});
        auto pipeline = device->CreateComputePipeline({
            .computeShader = {.data = shaderCode.bytecode.data(), .size = shaderCode.bytecode.size()},
            .shaderResourceBuffers = 3U});
        auto allocator = device->CreateCommandAllocator(rhi::QueueType::Graphics);
        auto commands = device->CreateCommandList(*allocator);
        auto fence = device->CreateFence(0U);
        commands->Reset(*allocator);
        commands->SetComputePipeline(*pipeline);
        commands->SetComputeBuffer(0U, *terrain);
        commands->SetComputeBuffer(1U, *inputs);
        commands->SetComputeBuffer(2U, *outputs);
        commands->Dispatch(1U, 1U, 1U);
        commands->Transition(*outputs, rhi::ResourceState::UnorderedAccess, rhi::ResourceState::CopySource);
        commands->CopyBuffer(*outputs, 0U, *readback, 0U, count * sizeof(f32));
        commands->Close();
        auto queue = device->CreateQueue(rhi::QueueType::Graphics);
        queue->Submit(*commands);
        queue->Signal(*fence, 1U);
        fence->Wait(1U);
        std::array<f32, count> results{};
        std::memcpy(results.data(), readback->Map(), sizeof(results));
        readback->Unmap();
        u32 blocked = 0U, lit = 0U, partial = 0U;
        for (u32 i = 0U; i < count; ++i)
        {
            const auto p = points[i];
            const double reference = lighting::TerrainSunVisibility(field,
                {p.x, p.y, p.z}, {0, 1, 0}, p.w);
            if (!std::isfinite(results[i]) || std::abs(results[i] - reference) > 0.03)
            {
                std::cerr << "Terrain visibility mismatch at " << i << ": " << results[i]
                          << " versus " << reference << '\n';
                return 1;
            }
            blocked += reference == 0.0;
            lit += reference == 1.0;
            partial += reference > 0.0 && reference < 1.0;
        }
        if (blocked == 0U || lit == 0U || partial == 0U) return 2;
        std::cout << "GPU terrain visibility matches CPU across umbra, penumbra and sunlight.\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 3;
    }
}
