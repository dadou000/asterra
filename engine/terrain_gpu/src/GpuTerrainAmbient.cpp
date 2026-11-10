#include <orbit/terrain_gpu/GpuTerrainAmbient.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <stdexcept>
#include <string>

namespace orbit::terrain_gpu
{
namespace
{
constexpr u32 kThreadGroupSize = 8U;
// Ten float4: level (up+R, east+spacing, north+halfCells, centre offset+origin),
// level dims (resolution, firstSample, mode, count), then the same for the
// coarser level.
constexpr u32 kPushConstantDwords = 40U;

// One thread per logical sample of the level. Sixteen azimuths are marched
// across the level's own grid; whatever lies beyond its reach is taken from
// the coarser level's finished result at the same place (the more occluded
// of the two wins, which is the horizon maximum). Elevation is bed height:
// standing water is not opaque relief.
constexpr const char* kComputeSource = R"(
struct Constants
{
    float4 upPlanetRadius;
    float4 eastSpacing;
    float4 northHalfCells;
    float4 centerOffsetOrigin; // xy centre offset, zw origin as uint bits
    uint4 dims;                // resolution, firstSample, mode (1 = clear), clear count
    float4 cUpPlanetRadius;
    float4 cEastSpacing;
    float4 cNorthHalfCells;
    float4 cCenterOffsetOrigin;
    uint4 cDims;               // resolution (0 = no coarser), firstSample
};
[[vk::push_constant]] Constants g;

[[vk::binding(0, 0)]] ByteAddressBuffer g_samples;
[[vk::binding(1, 0)]] RWByteAddressBuffer g_ambient;

#define TA_BUFFER RWByteAddressBuffer
)" /* common shader is spliced here */ R"(
float HeightAt(uint x, uint y, uint resolution, uint originX, uint originY)
{
    return asfloat(g_samples.Load(TaPhysicalIndex(x, y, resolution, originX, originY) * 32u));
}

float BilinearHeight(float2 p, uint resolution, uint originX, uint originY)
{
    const float top = float(resolution - 1u);
    const float2 q = clamp(p, 0.0, top - 0.001);
    const float2 base = floor(q);
    const float2 f = q - base;
    const uint x0 = uint(base.x), y0 = uint(base.y);
    const float h00 = HeightAt(x0, y0, resolution, originX, originY);
    const float h10 = HeightAt(x0 + 1u, y0, resolution, originX, originY);
    const float h01 = HeightAt(x0, y0 + 1u, resolution, originX, originY);
    const float h11 = HeightAt(x0 + 1u, y0 + 1u, resolution, originX, originY);
    return lerp(lerp(h00, h10, f.x), lerp(h01, h11, f.x), f.y);
}

uint EncodeTexel(float visibility, float2 lean)
{
    const uint v = uint(round(saturate(visibility) * 255.0));
    const uint e = uint(clamp(round(lean.x * 3.0 * 127.0) + 128.0, 1.0, 255.0));
    const uint n = uint(clamp(round(lean.y * 3.0 * 127.0) + 128.0, 1.0, 255.0));
    return v | (e << 8u) | (n << 16u) | (1u << 24u);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (g.dims.z == 1u)
    {
        // Clear mode: a square dispatch covering dims.w texels.
        const uint width = (uint(ceil(sqrt(float(g.dims.w)) / 8.0))) * 8u;
        const uint index = id.y * width + id.x;
        if (index < g.dims.w)
            g_ambient.Store(index * 4u, 0u);
        return;
    }

    const uint resolution = g.dims.x;
    if (id.x >= resolution || id.y >= resolution)
        return;
    const uint originX = asuint(g.centerOffsetOrigin.z);
    const uint originY = asuint(g.centerOffsetOrigin.w);
    const float planetRadius = g.upPlanetRadius.w;
    const float spacing = g.eastSpacing.w;

    const float2 here = float2(id.xy);
    const float h0 = HeightAt(id.x, id.y, resolution, originX, originY);

    static const float kSteps[13] = {1.0, 2.0, 3.0, 4.0, 6.0, 8.0, 12.0, 16.0, 24.0, 32.0, 48.0, 64.0, 96.0};
    float visibilitySum = 0.0;
    float2 lean = float2(0.0, 0.0);
    [loop]
    for (uint azimuth = 0u; azimuth < 16u; ++azimuth)
    {
        float s;
        float c;
        sincos(float(azimuth) * (6.28318530718 / 16.0), s, c);
        const float2 direction = float2(c, s);
        float maximumSlope = 0.0;
        [loop]
        for (uint stepIndex = 0u; stepIndex < 13u; ++stepIndex)
        {
            const float2 p = here + direction * kSteps[stepIndex];
            if (p.x < 0.0 || p.y < 0.0 || p.x > float(resolution - 1u) || p.y > float(resolution - 1u))
                break;
            const float meters = kSteps[stepIndex] * spacing;
            const float h = BilinearHeight(p, resolution, originX, originY);
            // Curvature drops distant relief below the local horizontal.
            const float slope = (h - h0 - meters * meters / (2.0 * planetRadius)) / meters;
            maximumSlope = max(maximumSlope, slope);
        }
        // Cosine-weighted visible fraction of this azimuth's sky slice.
        const float azimuthVisibility = 1.0 / (1.0 + maximumSlope * maximumSlope);
        visibilitySum += azimuthVisibility;
        lean -= direction * (1.0 - azimuthVisibility);
    }
    float visibility = visibilitySum / 16.0;
    lean /= 16.0;

    if (g.cDims.x != 0u)
    {
        // The coarser level's horizon covers what this level's grid cannot.
        const float3 up = g.upPlanetRadius.xyz, east = g.eastSpacing.xyz, north = g.northHalfCells.xyz;
        const float2 offset = g.centerOffsetOrigin.xy + (here - g.northHalfCells.w) * spacing;
        const float3 direction = TaSurfaceDirection(offset, planetRadius, up, east, north);
        const float3 cUp = g.cUpPlanetRadius.xyz, cEast = g.cEastSpacing.xyz, cNorth = g.cNorthHalfCells.xyz;
        const float2 logical = TaLogicalFromDirection(direction, cUp, cEast, cNorth, g.cUpPlanetRadius.w,
            g.cCenterOffsetOrigin.xy, g.cEastSpacing.w, g.cNorthHalfCells.w);
        float coarseVisibility;
        float2 coarseLean;
        if (TaSampleLevel(g_ambient, g.cDims.y, g.cDims.x, asuint(g.cCenterOffsetOrigin.z),
                asuint(g.cCenterOffsetOrigin.w), logical, coarseVisibility, coarseLean) &&
            coarseVisibility < visibility)
        {
            visibility = coarseVisibility;
            const float3 world = cEast * coarseLean.x + cNorth * coarseLean.y;
            lean = float2(dot(world, east), dot(world, north));
        }
    }

    g_ambient.Store((g.dims.y + TaPhysicalIndex(id.x, id.y, resolution, originX, originY)) * 4u,
        EncodeTexel(visibility, lean));
}
)";

void StoreLevel(
    std::array<u32, kPushConstantDwords>& out,
    const u32 first,
    const GpuTerrainAmbientLevel& level,
    const f64 planetRadius)
{
    const auto put = [&](const u32 index, const f64 value)
    { out[index] = std::bit_cast<u32>(static_cast<f32>(value)); };
    put(first + 0, level.surfaceFrame.up.x);
    put(first + 1, level.surfaceFrame.up.y);
    put(first + 2, level.surfaceFrame.up.z);
    put(first + 3, planetRadius);
    put(first + 4, level.surfaceFrame.east.x);
    put(first + 5, level.surfaceFrame.east.y);
    put(first + 6, level.surfaceFrame.east.z);
    put(first + 7, level.spacingMeters);
    put(first + 8, level.surfaceFrame.north.x);
    put(first + 9, level.surfaceFrame.north.y);
    put(first + 10, level.surfaceFrame.north.z);
    put(first + 11, (static_cast<f64>(level.resolution) - 1.0) * 0.5);
    put(first + 12, level.centerOffsetMeters.x);
    put(first + 13, level.centerOffsetMeters.y);
    out[first + 14] = level.originX;
    out[first + 15] = level.originY;
}
} // namespace

GpuTerrainAmbient::GpuTerrainAmbient(
    rhi::Device& device,
    const shader::Compiler& shaderCompiler)
{
    // The common shader is spliced between the two halves of the literal.
    std::string source(kComputeSource);
    const std::string marker = "#define TA_BUFFER RWByteAddressBuffer\n";
    const auto at = source.find(marker);
    if (at == std::string::npos)
        throw std::logic_error("Terrain ambient compute shader marker not found.");
    source.insert(at + marker.size(), terrain::kTerrainAmbientCommonShader);

    const shader::Binary compute = shaderCompiler.Compile({
        .source = source,
        .entryPoint = "main",
        .stage = shader::Stage::Compute,
        .debug = false
    });
    if (compute.bytecode.empty())
        throw std::runtime_error("Orbit failed to compile the terrain ambient compute shader.");

    pipeline_ = device.CreateComputePipeline({
        .computeShader = {
            .data = compute.bytecode.data(),
            .size = compute.bytecode.size()
        },
        .pushConstantDwords = kPushConstantDwords,
        .shaderResourceBuffers = 2
    });
}

GpuTerrainAmbient::~GpuTerrainAmbient() = default;

void GpuTerrainAmbient::Clear(
    rhi::CommandList& commandList,
    const u32 sampleCount,
    rhi::Buffer& ambient) const
{
    if (sampleCount == 0U)
        return;
    std::array<u32, kPushConstantDwords> constants{};
    constants[16 + 2] = 1U;
    constants[16 + 3] = sampleCount;
    const u32 side = static_cast<u32>(std::ceil(std::sqrt(static_cast<f64>(sampleCount))));
    const u32 groups = (side + kThreadGroupSize - 1U) / kThreadGroupSize;
    commandList.SetComputePipeline(*pipeline_);
    // Binding 0 is never read in clear mode, but the slot still needs a buffer.
    commandList.SetComputeBuffer(0, ambient);
    commandList.SetComputeBuffer(1, ambient);
    commandList.SetComputeConstants(constants);
    commandList.Dispatch(groups, groups, 1U);
}

void GpuTerrainAmbient::Dispatch(
    rhi::CommandList& commandList,
    const GpuTerrainAmbientRequest& request,
    rhi::Buffer& samples,
    rhi::Buffer& ambient) const
{
    if (request.level.resolution < 5U)
        return;
    std::array<u32, kPushConstantDwords> constants{};
    StoreLevel(constants, 0U, request.level, request.planetRadiusMeters);
    constants[16 + 0] = request.level.resolution;
    constants[16 + 1] = request.level.firstSample;
    if (request.coarser.resolution != 0U)
    {
        StoreLevel(constants, 20U, request.coarser, request.planetRadiusMeters);
        constants[36 + 0] = request.coarser.resolution;
        constants[36 + 1] = request.coarser.firstSample;
    }
    const u32 groups =
        (request.level.resolution + kThreadGroupSize - 1U) / kThreadGroupSize;
    commandList.SetComputePipeline(*pipeline_);
    commandList.SetComputeBuffer(0, samples);
    commandList.SetComputeBuffer(1, ambient);
    commandList.SetComputeConstants(constants);
    commandList.Dispatch(groups, groups, 1U);
}
} // namespace orbit::terrain_gpu
