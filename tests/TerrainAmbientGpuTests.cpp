// Terrain sky-visibility channel on a real Vulkan device: the compute pass
// over a synthetic toroidal clipmap level, its merge with a coarser level, and
// the lighting lookup that reads the result back through the same addressing.
#include <orbit/rhi/vulkan/VulkanBackend.hpp>
#include <orbit/shader/dxc/DxcShaderCompiler.hpp>
#include <orbit/terrain/TerrainAmbientLayout.hpp>
#include <orbit/terrain_gpu/GpuTerrainAmbient.hpp>
#include <orbit/world/Planet.hpp>

#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace
{
using namespace orbit;

constexpr u32 kResolution = 129U;
constexpr u32 kSamples = kResolution * kResolution;

struct Texel
{
    f32 visibility{0.0F};
    f32 leanEast{0.0F};
    f32 leanNorth{0.0F};
    bool valid{false};
};

Texel Decode(const u32 packed)
{
    return {
        .visibility = static_cast<f32>(packed & 0xFFU) / 255.0F,
        .leanEast = (static_cast<f32>((packed >> 8U) & 0xFFU) - 128.0F) / 127.0F / 3.0F,
        .leanNorth = (static_cast<f32>((packed >> 16U) & 0xFFU) - 128.0F) / 127.0F / 3.0F,
        .valid = ((packed >> 24U) & 0xFFU) != 0U};
}

u32 PhysicalIndex(const u32 x, const u32 y, const u32 originX, const u32 originY)
{
    return ((y + originY) % kResolution) * kResolution + ((x + originX) % kResolution);
}

std::unique_ptr<rhi::Buffer> MakeSamples(
    rhi::Device& device, const u32 originX, const u32 originY,
    const std::function<f32(u32, u32)>& height)
{
    std::vector<std::array<f32, 8>> records(kSamples);
    for (u32 y = 0U; y < kResolution; ++y)
        for (u32 x = 0U; x < kResolution; ++x)
            records[PhysicalIndex(x, y, originX, originY)][0] = height(x, y);
    auto buffer = device.CreateBuffer({.sizeBytes = records.size() * sizeof(records[0]),
        .usage = rhi::BufferUsage::Structured, .memory = rhi::MemoryUsage::HostVisible,
        .initialState = rhi::ResourceState::ShaderResource});
    std::memcpy(buffer->Map(), records.data(), records.size() * sizeof(records[0]));
    buffer->Unmap();
    return buffer;
}

void WriteLevelInfo(
    std::byte* const record, const terrain_gpu::GpuTerrainAmbientLevel& level, const f64 radius)
{
    const std::array<f32, 12> floats{
        static_cast<f32>(level.surfaceFrame.up.x), static_cast<f32>(level.surfaceFrame.up.y),
        static_cast<f32>(level.surfaceFrame.up.z), static_cast<f32>(radius),
        static_cast<f32>(level.surfaceFrame.east.x), static_cast<f32>(level.surfaceFrame.east.y),
        static_cast<f32>(level.surfaceFrame.east.z), static_cast<f32>(level.spacingMeters),
        static_cast<f32>(level.surfaceFrame.north.x), static_cast<f32>(level.surfaceFrame.north.y),
        static_cast<f32>(level.surfaceFrame.north.z), (static_cast<f32>(level.resolution) - 1.0F) * 0.5F};
    const std::array<u32, 8> tail{
        std::bit_cast<u32>(static_cast<f32>(level.centerOffsetMeters.x)),
        std::bit_cast<u32>(static_cast<f32>(level.centerOffsetMeters.y)),
        level.originX, level.originY, level.resolution, level.firstSample, 1U, 0U};
    std::memcpy(record, floats.data(), sizeof(floats));
    std::memcpy(record + sizeof(floats), tail.data(), sizeof(tail));
}
} // namespace

int main()
{
    try
    {
        const world::PlanetDefinition planet{.radiusMeters = 6'000'000.0};
        const auto frame = world::MakeSurfaceFrame(math::Normalize(math::Double3{0.3, 0.8, 0.5}));
        auto device = rhi::vulkan::CreateDevice();
        const shader::dxc::DxcShaderCompiler compiler;
        const terrain_gpu::GpuTerrainAmbient ambientPass(*device, compiler);

        // Fine level: 10 m samples, a 300 m plateau for logical y >= 80, with a
        // non-zero toroidal origin so physical and logical indices differ.
        terrain_gpu::GpuTerrainAmbientLevel fine{
            .resolution = kResolution, .spacingMeters = 10.0, .centerOffsetMeters = {0.0, 0.0},
            .originX = 17U, .originY = 29U, .surfaceFrame = frame, .firstSample = 0U};
        // Coarse level: 80 m samples, same centre, a 2 km wall ~3 km north.
        terrain_gpu::GpuTerrainAmbientLevel coarse{
            .resolution = kResolution, .spacingMeters = 80.0, .centerOffsetMeters = {0.0, 0.0},
            .originX = 5U, .originY = 61U, .surfaceFrame = frame, .firstSample = kSamples};

        auto fineSamples = MakeSamples(*device, fine.originX, fine.originY,
            [](const u32, const u32 y) { return y >= 80U ? 300.0F : 0.0F; });
        auto flatSamples = MakeSamples(*device, fine.originX, fine.originY,
            [](const u32, const u32) { return 0.0F; });
        auto coarseSamples = MakeSamples(*device, coarse.originX, coarse.originY,
            [](const u32, const u32 y) { return y >= 101U ? 2000.0F : 0.0F; });

        constexpr u32 kTotal = kSamples * 3U;
        auto ambient = device->CreateBuffer({.sizeBytes = kTotal * 4U,
            .usage = rhi::BufferUsage::Structured, .memory = rhi::MemoryUsage::GpuOnly,
            .initialState = rhi::ResourceState::ShaderResource});
        auto readback = device->CreateBuffer({.sizeBytes = kTotal * 4U,
            .usage = rhi::BufferUsage::Generic, .memory = rhi::MemoryUsage::HostReadback,
            .initialState = rhi::ResourceState::CopyDestination});

        // Level 0 (plateau wall) at slot 0, level 1 (flat fine + coarse merge) at
        // slot 2 with the coarse level at slot 1.
        terrain_gpu::GpuTerrainAmbientLevel flatFine = fine;
        flatFine.firstSample = 2U * kSamples;
        auto allocator = device->CreateCommandAllocator(rhi::QueueType::Graphics);
        auto commands = device->CreateCommandList(*allocator);
        auto fence = device->CreateFence(0U);
        auto queue = device->CreateQueue(rhi::QueueType::Graphics);
        commands->Reset(*allocator);
        commands->Transition(*ambient, rhi::ResourceState::ShaderResource, rhi::ResourceState::UnorderedAccess);
        ambientPass.Clear(*commands, kTotal, *ambient);
        commands->UavBarrier(*ambient);
        ambientPass.Dispatch(*commands, {.level = fine, .planetRadiusMeters = planet.radiusMeters},
            *fineSamples, *ambient);
        commands->UavBarrier(*ambient);
        ambientPass.Dispatch(*commands, {.level = coarse, .planetRadiusMeters = planet.radiusMeters},
            *coarseSamples, *ambient);
        commands->UavBarrier(*ambient);
        ambientPass.Dispatch(*commands,
            {.level = flatFine, .coarser = coarse, .planetRadiusMeters = planet.radiusMeters},
            *flatSamples, *ambient);
        commands->UavBarrier(*ambient);
        commands->Transition(*ambient, rhi::ResourceState::UnorderedAccess, rhi::ResourceState::CopySource);
        commands->CopyBuffer(*ambient, 0U, *readback, 0U, kTotal * 4U);
        commands->Close();
        queue->Submit(*commands);
        queue->Signal(*fence, 1U);
        fence->Wait(1U);
        std::vector<u32> texels(kTotal);
        std::memcpy(texels.data(), readback->Map(), texels.size() * 4U);
        readback->Unmap();

        const auto fineAt = [&](const u32 x, const u32 y)
        { return Decode(texels[PhysicalIndex(x, y, fine.originX, fine.originY)]); };
        const auto mergedAt = [&](const u32 x, const u32 y)
        { return Decode(texels[2U * kSamples + PhysicalIndex(x, y, fine.originX, fine.originY)]); };

        const Texel plateau = fineAt(64U, 100U);
        const Texel wallBase = fineAt(64U, 79U);
        const Texel farOpen = fineAt(64U, 20U);
        std::cout << "plateau " << plateau.visibility << " wallBase " << wallBase.visibility
                  << " (lean north " << wallBase.leanNorth << ") farOpen " << farOpen.visibility << '\n';
        if (!plateau.valid || !wallBase.valid || !farOpen.valid)
        {
            std::cerr << "Ambient texels were not marked valid.\n";
            return 1;
        }
        if (plateau.visibility < 0.95F)
        {
            std::cerr << "A sample on the plateau must see almost the whole sky.\n";
            return 2;
        }
        if (wallBase.visibility > 0.7F || wallBase.leanNorth > -0.03F)
        {
            std::cerr << "The foot of a 300 m wall must be occluded and lean away from it (south).\n";
            return 3;
        }
        if (farOpen.visibility < wallBase.visibility + 0.2F)
        {
            std::cerr << "A sample 600 m from the wall must be more open than one at its foot.\n";
            return 4;
        }

        const Texel mergedCentre = mergedAt(64U, 64U);
        const Texel flatCentre = fineAt(64U, 20U);
        std::cout << "flat fine with coarse wall: " << mergedCentre.visibility
                  << " (flat fine alone is open at 1.0, wall-plateau farOpen " << flatCentre.visibility << ")\n";
        // A 2 km wall 3 km north blocks about the northern half of the sky
        // (analytically ~0.94); a flat level alone would read exactly 1.0.
        if (!mergedCentre.valid || mergedCentre.visibility > 0.97F || mergedCentre.leanNorth > -0.005F)
        {
            std::cerr << "A flat fine level must take the far wall's occlusion from the coarser level.\n";
            return 5;
        }

        // Lighting lookup: the same addressing read back through the shader.
        auto levels = device->CreateBuffer({.sizeBytes = 2U * terrain::kTerrainAmbientLevelInfoBytes,
            .usage = rhi::BufferUsage::Structured, .memory = rhi::MemoryUsage::HostVisible,
            .initialState = rhi::ResourceState::ShaderResource});
        {
            auto* bytes = static_cast<std::byte*>(levels->Map());
            WriteLevelInfo(bytes, fine, planet.radiusMeters);
            WriteLevelInfo(bytes + terrain::kTerrainAmbientLevelInfoBytes, coarse, planet.radiusMeters);
            levels->Unmap();
        }
        const std::array<math::Double2, 3> probes{
            math::Double2{0.0, (79.0 - 64.0) * 10.0},   // logical (64, 79)
            math::Double2{0.0, (100.0 - 64.0) * 10.0},  // logical (64, 100)
            math::Double2{200000.0, 0.0}};              // far outside every level
        std::array<math::Float4, 3> positions{};
        for (std::size_t i = 0; i < probes.size(); ++i)
        {
            const auto d = world::DirectionAtSurfaceOffset(planet, frame, probes[i]);
            const f64 radius = planet.radiusMeters + (i == 0 ? 0.0 : 0.0);
            positions[i] = {static_cast<f32>(d.x * radius), static_cast<f32>(d.y * radius),
                static_cast<f32>(d.z * radius), 0.0F};
        }
        auto positionBuffer = device->CreateBuffer({.sizeBytes = sizeof(positions),
            .usage = rhi::BufferUsage::Structured, .memory = rhi::MemoryUsage::HostVisible,
            .initialState = rhi::ResourceState::ShaderResource});
        std::memcpy(positionBuffer->Map(), positions.data(), sizeof(positions));
        positionBuffer->Unmap();
        auto resultBuffer = device->CreateBuffer({.sizeBytes = sizeof(positions),
            .usage = rhi::BufferUsage::Structured, .memory = rhi::MemoryUsage::GpuOnly,
            .initialState = rhi::ResourceState::UnorderedAccess});
        auto resultReadback = device->CreateBuffer({.sizeBytes = sizeof(positions),
            .usage = rhi::BufferUsage::Generic, .memory = rhi::MemoryUsage::HostReadback,
            .initialState = rhi::ResourceState::CopyDestination});

        const std::string lookupSource = std::string("#define TA_BUFFER ByteAddressBuffer\n") +
            terrain::kTerrainAmbientCommonShader + terrain::kTerrainAmbientLookupShader + R"(
[[vk::binding(0, 0)]] ByteAddressBuffer g_levels;
[[vk::binding(1, 0)]] ByteAddressBuffer g_ambient;
[[vk::binding(2, 0)]] StructuredBuffer<float4> g_positions;
[[vk::binding(3, 0)]] RWStructuredBuffer<float4> g_result;
[numthreads(3, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    g_result[id.x] = TerrainAmbientLookup(g_levels, g_ambient, 2u, g_positions[id.x].xyz);
}
)";
        const auto lookupCode = compiler.Compile({.source = lookupSource, .entryPoint = "main",
            .stage = shader::Stage::Compute});
        auto lookupPipeline = device->CreateComputePipeline({
            .computeShader = {.data = lookupCode.bytecode.data(), .size = lookupCode.bytecode.size()},
            .shaderResourceBuffers = 4U});
        // The ambient buffer finished its copy in CopySource; return it to a
        // readable state for the lookup.
        commands->Reset(*allocator);
        commands->Transition(*ambient, rhi::ResourceState::CopySource, rhi::ResourceState::ShaderResource);
        commands->SetComputePipeline(*lookupPipeline);
        commands->SetComputeBuffer(0U, *levels);
        commands->SetComputeBuffer(1U, *ambient);
        commands->SetComputeBuffer(2U, *positionBuffer);
        commands->SetComputeBuffer(3U, *resultBuffer);
        commands->Dispatch(1U, 1U, 1U);
        commands->Transition(*resultBuffer, rhi::ResourceState::UnorderedAccess, rhi::ResourceState::CopySource);
        commands->CopyBuffer(*resultBuffer, 0U, *resultReadback, 0U, sizeof(positions));
        commands->Close();
        queue->Submit(*commands);
        queue->Signal(*fence, 2U);
        fence->Wait(2U);
        std::array<math::Float4, 3> results{};
        std::memcpy(results.data(), resultReadback->Map(), sizeof(results));
        resultReadback->Unmap();

        std::cout << "lookup wallBase " << results[0].x << " plateau " << results[1].x
                  << " outside " << results[2].x << '\n';
        if (std::abs(results[0].x - wallBase.visibility) > 0.03F)
        {
            std::cerr << "Lighting lookup disagrees with the compute output at the wall base.\n";
            return 6;
        }
        if (std::abs(results[1].x - plateau.visibility) > 0.03F)
        {
            std::cerr << "Lighting lookup disagrees with the compute output on the plateau.\n";
            return 7;
        }
        // Outside the fine level the coarse level answers (its flat ground reads
        // open or the wall); either way it must be a valid hit, not "none".
        const f32 bentLength = std::sqrt(results[0].y * results[0].y +
            results[0].z * results[0].z + results[0].w * results[0].w);
        if (std::abs(bentLength - 1.0F) > 1.0e-3F)
        {
            std::cerr << "The bent normal must be a unit vector.\n";
            return 8;
        }
        if (results[2].x >= 0.0F && results[2].x > 1.0F)
        {
            std::cerr << "A probe outside every level must not return a visibility above one.\n";
            return 9;
        }
        std::cout << "Terrain ambient compute and lighting lookup agree.\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 20;
    }
}
