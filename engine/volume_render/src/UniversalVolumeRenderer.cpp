#include <orbit/cloud_genera/CloudGenera.hpp>
#include <orbit/volume_fields/VolumeFieldStorage.hpp>

#include <orbit/volume_render/UniversalVolumeRenderer.hpp>
#include <orbit/lighting/RadianceClipmapResidency.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace orbit::volume_render
{
namespace
{
std::atomic<u32> gCloudLookVariant{8U};
} // namespace

void SetCloudLookVariant(const u32 variant) noexcept
{
    gCloudLookVariant.store(std::min(variant, 8U));
}

u32 CloudLookVariant() noexcept
{
    return gCloudLookVariant.load();
}

namespace
{
constexpr u32 kInvalidSlot = ~0U;
constexpr u32 kMaximumVolumeLocalLights = 16U;

struct alignas(16) GpuVolumeParams
{
    math::Float4 cameraForwardAspect{};
    math::Float4 cameraUpTanHalfFov{};
    math::Float4 cameraPositionTemporal{};
    math::Float4 domainCenterExtinction{};
    math::Float4 domainHalfAlbedo{};
    math::Float4 scatteringColorAnisotropy{};
    math::Float4 emissionColorScale{};
    math::Float4 stellarDirectionScale{};
    math::Float4 stellarColorAmbient{};
    math::Float4 depthRangeHistory{};

    std::array<i32,4> minimumTile{};
    std::array<u32,4> tileLayout{};
    std::array<u32,4> logicalResolution{};
    std::array<u32,4> renderParams{};
    // Offset of the field tile grid from the world origin (VolumeFieldStorage).
    math::Float4 gridAnchor{};
    // Atmosphere-filtered sun colour at the top of a static volume.
    math::Float4 sunColorTop{1.0F, 1.0F, 1.0F, 0.0F};
    // Atmosphere LUT geometry: bottom radius, top radius, 1 = LUTs bound.
    math::Float4 atmosphere{};
    // Box around the density of a static volume (xyz), w = 1 when valid.
    math::Float4 occupancyMinimum{};
    math::Float4 occupancyMaximum{};
    // x = Cloud Type of the volume (0 = storm, typed per sample).
    math::Float4 typeParams{};
};

static_assert(
    sizeof(GpuVolumeParams) == 320U);

[[nodiscard]] render_graph::BufferHandle
FindField(
    const volume_fields::ImportedVolumeFields& fields,
    const world_model::VolumeField field)
{
    for (const auto& channel :
         fields.channels)
    {
        if (channel.field == field)
        {
            return channel.buffer;
        }
    }

    return {};
}

[[nodiscard]] std::unique_ptr<rhi::GraphicsPipeline>
CompileFullscreen(
    rhi::Device& device,
    const shader::Compiler& compiler,
    const char* pixelSource,
    const u32 pushDwords,
    const u32 buffers,
    const u32 textures)
{
    constexpr const char* kVs = R"(
struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput main(uint vertexId : SV_VertexID)
{
    const float2 p[6] =
    {
        float2(-1,-1),
        float2(-1, 1),
        float2( 1,-1),
        float2( 1,-1),
        float2(-1, 1),
        float2( 1, 1)
    };

    const float2 uv[6] =
    {
        float2(0,1),
        float2(0,0),
        float2(1,1),
        float2(1,1),
        float2(0,0),
        float2(1,0)
    };

    VSOutput o;
    o.position = float4(p[vertexId], 0, 1);
    o.uv = uv[vertexId];
    return o;
}
)";

    const auto vs =
        compiler.Compile({
            .source = kVs,
            .entryPoint = "main",
            .stage = shader::Stage::Vertex,
            .debug = false
        });

    const auto ps =
        compiler.Compile({
            .source = pixelSource,
            .entryPoint = "main",
            .stage = shader::Stage::Pixel,
            .debug = false
        });

    if (vs.bytecode.empty() ||
        ps.bytecode.empty())
    {
        throw std::runtime_error(
            "Orbit failed to compile M35 universal volume shaders.");
    }

    return device.CreateGraphicsPipeline({
        .vertexShader = {
            .data = vs.bytecode.data(),
            .size = vs.bytecode.size()
        },
        .pixelShader = {
            .data = ps.bytecode.data(),
            .size = ps.bytecode.size()
        },
        .vertexAttributes = {},
        .vertexStrideBytes = 0U,
        .pushConstantDwords = pushDwords,
        .shaderResourceBuffers = buffers,
        .sampledTextures = textures,
        .topology = rhi::PrimitiveTopology::TriangleList,
        .fillMode = rhi::FillMode::Solid,
        .cullMode = rhi::CullMode::None,
        .blendMode = rhi::BlendMode::Opaque,
        .depthTest = false,
        .depthWrite = false,
        .colorAttachmentFormats = {
            rhi::TextureFormat::RGBA16_Float
        },
        .colorAttachmentCount = 1U
    });
}

constexpr const char* kRaymarchPs = R"(
//@CLOUD_GENERA@
struct GpuLocalLight
{
    float4 positionType;
    float4 directionRange;
    float4 colorFlux;
    float4 cone;
};

struct GpuRadianceCell
{
    float4 irradiance0;
    float4 irradianceX;
    float4 irradianceY;
    float4 irradianceZ;
};

struct GpuRadianceLevelInfo
{
    float4 centerCellSize;
    uint4 moduloAxis;
    uint4 offsetCountLevel;
};

struct GpuVolumeParams
{
    float4 cameraForwardAspect;
    float4 cameraUpTanHalfFov;
    float4 cameraPositionTemporal;
    float4 domainCenterExtinction;
    float4 domainHalfAlbedo;
    float4 scatteringColorAnisotropy;
    float4 emissionColorScale;
    float4 stellarDirectionScale;
    float4 stellarColorAmbient;
    float4 depthRangeHistory;

    int4 minimumTile;
    uint4 tileLayout;
    uint4 logicalResolution;
    uint4 renderParams;
    float4 gridAnchor;
    float4 sunColorTop;
    float4 atmosphere;
    float4 occupancyMinimum;
    float4 occupancyMaximum;
    float4 typeParams;
};

[[vk::binding(0, 0)]]
ByteAddressBuffer g_density : register(t0);
[[vk::binding(1, 0)]]
ByteAddressBuffer g_emission : register(t1);
[[vk::binding(2, 0)]]
StructuredBuffer<uint> g_slotMap : register(t2);
[[vk::binding(3, 0)]]
StructuredBuffer<GpuLocalLight> g_localLights : register(t3);
[[vk::binding(4, 0)]]
StructuredBuffer<GpuVolumeParams> g_params : register(t4);
[[vk::binding(5, 0)]]
StructuredBuffer<GpuRadianceCell> g_radianceCells : register(t5);
[[vk::binding(6, 0)]]
StructuredBuffer<GpuRadianceLevelInfo> g_radianceLevels : register(t6);
[[vk::binding(7, 0)]]
StructuredBuffer<uint4> g_particleLightGrid : register(t7);

[[vk::binding(8, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_depth;
[[vk::binding(8, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_depthSampler;

[[vk::binding(9, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_history;
[[vk::binding(9, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_historySampler;

[[vk::binding(10, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_transmittance;
[[vk::binding(10, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_transmittanceSampler;

[[vk::binding(11, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_multi;
[[vk::binding(11, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_multiSampler;

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float SafeRcp(float x)
{
    return
        abs(x) > 1.0e-8
            ? 1.0 / x
            : (x >= 0.0
                ? 1.0e8
                : -1.0e8);
}

bool RayBox(
    float3 origin,
    float3 direction,
    float3 minimum,
    float3 maximum,
    out float nearDistance,
    out float farDistance)
{
    const float3 inv =
        float3(
            SafeRcp(direction.x),
            SafeRcp(direction.y),
            SafeRcp(direction.z));

    const float3 a =
        (minimum - origin) * inv;
    const float3 b =
        (maximum - origin) * inv;

    const float3 lo = min(a, b);
    const float3 hi = max(a, b);

    nearDistance =
        max(
            max(lo.x, lo.y),
            lo.z);
    farDistance =
        min(
            min(hi.x, hi.y),
            hi.z);

    return
        farDistance >=
            max(nearDistance, 0.0);
}

float ReverseZViewDepth(
    float depth,
    float nearPlane,
    float farPlane)
{
    nearPlane =
        max(nearPlane, 1.0e-5);
    farPlane =
        max(
            farPlane,
            nearPlane + 1.0e-4);

    return
        nearPlane * farPlane /
        max(
            depth *
                (farPlane - nearPlane) +
            nearPlane,
            1.0e-6);
}

// cell size/tile edge are packed into render-independent values reconstructed
// from domain extent and logical resolution. Keeping this helper explicit
// avoids a second coordinate convention for surface and Local3D volumes.
float SampleFieldNearest(
    ByteAddressBuffer field,
    float3 worldPosition,
    GpuVolumeParams p,
    float3 cellSize,
    uint tileEdge)
{
    worldPosition -= p.gridAnchor.xyz;

    const float3 tileWorld =
        cellSize *
        float(tileEdge);

    const int3 tile =
        int3(
            floor(
                worldPosition /
                tileWorld));

    const int3 logical =
        tile -
        p.minimumTile.xyz;

    if (any(logical < 0) ||
        logical.x >= int(p.tileLayout.x) ||
        logical.y >= int(p.tileLayout.y) ||
        logical.z >= int(p.tileLayout.z))
    {
        return 0.0;
    }

    const uint mapIndex =
        (uint(logical.z) *
             p.tileLayout.y +
         uint(logical.y)) *
            p.tileLayout.x +
        uint(logical.x);

    const uint slot =
        g_slotMap[mapIndex];

    if (slot == 0xffffffffu)
    {
        return 0.0;
    }

    const float3 globalCell =
        floor(
            worldPosition /
            cellSize);

    int3 local =
        int3(globalCell) -
        tile *
            int(tileEdge);

    local =
        clamp(
            local,
            0,
            int(tileEdge) - 1);

    const uint localIndex =
        uint(local.z) *
            tileEdge *
            tileEdge +
        uint(local.y) *
            tileEdge +
        uint(local.x);

    const uint tileCellCount =
        tileEdge *
        tileEdge *
        tileEdge;

    return
        asfloat(
            field.Load(
                (slot *
                     tileCellCount +
                 localIndex) *
                    4u));
}

)"
R"(
// Static (baked/procedural) volumes read the field trilinearly between cell
// centres; the live solver keeps nearest reads of its world-aligned cells.
float SampleField(
    ByteAddressBuffer field,
    float3 worldPosition,
    GpuVolumeParams p,
    float3 cellSize,
    uint tileEdge)
{
    if (p.gridAnchor.w < 0.5)
    {
        return SampleFieldNearest(
            field, worldPosition, p, cellSize, tileEdge);
    }

    const float3 grid =
        (worldPosition - p.gridAnchor.xyz) / cellSize - 0.5;
    const float3 base = floor(grid);
    const float3 f = grid - base;

    float accumulated = 0.0;

    [unroll]
    for (int z = 0; z < 2; ++z)
    {
        [unroll]
        for (int y = 0; y < 2; ++y)
        {
            [unroll]
            for (int x = 0; x < 2; ++x)
            {
                const float weight =
                    (x == 0 ? 1.0 - f.x : f.x) *
                    (y == 0 ? 1.0 - f.y : f.y) *
                    (z == 0 ? 1.0 - f.z : f.z);
                if (weight <= 0.0)
                {
                    continue;
                }
                const float3 centre =
                    p.gridAnchor.xyz +
                    (base + float3(x, y, z) + 0.5) * cellSize;
                accumulated +=
                    weight *
                    SampleFieldNearest(
                        field, centre, p, cellSize, tileEdge);
            }
        }
    }

    // The baked density is 1 - exp(-tau). Scaling the optical depth by k
    // (1 - (1 - d)^k) lifts thin anvil and cirrus, whose condensate is a few
    // hundredths of a g/kg, without touching the opaque core and without
    // simulating again.
    return CloudBoost(accumulated);
}

)"
R"(
float Hash13(float3 q)
{
    q = frac(q * 0.1031);
    q += dot(q, q.zyx + 31.32);
    return frac((q.x + q.y) * q.z);
}

float ValueNoise(float3 q)
{
    const float3 i = floor(q);
    float3 f = frac(q);
    f = f * f * (3.0 - 2.0 * f);
    const float n000 = Hash13(i);
    const float n100 = Hash13(i + float3(1, 0, 0));
    const float n010 = Hash13(i + float3(0, 1, 0));
    const float n110 = Hash13(i + float3(1, 1, 0));
    const float n001 = Hash13(i + float3(0, 0, 1));
    const float n101 = Hash13(i + float3(1, 0, 1));
    const float n011 = Hash13(i + float3(0, 1, 1));
    const float n111 = Hash13(i + float3(1, 1, 1));
    return lerp(
        lerp(lerp(n000, n100, f.x), lerp(n010, n110, f.x), f.y),
        lerp(lerp(n001, n101, f.x), lerp(n011, n111, f.x), f.y),
        f.z);
}

)"
R"(
// Billowy erosion of the low-resolution field edge so a baked 2 km grid does
// not read as boxes: detail only removes density, a solid core stays solid.
// Sub-voxel character of each generated genus (the bake holds the layout, this
// adds the grain, fibres and rims the grid is too coarse to store). The look
// variant switches three independent techniques so they can be compared live:
// bit 0 warps the domain (curled strands, swirled billows) and adds a finer
// octave, bit 1 uses ridged noise (crisp fibres and cauliflower creases).
// Look variant of this volume: 0-7 forces one, anything higher (8, auto) takes
// the genus' own choice from the cloud_genera table.
uint LookVariantOf(
    GpuVolumeParams p,
    uint genus)
{
    const uint forced = uint(p.typeParams.y + 0.5);
    return forced > 7u ? kCloudGenusLook[genus] : forced;
}

float Ridge(float v)
{
    return 1.0 - abs(2.0 * v - 1.0);
}

float ErodeGenus(
    float density,
    float3 relIn,
    uint genus,
    uint variant)
{
    const bool warpOn = (variant & 1u) != 0u;
    const bool ridged = (variant & 2u) != 0u;
    float3 rel = relIn;
    if (warpOn)
    {
        const float3 w =
            float3(
                ValueNoise(relIn / 900.0 + 3.0),
                ValueNoise(relIn / 900.0 + 9.0),
                ValueNoise(relIn / 900.0 + 17.0)) - 0.5;
        rel += w * float3(700.0, 700.0, 120.0) * (genus == 1u ? 1.8 : 1.0);
    }
    if (genus == 1u)
    {
        // Cirrus: hair-thin strands along the high wind.
        const float2 wind = normalize(float2(0.97, 0.23));
        const float along = dot(rel.xy, wind);
        const float across = rel.x * -wind.y + rel.y * wind.x;
        const float f0 = ValueNoise(float3(along / 5000.0, across / 420.0, rel.z / 400.0));
        const float f1 = ValueNoise(float3(along / 2500.0 + 9.0, across / 210.0, rel.z / 250.0));
        float streak = 0.6 * f0 + 0.4 * f1;
        float lo = 0.42;
        float width = 0.22;
        if (warpOn)
        {
            streak = 0.5 * streak + 0.5 * ValueNoise(float3(along / 1300.0 + 4.0, across / 105.0, rel.z / 160.0));
        }
        if (ridged)
        {
            streak = 0.5 * Ridge(f0) + 0.5 * Ridge(f1);
            lo = 0.5;
            width = 0.26;
        }
        return saturate(density * 2.6 * saturate((streak - lo) / width));
    }
    if (genus == 2u)
    {
        // Cirrocumulus: small crisp grains with gaps.
        const float a0 = ValueNoise(rel / float3(220.0, 220.0, 150.0));
        const float a1 = ValueNoise(rel / float3(110.0, 110.0, 90.0) + 5.0);
        float grain = 0.6 * a0 + 0.4 * a1;
        if (ridged)
        {
            grain = 0.6 * Ridge(a0) + 0.4 * Ridge(a1);
        }
        return saturate((density * (0.35 + 1.3 * grain) - 0.1) / 0.75);
    }
    if (genus == 3u)
    {
        // Cirrostratus: an even veil, faint streaks.
        return saturate(density * (0.85 + 0.3 * ValueNoise(rel / float3(3000.0, 3000.0, 500.0))));
    }
    if (genus == 4u)
    {
        // Altocumulus: rounded cloudlets with crisp rims.
        const float a0 = ValueNoise(rel / float3(430.0, 430.0, 260.0));
        const float a1 = ValueNoise(rel / float3(190.0, 190.0, 130.0) + 3.0);
        float n = 0.6 * a0 + 0.4 * a1;
        if (ridged)
        {
            n = lerp(n, 0.6 * Ridge(a0) + 0.4 * Ridge(a1), 0.6);
        }
        return saturate((density - (1.0 - n) * 0.30) * 1.5);
    }
    if (genus == 5u)
    {
        // Altostratus: a smooth grey sheet.
        return saturate(density * (0.88 + 0.24 * ValueNoise(rel / float3(5000.0, 5000.0, 900.0))));
    }
    if (genus == 6u)
    {
        // Nimbostratus: heavy and even, ragged where it thins.
        const float n = ValueNoise(rel / float3(700.0, 700.0, 260.0));
        return saturate(
            (density - (1.0 - n) * 0.22 * (1.0 - density)) *
            (0.9 + 0.2 * ValueNoise(rel / float3(6000.0, 6000.0, 1200.0))));
    }
    if (genus == 8u)
    {
        // Stratus: smooth and low.
        return saturate(density * (0.9 + 0.2 * ValueNoise(rel / float3(2400.0, 2400.0, 300.0))));
    }
    // 7 stratocumulus (soft lumps), 9 cumulus (cauliflower billows).
    const bool cumulus = genus == 9u;
    const float3 q = rel / (cumulus ? float3(380.0, 380.0, 330.0) : float3(420.0, 420.0, 260.0));
    const float n0 = ValueNoise(q);
    const float n1 = ValueNoise(q * 2.2 + 11.0);
    const float n2 = ValueNoise(q * 4.6 + 31.0);
    float n = 0.5 * n0 + 0.3 * n1 + 0.2 * n2;
    if (ridged)
    {
        // Creases between billows: the cauliflower look.
        n = lerp(n, 0.5 * Ridge(n0) + 0.3 * Ridge(n1) + 0.2 * Ridge(n2), 0.7);
    }
    if (warpOn)
    {
        n = 0.8 * n + 0.2 * ValueNoise(q * 9.5 + 53.0);
    }
    const float carve = cumulus ? 0.95 : 0.8;
    return saturate((density - (1.0 - n) * carve * (1.0 - (cumulus ? 0.45 : 0.6) * density)) * (cumulus ? 2.1 : 1.8));
}

)"
R"(
float ErodeDensity(
    float density,
    float3 relativeToDomain,
    float cellMetres,
    float iceType,
    float midType,
    bool authoredLayer)
{
    const float3 q = relativeToDomain / max(cellMetres, 1.0);
    // Features no finer than the march step (about a cell): finer noise is
    // aliased by the steps into grain instead of reading as billows.
    const float n =
        0.60 * ValueNoise(q * 0.45) +
        0.30 * ValueNoise(q * 0.95 + 17.0) +
        0.10 * ValueNoise(q * 1.9 + 41.0);
    // Cumulus / cumulonimbus: round billows, a solid core stays solid.
    const float cumulus =
        saturate(density - (1.0 - n) * 0.32 * (1.0 - density * 0.6));

    float result = cumulus;

    if (midType > 0.001)
    {
        // Altocumulus / altostratus: flat sheets broken into rounded cloudlets
        // about a kilometre across, thin and even, no towering relief.
        const float cells =
            0.55 * ValueNoise(relativeToDomain / float3(1700.0, 1700.0, 600.0) + 3.0) +
            0.30 * ValueNoise(relativeToDomain / float3(800.0, 800.0, 400.0) + 9.0) +
            0.15 * ValueNoise(relativeToDomain / float3(380.0, 380.0, 300.0) + 21.0);
        const float sheet = density * saturate((cells - 0.22) / 0.45);
        result = lerp(result, sheet * (0.6 + 0.8 * ValueNoise(relativeToDomain / 260.0 + 5.0)), midType);
    }

    if (iceType > 0.001)
    {
        // Cirrus / cirrostratus: ice, drawn out into fibres along the high
        // wind (the storm's own motion), long along and razor thin across.
        const float2 wind = normalize(float2(0.97, 0.23));
        const float along = dot(relativeToDomain.xy, wind);
        const float across =
            relativeToDomain.x * -wind.y + relativeToDomain.y * wind.x;
        const float fibre =
            0.55 * ValueNoise(float3(along / 9000.0, across / 1100.0, relativeToDomain.z / 1400.0)) +
            0.30 * ValueNoise(float3(along / 5200.0 + 11.0, across / 450.0, relativeToDomain.z / 800.0)) +
            0.15 * ValueNoise(float3(along / 2600.0 + 5.0, across / 220.0, relativeToDomain.z / 450.0));
        const float strands = density * saturate((fibre - 0.28) / 0.38);
        result = lerp(result, strands * (0.55 + 0.9 * ValueNoise(relativeToDomain / float3(420.0, 160.0, 240.0) + 13.0)), iceType);
    }

    return result;
}

int PositiveModulo(
    int value,
    int modulus)
{
    const int result =
        value % modulus;
    return
        result < 0
            ? result + modulus
            : result;
}

float3 SampleRadianceCache(
    float3 cameraRelativePosition,
    uint levelCount)
{
    [loop]
    for (uint levelIndex = 0u;
         levelIndex < levelCount;
         ++levelIndex)
    {
        const GpuRadianceLevelInfo level =
            g_radianceLevels[levelIndex];

        const float cellSize =
            max(
                level.centerCellSize.w,
                1.0e-5);
        const int axis =
            int(level.moduloAxis.w);

        if (axis <= 0)
        {
            continue;
        }

        const float3 relative =
            (cameraRelativePosition -
             level.centerCellSize.xyz) /
            cellSize;

        const int3 delta =
            int3(round(relative));
        const int halfAxis =
            axis / 2;

        if (any(abs(delta) >
                halfAxis))
        {
            continue;
        }

        const int px =
            PositiveModulo(
                int(level.moduloAxis.x) +
                    delta.x,
                axis);
        const int py =
            PositiveModulo(
                int(level.moduloAxis.y) +
                    delta.y,
                axis);
        const int pz =
            PositiveModulo(
                int(level.moduloAxis.z) +
                    delta.z,
                axis);

        const uint localIndex =
            uint(px) +
            uint(axis) *
                (uint(py) +
                 uint(axis) *
                     uint(pz));

        if (localIndex >=
            level.offsetCountLevel.y)
        {
            continue;
        }

        const GpuRadianceCell cell =
            g_radianceCells[
                level.offsetCountLevel.x +
                localIndex];

        if (cell.irradiance0.w <= 0.5)
        {
            continue;
        }

        // The cache L0 coefficient is the stable low-frequency incident
        // authority. Directional direct lights are evaluated separately.
        return
            max(
                cell.irradiance0.rgb,
                0.0);
    }

    return 0.0;
}

float HenyeyGreenstein(
    float cosineTheta,
    float anisotropy)
{
    const float g =
        clamp(
            anisotropy,
            -0.95,
            0.95);
    const float g2 =
        g * g;
    const float denominator =
        max(
            1.0 +
                g2 -
                2.0 *
                g *
                clamp(
                    cosineTheta,
                    -1.0,
                    1.0),
            1.0e-4);

    return
        (1.0 - g2) /
        (12.566370614359172 *
         pow(
             denominator,
             1.5));
}

float RangeAttenuation(
    float distanceMeters,
    float rangeMeters)
{
    const float normalized =
        distanceMeters /
        max(
            rangeMeters,
            1.0e-4);
    const float quartic =
        normalized *
        normalized *
        normalized *
        normalized;
    const float smooth =
        saturate(
            1.0 - quartic);

    return smooth * smooth;
}

float LocalLightScale(
    GpuLocalLight light,
    float distanceMeters,
    float3 sampleToLight)
{
    const float luminousFlux =
        max(
            light.colorFlux.w,
            0.0);

    const float radiantWatts =
        luminousFlux /
        683.0;

    const bool spot =
        light.positionType.w > 0.5;

    float solidAngle =
        12.566370614359172;
    float angular =
        1.0;

    if (spot)
    {
        const float outerCos =
            clamp(
                light.cone.y,
                -1.0,
                1.0);

        solidAngle =
            max(
                6.283185307179586 *
                    (1.0 -
                     outerCos),
                1.0e-4);

        angular =
            smoothstep(
                outerCos,
                max(
                    light.cone.x,
                    outerCos +
                        1.0e-5),
                dot(
                    -sampleToLight,
                    normalize(
                        light.directionRange.xyz)));
    }

    const float radiantIntensity =
        radiantWatts /
        solidAngle;

    return
        radiantIntensity /
        max(
            distanceMeters *
                distanceMeters,
            0.0025) *
        RangeAttenuation(
            distanceMeters,
            light.directionRange.w) *
        angular /
        1361.0;
}

float4 SampleParticleLightGrid(float3 worldPosition)
{
    const uint4 meta = g_particleLightGrid[1];
    const float3 origin = float3(asfloat(meta.x),asfloat(meta.y),asfloat(meta.z));
    const float cellSize = asfloat(meta.w);
    if (!(cellSize > 0.0)) return 0.0;
    const int3 cell = int3(floor((worldPosition-origin)/cellSize));
    const int resolution = 32;
    if (any(cell < 0) || any(cell >= int3(resolution,resolution,resolution))) return 0.0;
    const uint index = 2u + (uint(cell.z)*32u + uint(cell.y))*32u + uint(cell.x);
    const uint4 packed = g_particleLightGrid[index];
    return float4(float(packed.x)/4096.0, float3(packed.y,packed.z,packed.w)/1024.0);
}

float ParticleGridTransmittance(float3 start,float3 direction,float maximumDistance)
{
    if (maximumDistance <= 1.0e-4) return 1.0;
    const float3 d=normalize(direction);
    const float distance=min(maximumDistance,192.0);
    const float stepLength=max(distance/6.0,8.0);
    float optical=0.0;
    [unroll] for(uint i=1u;i<=6u;++i)
    {
        const float t=min(stepLength*float(i),distance);
        optical += SampleParticleLightGrid(start+d*t).x * 0.18;
    }
    return exp(-min(optical,20.0));
}

float ShadowTransmittance(
    float3 start,
    float3 direction,
    float maximumDistance,
    GpuVolumeParams p,
    float3 cellSize,
    uint tileEdge)
{
    // A static cloud is tens of kilometres across: two shadow steps over the
    // whole box exit sample almost nothing and light the cloud at random. Use
    // a denser march with quadratic spacing (fine near the sample, where the
    // optical depth builds up) and stop after 32 km, a bit more than the
    // storm is tall.
    const bool smoothShadow = p.gridAnchor.w > 0.5;
    // Thin ice and sheet layers do not shade themselves.
    const uint genusCode = uint(p.typeParams.x + 0.5);
    if (smoothShadow && genusCode != 0u &&
        (kCloudGenusLight[genusCode].x > 0.5 || genusCode == 5u || genusCode == 6u || genusCode == 8u))
    {
        return 1.0;
    }
    const uint steps =
        smoothShadow
            ? (p.renderParams.y == 0u ? 0u : (p.typeParams.x > 0.5 ? 5u : 16u))
            : p.renderParams.y;

    if (steps == 0u ||
        maximumDistance <= 1.0e-4)
    {
        return 1.0;
    }

    float nearBox;
    float farBox;

    const float3 minimum =
        p.domainCenterExtinction.xyz -
        p.domainHalfAlbedo.xyz;
    const float3 maximum =
        p.domainCenterExtinction.xyz +
        p.domainHalfAlbedo.xyz;

    if (!RayBox(
            start,
            direction,
            minimum,
            maximum,
            nearBox,
            farBox))
    {
        return 1.0;
    }

    float distance =
        min(
            maximumDistance,
            max(
                farBox,
                0.0));

    if (smoothShadow)
    {
        distance = min(distance, 32000.0);
    }

    if (distance <= 1.0e-4)
    {
        return 1.0;
    }

    const float stepLength =
        distance /
        float(steps);

    float opticalDepth =
        0.0;

    [loop]
    for (uint index = 0u;
         index < steps;
         ++index)
    {
        const float u =
            (float(index) + 0.5) /
            float(steps);
        const float t =
            smoothShadow
                ? distance * u * u
                : (float(index) + 0.5) *
                    stepLength;
        const float segment =
            smoothShadow
                ? distance * 2.0 * u / float(steps)
                : stepLength;

        const float density =
            max(
                SampleField(
                    g_density,
                    start +
                        direction * t,
                    p,
                    cellSize,
                    tileEdge),
                0.0);

        opticalDepth +=
            density *
            p.domainCenterExtinction.w *
            segment;

        if (opticalDepth > 12.0)
        {
            return 0.0;
        }
    }

    return
        exp(-opticalDepth);
}

)"
R"(
// Same lookup the planet clouds use for the sun transmittance LUT.
float2 LutUv(float radius, float sunMu, GpuVolumeParams p)
{
    const float rb = p.atmosphere.x;
    const float rt = p.atmosphere.y;
    const float unit = saturate((radius * radius - rb * rb) / max(rt * rt - rb * rb, 1.0));
    return float2(saturate(sunMu * 0.5 + 0.5), unit);
}

)"
R"(
float4 main(VSOutput input) : SV_Target0
{
    const GpuVolumeParams p =
        g_params[0];

    const float3 forward =
        normalize(
            p.cameraForwardAspect.xyz);
    const float3 requestedUp =
        normalize(
            p.cameraUpTanHalfFov.xyz);
    const float3 right =
        normalize(
            cross(
                forward,
                requestedUp));
    const float3 up =
        normalize(
            cross(
                right,
                forward));

    const float2 ndc =
        float2(
            input.uv.x * 2.0 - 1.0,
            1.0 -
                input.uv.y * 2.0);

    const float3 rayDirection =
        normalize(
            forward +
            right *
                (ndc.x *
                 max(
                     p.cameraForwardAspect.w,
                     0.001) *
                 max(
                     p.cameraUpTanHalfFov.w,
                     0.001)) +
            up *
                (ndc.y *
                 max(
                     p.cameraUpTanHalfFov.w,
                     0.001)));

    const float3 rayOrigin =
        p.cameraPositionTemporal.xyz;

    float3 minimum =
        p.domainCenterExtinction.xyz -
        p.domainHalfAlbedo.xyz;
    float3 maximum =
        p.domainCenterExtinction.xyz +
        p.domainHalfAlbedo.xyz;

    if (p.gridAnchor.w > 0.5 && p.occupancyMinimum.w > 0.5)
    {
        // Only the box that holds density: rays elsewhere return at once.
        minimum = max(minimum, p.occupancyMinimum.xyz);
        maximum = min(maximum, p.occupancyMaximum.xyz);
    }

    float nearDistance;
    float farDistance;

    if (!RayBox(
            rayOrigin,
            rayDirection,
            minimum,
            maximum,
            nearDistance,
            farDistance))
    {
        return float4(
)"
R"(            0.0,0.0,0.0,1.0);
    }

    nearDistance =
        max(
            nearDistance,
            0.0);

    const float depth =
        g_depth.Sample(
            g_depthSampler,
            input.uv).r;

    if (depth > 0.0)
    {
        const float viewDepth =
            ReverseZViewDepth(
                depth,
                p.depthRangeHistory.x,
                p.depthRangeHistory.y);

        const float rayForward =
            max(
                dot(
                    rayDirection,
                    forward),
                1.0e-5);

        const float surfaceDistance =
            viewDepth /
            rayForward;

        farDistance =
            min(
                farDistance,
                surfaceDistance);
    }

    if (farDistance <= nearDistance)
    {
        return float4(
            0.0,0.0,0.0,1.0);
    }

    uint raySteps =
        max(
            p.renderParams.x,
            1u);
    const uint tileEdge =
        max(
            p.renderParams.z,
            1u);

    const float3 logicalResolution =
        float3(
            max(
                p.logicalResolution.x,
                1u),
            max(
                p.logicalResolution.y,
                1u),
            max(
                p.logicalResolution.z,
                1u));

    const float3 cellSize =
        (p.domainHalfAlbedo.xyz *
         2.0) /
        logicalResolution;

    const float segmentLength =
        farDistance -
        nearDistance;

    if (p.gridAnchor.w > 0.5)
    {
        // A static cloud spans tens of kilometres: a fixed handful of steps
        // skips thin layers (cirrus, anvil edges) and leaves concentric
        // bands. Step at about a cell and no coarser.
        // Distance along this ray to cross one cell on its fastest axis:
        // a near-horizontal ray is not held to the (much finer) vertical
        // cell size.
        const float3 crossing =
            cellSize / max(abs(rayDirection), 1.0e-3);
        const float cellCrossing =
            min(crossing.x, min(crossing.y, crossing.z));
        raySteps =
            clamp(
                uint(ceil(segmentLength / max(cellCrossing, 1.0))),
                raySteps,
                (p.typeParams.x > 0.5 ? 48u : 128u));
    }

    const float stepLength =
        segmentLength /
        float(raySteps);

    // Interleaved-gradient jitter of the first sample, so whatever banding is
    // left becomes fine fixed noise instead of rings.
    const float stepJitter =
        p.gridAnchor.w > 0.5
            ? frac(
                  52.9829189 *
                  frac(
                      dot(
                          input.position.xy,
                          float2(0.06711056, 0.00583715))) +
                  p.occupancyMaximum.w * 0.6180339887)
            : 0.5;

    float transmittance =
        1.0;
    float3 scatteringAccum =
        0.0;
    float3 emissionAccum =
        0.0;
    float shadowAccum =
        0.0;
    float shadowWeight =
        0.0;

    const float3 stellarDirection =
        normalize(
            p.stellarDirectionScale.xyz);
    const float stellarScale =
        max(
            p.stellarDirectionScale.w,
            0.0);
    const float3 stellarColor =
        max(
            p.stellarColorAmbient.rgb,
            0.0);
    const float ambient =
        p.gridAnchor.w > 0.5
            ? 0.8
            : max(
                p.stellarColorAmbient.w,
                0.0);

)"
R"(
    [loop]
    for (uint step = 0u;
         step < raySteps;
         ++step)
    {
        const float distance =
            nearDistance +
            (float(step) + stepJitter) *
                stepLength;

        const float3 worldPosition =
            rayOrigin +
            rayDirection *
                distance;

        float density =
            max(
                SampleField(
                    g_density,
                    worldPosition,
                    p,
                    cellSize,
                    tileEdge),
                0.0);

        // Cloud type from height above the ground and from how solid the
        // cloud is (the middle of a tower stays cumulonimbus whatever the
        // height): high thin cloud is ice (cirrus), mid-level thin cloud is
        // altocumulus / altostratus, the rest is cumulus / stratus.
        float iceType = 0.0;
        float midType = 0.0;

        const uint cloudType = uint(p.typeParams.x + 0.5);
        const bool authoredLayer = cloudType != 0u;

        if (p.gridAnchor.w > 0.5 && density > 0.002 && authoredLayer)
        {
            // The genus (1 cirrus ... 9 cumulus, kCloudGenusLight) decides
            // the shape and the lighting; no height or density heuristics.
            iceType = kCloudGenusLight[cloudType].x;
            density =
                ErodeGenus(
                    density,
                    worldPosition - p.domainCenterExtinction.xyz,
                    cloudType,
                    LookVariantOf(p, cloudType));
        }
        else if (p.gridAnchor.w > 0.5 && density > 0.002)
        {
            const float aboveGround =
                length(worldPosition) -
                (length(p.domainCenterExtinction.xyz) -
                 p.domainHalfAlbedo.z);
            // Judge how solid the cloud is on the baked density, before the
            // read-time optical-depth boost lifted the thin parts.
            const float bakedDensity =
                1.0 - pow(max(1.0 - density, 1.0e-4), 1.0 / 3.0);
            const float thin =
                1.0 - smoothstep(0.12, 0.35, bakedDensity);
            // The fast-core storm has no ice, so its anvil is dense water: above
            // the freezing levels everything reads as ice, thin parts fully.
            iceType = smoothstep(7000.0, 9500.0, aboveGround) * (0.15 + 0.85 * thin);
            midType =
                smoothstep(1800.0, 3000.0, aboveGround) *
                (1.0 - smoothstep(5500.0, 7500.0, aboveGround)) *
                thin;
            density =
                ErodeDensity(
                    density,
                    worldPosition - p.domainCenterExtinction.xyz,
                    min(cellSize.x, cellSize.z),
                    iceType,
                    midType,
                    false);
        }

        float emissionScalar =
            0.0;

        if (p.renderParams.w != 0u)
        {
            emissionScalar =
                max(
                    SampleField(
                        g_emission,
                        worldPosition,
                        p,
                        cellSize,
                        tileEdge),
                    0.0);
        }

        const float sigmaT =
            density *
            max(
                p.domainCenterExtinction.w,
                0.0);

        if (sigmaT <= 1.0e-8 &&
            emissionScalar <= 1.0e-8)
        {
            continue;
        }

        float3 scattering =
            0.0;
        float weightedShadow =
            1.0;
        float shadowSamples =
            1.0;

        if (sigmaT > 1.0e-8)
        {
            const bool staticCloud = p.gridAnchor.w > 0.5;
            float3 staticSource = float3(0.0, 0.0, 0.0);

            const float stellarShadow =
                ShadowTransmittance(
                    worldPosition +
                        stellarDirection *
                            0.01,
                    stellarDirection,
                    1.0e20,
                    p,
                    cellSize,
                    tileEdge) *
                ParticleGridTransmittance(
                    worldPosition + stellarDirection * 0.01,
                    stellarDirection,
                    192.0);

            const float phaseStellar =
                HenyeyGreenstein(
                    dot(
                        stellarDirection,
                        -rayDirection),
                    p.scatteringColorAnisotropy.w);

            // Static (baked) clouds are lit the way the planet clouds are
            // (celestial_clouds CloudRenderer): the sun as filtered by the
            // atmosphere at this height, the optical depth towards the sun
            // feeding four octaves of multiple scattering with a diffusion
            // falloff, a small sky term, a soft shoulder and a dark flat
            // base. The result is a radiance, so it replaces sigmaS*incident
            // below.
            float3 incident =
                staticCloud
                    ? float3(0.0, 0.0, 0.0)
                    : stellarColor *
                          stellarScale *
                          stellarShadow *
                          phaseStellar;

            if (staticCloud)
            {
                const float radiusHere = length(worldPosition);
                const float3 upHere = worldPosition / max(radiusHere, 1.0);
                const float heightInDomain =
                    saturate(
                        (radiusHere -
                         (length(p.domainCenterExtinction.xyz) -
                          p.domainHalfAlbedo.z)) /
                        max(2.0 * p.domainHalfAlbedo.z, 1.0));

                float3 sunTint =
                    lerp(
                        stellarColor,
                        p.sunColorTop.rgb,
                        heightInDomain);
                if (p.atmosphere.z > 0.5)
                {
                    sunTint =
                        g_transmittance.SampleLevel(
                            g_transmittanceSampler,
                            LutUv(
                                radiusHere,
                                dot(upHere, stellarDirection),
                                p),
                            0).rgb;
                }

                // View ray against the sun: looking towards the sun is forward
                // scattering (+1).
                const float cosine = dot(rayDirection, stellarDirection);
                const float sunDepth =
                    -log(max(stellarShadow, 1.0e-5));
                // Ice scatters in a sharp forward lobe (the silver edge of
                // cirrus facing the sun).
                const float singlePhase =
                    (0.1 * HenyeyGreenstein(cosine, 0.95) +
                     0.2 * HenyeyGreenstein(cosine, 0.8)) *
                    (1.0 + 0.3 * iceType);
                const float multiDepth = 1.6 * sqrt(sunDepth);
                float multiScatter = 0.0;
                float octave = 1.0;
                [unroll]
                for (uint octaveIndex = 0u; octaveIndex < 4u; ++octaveIndex)
                {
                    multiScatter +=
                        octave *
                        (3.0 * HenyeyGreenstein(cosine, 0.2 * octave) +
                         0.3 * HenyeyGreenstein(cosine, -0.4 * octave)) *
                        exp(-multiDepth * 0.8 * octave);
                    octave *= 0.5;
                }
)"
R"(
                // Ice crystals refract the sun into a 22 degree halo, seen
                // through thin high cloud.
                const float haloAngle = acos(clamp(cosine, -1.0, 1.0));
                const float halo =
                    iceType * (cloudType == 3u ? 0.5 : 0.18) *
                    exp(-pow((haloAngle - 0.384) / 0.03, 2.0));
                // Look variant bit 2: high contrast (silver rims toward the sun,
                // darker undersides).
                const bool contrastLook = (LookVariantOf(p, cloudType) & 4u) != 0u;
                const float silver =
                    contrastLook
                        ? 3.0 * pow(saturate(cosine), 12.0) * exp(-sunDepth * 1.2)
                        : 0.0;
                const float multi =
                    (singlePhase + halo) * 12.0 * exp(-sunDepth * 1.6) +
                    0.35 * multiScatter +
                    silver;
                const float skyFacing =
                    saturate(dot(upHere, stellarDirection) * 0.5 + 0.5);
                const float skyTerm =
                    (0.02 + 0.16 * heightInDomain * heightInDomain) *
                    skyFacing *
                    (0.35 + 0.65 * exp(-sunDepth * 0.12)) *
                    (1.0 + 0.5 * iceType);
                const float lit = multi + skyTerm;
                const float limited = lit / (1.0 + 0.25 * lit);
                const float thickness01 = saturate(density / 0.8);
                // A rain-bearing storm has a dark, flat base.
                // High ice has no dark base, a mid-level sheet only a faint one.
                float baseDark =
                    lerp(
                        lerp(
                            lerp(
                                1.0,
                                0.3,
                                (1.0 - smoothstep(0.0, 0.2, heightInDomain)) * 0.9),
                            0.75,
                            midType),
                        1.0,
                        iceType);
                if (authoredLayer)
                {
                    baseDark =
                        lerp(
                            1.0,
                            kCloudGenusLight[cloudType].z,
                            1.0 - smoothstep(0.0, 0.3, heightInDomain));
                    if (contrastLook)
                    {
                        baseDark = pow(baseDark, 1.7);
                    }
                }
                const float shading =
                    (0.62 + 0.38 * thickness01) * baseDark;
                staticSource =
                    stellarScale *
                    saturate(p.domainHalfAlbedo.w) *
                    sunTint *
                    limited *
                    4.6 *
                    shading *
                    (1.0 + 0.6 * iceType);
            }
            else
            {
                incident +=
                    ambient *
                    0.07957747154594767;
            }

            // Previous completed M38 particle grid contributes low-frequency
            // emissive radiance without downloading particle state.
            incident += SampleParticleLightGrid(worldPosition).yzw;

            const float3 sampleRelative =
                worldPosition -
                rayOrigin;

            const uint localCount =
                min(
                    p.tileLayout.w,
                    16u);

            weightedShadow =
                stellarShadow;

            [loop]
            for (uint lightIndex = 0u;
                 lightIndex < localCount;
                 ++lightIndex)
            {
                const GpuLocalLight light =
                    g_localLights[
                        lightIndex];

                const float3 delta =
                    light.positionType.xyz -
                    sampleRelative;
                const float lightDistance =
                    length(delta);

                if (lightDistance <= 1.0e-4 ||
                    lightDistance >=
                        light.directionRange.w)
                {
                    continue;
                }

                const float3 sampleToLight =
                    delta /
                    lightDistance;

                const float scale =
                    LocalLightScale(
                        light,
                        lightDistance,
                        sampleToLight);

                if (scale <= 0.0)
                {
                    continue;
                }

                const float shadow =
                    ShadowTransmittance(
                        worldPosition +
                            sampleToLight *
                                0.01,
                        sampleToLight,
                        lightDistance,
                        p,
                        cellSize,
                        tileEdge) *
                    ParticleGridTransmittance(
                        worldPosition + sampleToLight * 0.01,
                        sampleToLight,
                        lightDistance);

                const float phase =
                    HenyeyGreenstein(
                        dot(
                            sampleToLight,
                            -rayDirection),
                        p.scatteringColorAnisotropy.w);

                incident +=
                    max(
                        light.colorFlux.rgb,
                        0.0) *
                    scale *
                    shadow *
                    phase;

                weightedShadow +=
                    shadow;
                shadowSamples +=
                    1.0;
            }

            const float3 sigmaS =
                sigmaT *
                saturate(
                    p.domainHalfAlbedo.w) *
                max(
                    p.scatteringColorAnisotropy.rgb,
                    0.0);

            scattering =
                sigmaS *
                incident +
                sigmaT *
                staticSource;
        }

        const float3 emission =
            emissionScalar *
            max(
                p.emissionColorScale.rgb,
                0.0) *
            max(
                p.emissionColorScale.w,
                0.0);

        const float segmentT =
            sigmaT > 1.0e-8
                ? exp(
                      -sigmaT *
                      stepLength)
                : 1.0;

        const float integral =
            sigmaT > 1.0e-8
                ? (1.0 -
                   segmentT) /
                      sigmaT
                : stepLength;

        scatteringAccum +=
            transmittance *
            scattering *
            integral;
        emissionAccum +=
            transmittance *
            emission *
            integral;

        shadowAccum +=
            weightedShadow /
            shadowSamples;
        shadowWeight +=
            1.0;

        transmittance *=
            segmentT;

        if (transmittance <
            0.002)
        {
            break;
        }
    }

    const uint debugMode =
        uint(
            p.depthRangeHistory.w +
            0.5);

    float4 current;

    if (debugMode == 1u)
    {
        current =
            float4(
                scatteringAccum,
                0.0);
    }
    else if (debugMode == 2u)
    {
        const float extinction =
            1.0 -
            transmittance;
        current =
            float4(
                extinction.xxx,
                0.0);
    }
    else if (debugMode == 3u)
    {
        current =
            float4(
                emissionAccum,
                0.0);
    }
    else if (debugMode == 4u)
    {
        const float shadow =
            shadowWeight > 0.0
                ? shadowAccum /
                    shadowWeight
                : 1.0;
        current =
            float4(
                shadow.xxx,
                0.0);
    }
    else
    {
        current =
            float4(
                scatteringAccum +
                    emissionAccum,
                transmittance);
    }

    const bool historyValid =
        p.depthRangeHistory.z >
        0.5;

    const bool temporalEnabled =
        p.cameraPositionTemporal.w >
        0.0 &&
        debugMode == 0u;

    if (historyValid &&
        temporalEnabled)
    {
        const float4 history =
            g_history.Sample(
                g_historySampler,
                input.uv);

        const float radianceDelta =
            length(
                current.rgb -
                history.rgb);
        const float transmittanceDelta =
            abs(
                current.a -
                history.a);

        const float rejection =
            saturate(
                radianceDelta * (p.gridAnchor.w > 0.5 ? 0.9 : 2.5) +
                transmittanceDelta * (p.gridAnchor.w > 0.5 ? 2.5 : 6.0));

        const float historyWeight =
            saturate(
                p.cameraPositionTemporal.w) *
            (1.0 -
             rejection);

        current =
            lerp(
                current,
                history,
                historyWeight);
    }

    return current;
}
)";

constexpr const char* kCompositePs = R"(
[[vk::binding(0, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_scene;
[[vk::binding(0, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_sceneSampler;

[[vk::binding(1, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_volume;
[[vk::binding(1, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_volumeSampler;

struct Constants
{
    uint debugMode;
};

[[vk::push_constant]]
Constants g;

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 main(VSOutput input) : SV_Target0
{
    const float4 volume =
        g_volume.Sample(
            g_volumeSampler,
            input.uv);

    if (g.debugMode != 0u)
    {
        return float4(
            max(volume.rgb, 0.0),
            1.0);
    }

    const float3 scene =
        g_scene.Sample(
            g_sceneSampler,
            input.uv).rgb;

    return float4(
        max(
            volume.rgb +
            scene *
                saturate(volume.a),
            0.0),
        1.0);
}
)";

constexpr const char* kCopyPs = R"(
[[vk::binding(0, 0)]]
[[vk::combinedImageSampler]]
Texture2D g_source;
[[vk::binding(0, 0)]]
[[vk::combinedImageSampler]]
SamplerState g_sampler;

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float4 main(VSOutput input) : SV_Target0
{
    return
        g_source.Sample(
            g_sampler,
            input.uv);
}
)";

void ConfigureFullscreen(
    rhi::CommandList& commands,
    rhi::Texture& target,
    const u32 width,
    const u32 height)
{
    commands.SetRenderTarget(target);
    commands.SetViewport({
        .x = 0.0F,
        .y = 0.0F,
        .width =
            static_cast<f32>(width),
        .height =
            static_cast<f32>(height),
        .minDepth = 0.0F,
        .maxDepth = 1.0F
    });
    commands.SetScissor({
        .left = 0,
        .top = 0,
        .right =
            static_cast<i32>(width),
        .bottom =
            static_cast<i32>(height)
    });
}
} // namespace

f32 HenyeyGreensteinPhase(
    const f32 cosineTheta,
    const f32 anisotropy) noexcept
{
    const f32 g =
        std::clamp(
            anisotropy,
            -0.95F,
            0.95F);
    const f32 g2 =
        g * g;
    const f32 denominator =
        std::max(
            1.0F +
                g2 -
                2.0F *
                    g *
                    std::clamp(
                        cosineTheta,
                        -1.0F,
                        1.0F),
            1.0e-4F);

    return
        (1.0F - g2) /
        (12.566370614359172F *
         std::pow(
             denominator,
             1.5F));
}

HomogeneousVolumeReference
IntegrateHomogeneousVolume(
    const f32 density,
    const f32 distanceMeters,
    const f32 extinctionScale,
    const f32 singleScatteringAlbedo,
    const math::Float3 scatteringColor,
    const math::Float3 incidentRadiance,
    const math::Float3 emissionColor,
    const f32 emissionScalar,
    const f32 emissionScale) noexcept
{
    const f32 length =
        std::max(
            distanceMeters,
            0.0F);
    const f32 sigmaT =
        std::max(
            density,
            0.0F) *
        std::max(
            extinctionScale,
            0.0F);

    if (length <= 0.0F)
    {
        return {};
    }

    const f32 transmittance =
        sigmaT > 1.0e-8F
            ? std::exp(
                  -sigmaT *
                  length)
            : 1.0F;
    const f32 integral =
        sigmaT > 1.0e-8F
            ? (1.0F -
               transmittance) /
                  sigmaT
            : length;
    const f32 albedo =
        std::clamp(
            singleScatteringAlbedo,
            0.0F,
            1.0F);

    const math::Float3 sigmaS{
        sigmaT *
            albedo *
            std::max(
                scatteringColor.x,
                0.0F),
        sigmaT *
            albedo *
            std::max(
                scatteringColor.y,
                0.0F),
        sigmaT *
            albedo *
            std::max(
                scatteringColor.z,
                0.0F)
    };

    HomogeneousVolumeReference result{
        .transmittance =
            transmittance,
        .scatteredRadiance = {
            sigmaS.x *
                std::max(
                    incidentRadiance.x,
                    0.0F) *
                integral,
            sigmaS.y *
                std::max(
                    incidentRadiance.y,
                    0.0F) *
                integral,
            sigmaS.z *
                std::max(
                    incidentRadiance.z,
                    0.0F) *
                integral
        },
        .emittedRadiance = {
            std::max(
                emissionColor.x,
                0.0F) *
                std::max(
                    emissionScalar,
                    0.0F) *
                std::max(
                    emissionScale,
                    0.0F) *
                integral,
            std::max(
                emissionColor.y,
                0.0F) *
                std::max(
                    emissionScalar,
                    0.0F) *
                std::max(
                    emissionScale,
                    0.0F) *
                integral,
            std::max(
                emissionColor.z,
                0.0F) *
                std::max(
                    emissionScalar,
                    0.0F) *
                std::max(
                    emissionScale,
                    0.0F) *
                integral
        }
    };

    result.totalRadiance = {
        result.scatteredRadiance.x +
            result.emittedRadiance.x,
        result.scatteredRadiance.y +
            result.emittedRadiance.y,
        result.scatteredRadiance.z +
            result.emittedRadiance.z
    };

    return result;
}

std::optional<
    lighting::EmissiveVolumeSource>
BuildEmissiveVolumeSource(
    const world_model::ResolvedVolumeDomain& domain,
    const f32 emissionAuthorityScalar) noexcept
{
    constexpr u64 kEmissionBit =
        static_cast<u64>(
            world_model::
                VolumeField::Emission);

    if (!domain.enabled ||
        !domain.renderEnabled ||
        (domain.fieldMask &
         kEmissionBit) == 0U ||
        emissionAuthorityScalar <= 0.0F ||
        domain.emissionScale <= 0.0F ||
        domain.giEmissionScale <= 0.0F)
    {
        return std::nullopt;
    }

    const f64 radius =
        std::sqrt(
            domain.halfExtentsMeters.x *
                domain.halfExtentsMeters.x +
            domain.halfExtentsMeters.y *
                domain.halfExtentsMeters.y +
            domain.halfExtentsMeters.z *
                domain.halfExtentsMeters.z);

    const f32 boundedRadius =
        static_cast<f32>(
            std::max(
                radius,
                0.05));

    return lighting::EmissiveVolumeSource{
        .centerInFrameMeters =
            domain.centerMeters,
        .radiusMeters =
            boundedRadius,
        .emissionLinear =
            domain.emissionColor,
        .intensityScale =
            emissionAuthorityScalar *
            domain.emissionScale *
            domain.giEmissionScale,
        .influenceRangeMeters =
            std::max(
                boundedRadius * 12.0F,
                1.0F),
        .stableId =
            domain.object.high ^
            domain.object.low
    };
}

class UniversalVolumeRenderer::Impl
{
public:
    struct Presentation
    {
        scene::ObjectId volume{};
        std::string viewport;
        u32 width{0U};
        u32 height{0U};
        // The raymarch (current/history) can run below the output size.
        u32 marchWidth{0U};
        u32 marchHeight{0U};
        u32 frameCounter{0U};

        std::unique_ptr<rhi::Texture>
            current;
        std::unique_ptr<rhi::Texture>
            history;
        std::unique_ptr<rhi::Texture>
            composite;

        std::unique_ptr<rhi::Buffer>
            slotMap;
        std::unique_ptr<rhi::Buffer>
            localLights;
        std::unique_ptr<rhi::Buffer>
            params;
        std::unique_ptr<rhi::Buffer>
            dummyRadianceCells;
        std::unique_ptr<rhi::Buffer>
            dummyRadianceLevels;
        std::unique_ptr<rhi::Buffer>
            dummyParticleLightGrid;

        u32 slotCapacity{0U};
        u32 lightCapacity{0U};
        bool historyValid{false};
    };

    struct Entry
    {
        scene::ObjectId volume{};
        VolumeRenderRuntimeSettings settings{};
        VolumeRenderDiagnostics diagnostics{};
    };

    Impl(
        rhi::Device& device,
        const shader::Compiler& compiler)
        : device(&device)
    {
        std::string raymarchSource = kRaymarchPs;
        {
            const std::string marker = "//@CLOUD_GENERA@";
            const auto at = raymarchSource.find(marker);
            if (at != std::string::npos)
            {
                raymarchSource.replace(at, marker.size(), cloud_genera::CloudGeneraHlsl());
            }
        }
        raymarchPipeline =
            CompileFullscreen(
                device,
                compiler,
                raymarchSource.c_str(),
                0U,
                8U,
                4U);
        compositePipeline =
            CompileFullscreen(
                device,
                compiler,
                kCompositePs,
                1U,
                0U,
                2U);
        copyPipeline =
            CompileFullscreen(
                device,
                compiler,
                kCopyPs,
                0U,
                0U,
                1U);
    }

    [[nodiscard]] Entry&
    EnsureEntry(
        const scene::ObjectId volume)
    {
        auto found =
            std::find_if(
                entries.begin(),
                entries.end(),
                [volume](const Entry& entry)
                {
                    return entry.volume ==
                        volume;
                });

        if (found == entries.end())
        {
            entries.push_back({
                .volume = volume
            });
            return entries.back();
        }

        return *found;
    }

    [[nodiscard]] Presentation&
    EnsurePresentation(
        const scene::ObjectId volume,
        const std::string_view viewport,
        const u32 width,
        const u32 height,
        const u32 marchWidth,
        const u32 marchHeight)
    {
        auto found =
            std::find_if(
                presentations.begin(),
                presentations.end(),
                [volume, viewport](
                    const Presentation& item)
                {
                    return
                        item.volume ==
                            volume &&
                        item.viewport ==
                            viewport;
                });

        if (found ==
            presentations.end())
        {
            presentations.push_back({
                .volume = volume,
                .viewport =
                    std::string(viewport)
            });
            found =
                std::prev(
                    presentations.end());
        }

        if (found->width != width ||
            found->height != height ||
            found->marchWidth != marchWidth ||
            found->marchHeight != marchHeight ||
            found->current == nullptr ||
            found->history == nullptr ||
            found->composite == nullptr)
        {
            found->width = width;
            found->height = height;
            found->marchWidth = marchWidth;
            found->marchHeight = marchHeight;
            found->current =
                device->CreateTexture({
                    .width = marchWidth,
                    .height = marchHeight,
                    .format =
                        rhi::TextureFormat::
                            RGBA16_Float,
                    .initialState =
                        rhi::ResourceState::
                            ShaderResource
                });
            found->history =
                device->CreateTexture({
                    .width = marchWidth,
                    .height = marchHeight,
                    .format =
                        rhi::TextureFormat::
                            RGBA16_Float,
                    .initialState =
                        rhi::ResourceState::
                            ShaderResource
                });
            found->composite =
                device->CreateTexture({
                    .width = width,
                    .height = height,
                    .format =
                        rhi::TextureFormat::
                            RGBA16_Float,
                    .initialState =
                        rhi::ResourceState::
                            ShaderResource
                });
            found->historyValid =
                false;
        }

        return *found;
    }

    rhi::Device* device{nullptr};
    std::unique_ptr<rhi::GraphicsPipeline>
        raymarchPipeline;
    std::unique_ptr<rhi::GraphicsPipeline>
        compositePipeline;
    std::unique_ptr<rhi::GraphicsPipeline>
        copyPipeline;
    std::vector<Entry> entries;
    std::vector<Presentation>
        presentations;
};

UniversalVolumeRenderer::UniversalVolumeRenderer(
    rhi::Device& device,
    const shader::Compiler& compiler)
    : impl_(
          std::make_unique<Impl>(
              device,
              compiler))
{
}

UniversalVolumeRenderer::~UniversalVolumeRenderer() = default;

VolumeRenderRuntimeSettings&
UniversalVolumeRenderer::Settings(
    const scene::ObjectId volume)
{
    return
        impl_->EnsureEntry(
            volume).
            settings;
}

VolumeRenderDiagnostics
UniversalVolumeRenderer::LiveDiagnostics(
    const scene::ObjectId volume) const noexcept
{
    const auto found =
        std::find_if(
            impl_->entries.begin(),
            impl_->entries.end(),
            [volume](const Impl::Entry& entry)
            {
                return entry.volume ==
                    volume;
            });

    return
        found != impl_->entries.end()
            ? found->diagnostics
            : VolumeRenderDiagnostics{};
}

void UniversalVolumeRenderer::RemoveMissing(
    const scene::ObjectStore& objects)
{
    std::erase_if(
        impl_->entries,
        [&objects](const Impl::Entry& entry)
        {
            const auto record =
                objects.Find(
                    entry.volume);
            return
                !record.has_value() ||
                record->type !=
                    world_model::
                        kVolumeType;
        });

    std::erase_if(
        impl_->presentations,
        [&objects](const Impl::Presentation& item)
        {
            const auto record =
                objects.Find(
                    item.volume);
            return
                !record.has_value() ||
                record->type !=
                    world_model::
                        kVolumeType;
        });
}

void UniversalVolumeRenderer::AddLivePasses(
    render_graph::RenderGraph& graph,
    const std::string_view prefix,
    const std::string_view viewportId,
    const render_graph::TextureHandle sceneColor,
    const render_graph::TextureHandle depth,
    const u32 width,
    const u32 height,
    const render_view::CameraState& camera,
    const lighting::LightingView& lightingView,
    const world_model::ResolvedVolumeDomain& domain,
    volume_fields::VolumeFieldStorage& storage,
    const volume_fields::ImportedVolumeFields& fields,
    const lighting::DirectionalLight& stellar,
    const std::span<const lighting::ResolvedLocalLight> localLights,
    const render_graph::BufferHandle radianceCells,
    const render_graph::BufferHandle radianceLevels,
    const u32 radianceLevelCount,
    const render_graph::BufferHandle particleLightGrid,
    const bool resetHistory,
    const math::Float3 stellarColorTop,
    const VolumeAtmosphereLuts& atmosphereLuts)
{
    auto& entry =
        impl_->EnsureEntry(
            domain.object);

    entry.diagnostics = {};

    const auto density =
        FindField(
            fields,
            world_model::
                VolumeField::Density);

    if (!domain.enabled ||
        !domain.renderEnabled ||
        !density.IsValid() ||
        width == 0U ||
        height == 0U)
    {
        return;
    }

    // Fine fibres and cloudlets (ice, altocumulus) are narrower than two
    // pixels at half resolution: march those layers at full resolution.
    const bool fineLayer = domain.cloudType >= 1U && domain.cloudType <= 4U;
    const bool halfResolution = storage.SmoothSampling() && !fineLayer;
    auto& presentation =
        impl_->EnsurePresentation(
            domain.object,
            viewportId,
            width,
            height,
            halfResolution ? std::max(width / 2U, 1U) : width,
            halfResolution ? std::max(height / 2U, 1U) : height);

    if (resetHistory)
    {
        presentation.historyValid =
            false;
    }

    const auto& fieldDiagnostics =
        storage.Diagnostics();
    const auto tiles =
        storage.Tiles();

    const u32 mapCount =
        std::max(
            fieldDiagnostics.tilesX *
                fieldDiagnostics.tilesY *
                fieldDiagnostics.tilesZ,
            1U);

    if (presentation.slotCapacity <
            mapCount ||
        presentation.slotMap == nullptr)
    {
        presentation.slotCapacity =
            mapCount;
        presentation.slotMap =
            impl_->device->
                CreateBuffer({
                    .sizeBytes =
                        static_cast<u64>(
                            mapCount) *
                        sizeof(u32),
                    .usage =
                        rhi::BufferUsage::
                            Structured,
                    .memory =
                        rhi::MemoryUsage::
                            HostVisible,
                    .initialState =
                        rhi::ResourceState::
                            ShaderResource
                });
    }

    i32 minimumX =
        std::numeric_limits<i32>::max();
    i32 minimumY =
        std::numeric_limits<i32>::max();
    i32 minimumZ =
        std::numeric_limits<i32>::max();

    for (const auto& tile : tiles)
    {
        if (!tile.resident)
        {
            continue;
        }

        minimumX =
            std::min(
                minimumX,
                tile.coord.x);
        minimumY =
            std::min(
                minimumY,
                tile.coord.y);
        minimumZ =
            std::min(
                minimumZ,
                tile.coord.z);
    }

    if (minimumX ==
        std::numeric_limits<i32>::max())
    {
        return;
    }

    auto* slotMap =
        reinterpret_cast<u32*>(
            presentation.slotMap->
                Map());

    std::fill_n(
        slotMap,
        presentation.slotCapacity,
        kInvalidSlot);

    for (const auto& tile : tiles)
    {
        if (!tile.resident)
        {
            continue;
        }

        const i32 lx =
            tile.coord.x -
            minimumX;
        const i32 ly =
            tile.coord.y -
            minimumY;
        const i32 lz =
            tile.coord.z -
            minimumZ;

        if (lx < 0 ||
            ly < 0 ||
            lz < 0 ||
            lx >=
                static_cast<i32>(
                    fieldDiagnostics.tilesX) ||
            ly >=
                static_cast<i32>(
                    fieldDiagnostics.tilesY) ||
            lz >=
                static_cast<i32>(
                    fieldDiagnostics.tilesZ))
        {
            continue;
        }

        const u32 index =
            (static_cast<u32>(lz) *
                 fieldDiagnostics.tilesY +
             static_cast<u32>(ly)) *
                fieldDiagnostics.tilesX +
            static_cast<u32>(lx);

        slotMap[index] =
            tile.slot;
    }

    presentation.slotMap->Unmap();

    const math::Double3 domainDelta =
        domain.centerMeters -
        lightingView.
            cameraPositionInFrameMeters;

    const math::Float3 domainCenterRelative{
        static_cast<f32>(
            domainDelta.x),
        static_cast<f32>(
            domainDelta.y),
        static_cast<f32>(
            domainDelta.z)
    };

    struct RankedLight
    {
        f32 distance{0.0F};
        lighting::ResolvedLocalLight light{};
    };

    std::vector<RankedLight>
        rankedLights;

    const f32 domainRadius =
        static_cast<f32>(
            std::sqrt(
                domain.halfExtentsMeters.x *
                    domain.halfExtentsMeters.x +
                domain.halfExtentsMeters.y *
                    domain.halfExtentsMeters.y +
                domain.halfExtentsMeters.z *
                    domain.halfExtentsMeters.z));

    for (const auto& light :
         localLights)
    {
        const math::Float3 delta{
            light.positionCameraRelativeMeters.x -
                domainCenterRelative.x,
            light.positionCameraRelativeMeters.y -
                domainCenterRelative.y,
            light.positionCameraRelativeMeters.z -
                domainCenterRelative.z
        };

        const f32 distance =
            math::Length(delta);

        if (distance >
            light.rangeMeters +
                domainRadius)
        {
            continue;
        }

        rankedLights.push_back({
            .distance = distance,
            .light = light
        });
    }

    std::stable_sort(
        rankedLights.begin(),
        rankedLights.end(),
        [](const RankedLight& a,
           const RankedLight& b)
        {
            if (a.distance != b.distance)
            {
                return
                    a.distance <
                    b.distance;
            }

            return
                a.light.stableId <
                b.light.stableId;
        });

    if (rankedLights.size() >
        kMaximumVolumeLocalLights)
    {
        rankedLights.resize(
            kMaximumVolumeLocalLights);
    }

    const u32 lightCount =
        static_cast<u32>(
            rankedLights.size());
    const u32 lightCapacity =
        std::max(
            lightCount,
            1U);

    if (presentation.lightCapacity <
            lightCapacity ||
        presentation.localLights ==
            nullptr)
    {
        presentation.lightCapacity =
            std::max(
                lightCapacity,
                std::max(
                    presentation.
                        lightCapacity *
                        2U,
                    4U));

        presentation.localLights =
            impl_->device->
                CreateBuffer({
                    .sizeBytes =
                        static_cast<u64>(
                            presentation.
                                lightCapacity) *
                        sizeof(
                            lighting::
                                GpuLocalLight),
                    .usage =
                        rhi::BufferUsage::
                            Structured,
                    .memory =
                        rhi::MemoryUsage::
                            HostVisible,
                    .initialState =
                        rhi::ResourceState::
                            ShaderResource
                });
    }

    auto* gpuLights =
        reinterpret_cast<
            lighting::GpuLocalLight*>(
                presentation.localLights->
                    Map());

    std::fill_n(
        gpuLights,
        presentation.lightCapacity,
        lighting::GpuLocalLight{});

    for (u32 index = 0U;
         index < lightCount;
         ++index)
    {
        gpuLights[index] =
            lighting::EncodeGpuLocalLight(
                rankedLights[index].light);
    }

    presentation.localLights->Unmap();

    if (presentation.params == nullptr)
    {
        presentation.params =
            impl_->device->
                CreateBuffer({
                    .sizeBytes =
                        sizeof(
                            GpuVolumeParams),
                    .usage =
                        rhi::BufferUsage::
                            Structured,
                    .memory =
                        rhi::MemoryUsage::
                            HostVisible,
                    .initialState =
                        rhi::ResourceState::
                            ShaderResource
                });
    }

    if (presentation.dummyRadianceCells == nullptr)
    {
        presentation.dummyRadianceCells =
            impl_->device->CreateBuffer({
                .sizeBytes =
                    sizeof(
                        lighting::GpuRadianceCell),
                .usage =
                    rhi::BufferUsage::
                        Structured,
                .memory =
                    rhi::MemoryUsage::
                        HostVisible,
                .initialState =
                    rhi::ResourceState::
                        ShaderResource
            });
        std::memset(
            presentation.
                dummyRadianceCells->Map(),
            0,
            static_cast<std::size_t>(
                presentation.
                    dummyRadianceCells->
                        SizeBytes()));
        presentation.
            dummyRadianceCells->Unmap();
    }

    if (presentation.dummyRadianceLevels == nullptr)
    {
        presentation.dummyRadianceLevels =
            impl_->device->CreateBuffer({
                .sizeBytes =
                    sizeof(
                        lighting::
                            GpuRadianceLevelInfo),
                .usage =
                    rhi::BufferUsage::
                        Structured,
                .memory =
                    rhi::MemoryUsage::
                        HostVisible,
                .initialState =
                    rhi::ResourceState::
                        ShaderResource
            });
        std::memset(
            presentation.
                dummyRadianceLevels->Map(),
            0,
            static_cast<std::size_t>(
                presentation.
                    dummyRadianceLevels->
                        SizeBytes()));
        presentation.
            dummyRadianceLevels->Unmap();
    }

    if (presentation.dummyParticleLightGrid == nullptr)
    {
        presentation.dummyParticleLightGrid = impl_->device->CreateBuffer({
            .sizeBytes = 2U * sizeof(std::array<u32,4U>),
            .usage = rhi::BufferUsage::Structured,
            .memory = rhi::MemoryUsage::HostVisible,
            .initialState = rhi::ResourceState::ShaderResource
        });
        std::memset(presentation.dummyParticleLightGrid->Map(),0,static_cast<std::size_t>(presentation.dummyParticleLightGrid->SizeBytes()));
        presentation.dummyParticleLightGrid->Unmap();
    }

    const f32 aspect =
        static_cast<f32>(width) /
        static_cast<f32>(height);
    const f32 tanHalfFov =
        std::tan(
            camera.verticalFovRadians *
            0.5F);

    const auto& runtimeSettings =
        entry.settings;

    GpuVolumeParams params{
        .cameraForwardAspect = {
            camera.forward.x,
            camera.forward.y,
            camera.forward.z,
            aspect
        },
        .cameraUpTanHalfFov = {
            camera.up.x,
            camera.up.y,
            camera.up.z,
            tanHalfFov
        },
        .cameraPositionTemporal = {
            static_cast<f32>(
                camera.
                    localPositionMeters.x),
            static_cast<f32>(
                camera.
                    localPositionMeters.y),
            static_cast<f32>(
                camera.
                    localPositionMeters.z),
            runtimeSettings.temporalEnabled
                ? domain.temporalWeight
                : 0.0F
        },
        .domainCenterExtinction = {
            static_cast<f32>(
                domain.centerMeters.x),
            static_cast<f32>(
                domain.centerMeters.y),
            static_cast<f32>(
                domain.centerMeters.z),
            domain.extinctionScale
        },
        .domainHalfAlbedo = {
            static_cast<f32>(
                std::abs(
                    domain.halfExtentsMeters.x)),
            static_cast<f32>(
                std::abs(
                    domain.halfExtentsMeters.y)),
            static_cast<f32>(
                std::abs(
                    domain.halfExtentsMeters.z)),
            domain.singleScatteringAlbedo
        },
        .scatteringColorAnisotropy = {
            domain.scatteringColor.x,
            domain.scatteringColor.y,
            domain.scatteringColor.z,
            domain.anisotropy
        },
        .emissionColorScale = {
            domain.emissionColor.x,
            domain.emissionColor.y,
            domain.emissionColor.z,
            domain.emissionScale
        },
        .stellarDirectionScale = {
            stellar.directionToLight.x,
            stellar.directionToLight.y,
            stellar.directionToLight.z,
            stellar.irradianceScale
        },
        .stellarColorAmbient = {
            stellar.colorLinear.x,
            stellar.colorLinear.y,
            stellar.colorLinear.z,
            0.035F
        },
        .depthRangeHistory = {
            camera.nearPlaneMeters,
            camera.farPlaneMeters,
            presentation.historyValid
                ? 1.0F
                : 0.0F,
            static_cast<f32>(
                runtimeSettings.debugMode)
        },
        .minimumTile = {
            minimumX,
            minimumY,
            minimumZ,
            0
        },
        .tileLayout = {
            fieldDiagnostics.tilesX,
            fieldDiagnostics.tilesY,
            fieldDiagnostics.tilesZ,
            lightCount
        },
        .logicalResolution = {
            fieldDiagnostics.resolutionX,
            fieldDiagnostics.resolutionY,
            fieldDiagnostics.resolutionZ,
            radianceCells.IsValid() &&
                    radianceLevels.IsValid()
                ? radianceLevelCount
                : 0U
        },
        .renderParams = {
            domain.renderSteps,
            domain.shadowSteps,
            fieldDiagnostics.tileEdge,
            FindField(
                fields,
                world_model::
                    VolumeField::Emission).
                    IsValid()
                ? 1U
                : 0U
        },
        .gridAnchor = {
            static_cast<f32>(storage.GridAnchor().x),
            static_cast<f32>(storage.GridAnchor().y),
            static_cast<f32>(storage.GridAnchor().z),
            storage.SmoothSampling() ? 1.0F : 0.0F
        },
        .sunColorTop = {stellarColorTop.x, stellarColorTop.y, stellarColorTop.z, 0.0F},
        .atmosphere = {
            atmosphereLuts.bottomRadiusMeters,
            atmosphereLuts.topRadiusMeters,
            atmosphereLuts.transmittance != nullptr &&
                    atmosphereLuts.multiScattering != nullptr
                ? 1.0F
                : 0.0F,
            0.0F},
        .occupancyMinimum = {
            static_cast<f32>(storage.OccupiedMinimum().x),
            static_cast<f32>(storage.OccupiedMinimum().y),
            static_cast<f32>(storage.OccupiedMinimum().z),
            storage.HasOccupiedBounds() ? 1.0F : 0.0F},
        .occupancyMaximum = {
            static_cast<f32>(storage.OccupiedMaximum().x),
            static_cast<f32>(storage.OccupiedMaximum().y),
            static_cast<f32>(storage.OccupiedMaximum().z),
            // Frame counter: the first-sample jitter changes every frame, so
            // the temporal blend averages it out instead of freezing a grain.
            static_cast<f32>(presentation.frameCounter++ % 4096U)},
        // Type 10 (cumulonimbus) is the simulated storm: typed by height.
        .typeParams = {domain.cloudType >= 10U ? 0.0F : static_cast<f32>(domain.cloudType), static_cast<f32>(CloudLookVariant()), 0.0F, 0.0F}
    };

    auto* mappedParams =
        presentation.params->Map();

    std::memcpy(
        mappedParams,
        &params,
        sizeof(params));

    presentation.params->Unmap();

    const auto currentHandle =
        graph.ImportTexture(
            std::string(prefix) +
                ".VolumeCurrent",
            *presentation.current,
            rhi::ResourceState::
                ShaderResource);
    const auto historyHandle =
        graph.ImportTexture(
            std::string(prefix) +
                ".VolumeHistory",
            *presentation.history,
            rhi::ResourceState::
                ShaderResource);
    const auto compositeHandle =
        graph.ImportTexture(
            std::string(prefix) +
                ".VolumeComposite",
            *presentation.composite,
            rhi::ResourceState::
                ShaderResource);

    const auto slotMapHandle =
        graph.ImportBuffer(
            std::string(prefix) +
                ".VolumeSlotMap",
            *presentation.slotMap,
            rhi::ResourceState::
                ShaderResource);
    const auto localLightsHandle =
        graph.ImportBuffer(
            std::string(prefix) +
                ".VolumeLocalLights",
            *presentation.localLights,
            rhi::ResourceState::
                ShaderResource);
    const auto paramsHandle =
        graph.ImportBuffer(
            std::string(prefix) +
                ".VolumeParams",
            *presentation.params,
            rhi::ResourceState::
                ShaderResource);

    const auto dummyRadianceCells =
        graph.ImportBuffer(
            std::string(prefix) +
                ".DummyRadianceCells",
            *presentation.
                dummyRadianceCells,
            rhi::ResourceState::
                ShaderResource);
    const auto dummyRadianceLevels =
        graph.ImportBuffer(
            std::string(prefix) +
                ".DummyRadianceLevels",
            *presentation.
                dummyRadianceLevels,
            rhi::ResourceState::
                ShaderResource);

    const auto effectiveRadianceCells =
        radianceCells.IsValid()
            ? radianceCells
            : dummyRadianceCells;
    const auto effectiveRadianceLevels =
        radianceLevels.IsValid()
            ? radianceLevels
            : dummyRadianceLevels;

    const auto dummyParticleLightGrid = graph.ImportBuffer(
        std::string(prefix) + ".DummyParticleLightGrid",
        *presentation.dummyParticleLightGrid,
        rhi::ResourceState::ShaderResource);
    const auto effectiveParticleLightGrid =
        particleLightGrid.IsValid()
            ? particleLightGrid
            : dummyParticleLightGrid;

    auto emission =
        FindField(
            fields,
            world_model::
                VolumeField::Emission);

    if (!emission.IsValid())
    {
        emission =
            density;
    }

    // A pass may declare each buffer once. A volume without an Emission field
    // (a baked Density-only cache, the Empty preset) binds the density buffer
    // as emission too, so repeated handles are declared a single time.
    std::vector<render_graph::BufferUse> raymarchBufferUses;
    for (const auto handle : {density, emission, slotMapHandle, localLightsHandle,
                              paramsHandle, effectiveRadianceCells,
                              effectiveRadianceLevels, effectiveParticleLightGrid})
    {
        const bool declared = std::any_of(
            raymarchBufferUses.begin(), raymarchBufferUses.end(),
            [handle](const render_graph::BufferUse& use)
            {
                return use.buffer.index == handle.index;
            });
        if (!declared)
        {
            raymarchBufferUses.push_back(
                {handle, rhi::ResourceState::ShaderResource,
                 render_graph::Access::Read});
        }
    }

    graph.AddPass(
        std::string(prefix) +
            ".VolumeRaymarch",
        {
            {
                .texture = depth,
                .state =
                    rhi::ResourceState::
                        DepthRead,
                .access =
                    render_graph::Access::
                        Read
            },
            {
                .texture =
                    historyHandle,
                .state =
                    rhi::ResourceState::
                        ShaderResource,
                .access =
                    render_graph::Access::
                        Read
            },
            {
                .texture =
                    currentHandle,
                .state =
                    rhi::ResourceState::
                        RenderTarget,
                .access =
                    render_graph::Access::
                        Write
            }
        },
        raymarchBufferUses,
        [this,
         currentHandle,
         depth,
         historyHandle,
         density,
         emission,
         slotMapHandle,
         localLightsHandle,
         paramsHandle,
         effectiveRadianceCells,
         effectiveRadianceLevels,
         effectiveParticleLightGrid,
         transmittanceLut = atmosphereLuts.transmittance,
         multiLut = atmosphereLuts.multiScattering,
         width = presentation.marchWidth,
         height = presentation.marchHeight](
            rhi::CommandList& commands,
            const render_graph::Resources& resources)
        {
            ConfigureFullscreen(
                commands,
                resources.Texture(
                    currentHandle),
                width,
                height);
            commands.SetGraphicsPipeline(
                *impl_->raymarchPipeline);
            commands.SetGraphicsBuffer(
                0U,
                resources.Buffer(density));
            commands.SetGraphicsBuffer(
                1U,
                resources.Buffer(emission));
            commands.SetGraphicsBuffer(
                2U,
                resources.Buffer(
                    slotMapHandle));
            commands.SetGraphicsBuffer(
                3U,
                resources.Buffer(
                    localLightsHandle));
            commands.SetGraphicsBuffer(
                4U,
                resources.Buffer(
                    paramsHandle));
            commands.SetGraphicsBuffer(
                5U,
                resources.Buffer(
                    effectiveRadianceCells));
            commands.SetGraphicsBuffer(
                6U,
                resources.Buffer(
                    effectiveRadianceLevels));
            commands.SetGraphicsBuffer(
                7U,
                resources.Buffer(
                    effectiveParticleLightGrid));
            commands.SetGraphicsTexture(
                0U,
                resources.Texture(depth));
            commands.SetGraphicsTexture(
                1U,
                resources.Texture(
                    historyHandle));
            // Without an atmosphere the LUT slots still need a texture;
            // the shader then ignores them.
            commands.SetGraphicsTexture(
                2U,
                transmittanceLut != nullptr
                    ? *transmittanceLut
                    : resources.Texture(historyHandle));
            commands.SetGraphicsTexture(
                3U,
                multiLut != nullptr
                    ? *multiLut
                    : resources.Texture(historyHandle));
            commands.Draw(6U);
        });

    graph.AddPass(
        std::string(prefix) +
            ".VolumeComposite",
        {
            {
                .texture = sceneColor,
                .state =
                    rhi::ResourceState::
                        ShaderResource,
                .access =
                    render_graph::Access::
                        Read
            },
            {
                .texture =
                    currentHandle,
                .state =
                    rhi::ResourceState::
                        ShaderResource,
                .access =
                    render_graph::Access::
                        Read
            },
            {
                .texture =
                    compositeHandle,
                .state =
                    rhi::ResourceState::
                        RenderTarget,
                .access =
                    render_graph::Access::
                        Write
            }
        },
        [this,
         sceneColor,
         currentHandle,
         compositeHandle,
         width,
         height,
         debugMode =
             runtimeSettings.debugMode](
            rhi::CommandList& commands,
            const render_graph::Resources& resources)
        {
            ConfigureFullscreen(
                commands,
                resources.Texture(
                    compositeHandle),
                width,
                height);
            commands.SetGraphicsPipeline(
                *impl_->compositePipeline);
            const std::array<u32,1>
                constants{
                    static_cast<u32>(
                        debugMode)
                };
            commands.SetGraphicsConstants(
                constants);
            commands.SetGraphicsTexture(
                0U,
                resources.Texture(
                    sceneColor));
            commands.SetGraphicsTexture(
                1U,
                resources.Texture(
                    currentHandle));
            commands.Draw(6U);
        });

    graph.AddPass(
        std::string(prefix) +
            ".VolumeCopyBack",
        {
            {
                .texture =
                    compositeHandle,
                .state =
                    rhi::ResourceState::
                        ShaderResource,
                .access =
                    render_graph::Access::
                        Read
            },
            {
                .texture = sceneColor,
                .state =
                    rhi::ResourceState::
                        RenderTarget,
                .access =
                    render_graph::Access::
                        Write
            }
        },
        [this,
         sceneColor,
         compositeHandle,
         width,
         height](
            rhi::CommandList& commands,
            const render_graph::Resources& resources)
        {
            ConfigureFullscreen(
                commands,
                resources.Texture(
                    sceneColor),
                width,
                height);
            commands.SetGraphicsPipeline(
                *impl_->copyPipeline);
            commands.SetGraphicsTexture(
                0U,
                resources.Texture(
                    compositeHandle));
            commands.Draw(6U);
        });

    graph.AddPass(
        std::string(prefix) +
            ".VolumeHistory",
        {
            {
                .texture =
                    currentHandle,
                .state =
                    rhi::ResourceState::
                        ShaderResource,
                .access =
                    render_graph::Access::
                        Read
            },
            {
                .texture =
                    historyHandle,
                .state =
                    rhi::ResourceState::
                        RenderTarget,
                .access =
                    render_graph::Access::
                        Write
            }
        },
        [this,
         currentHandle,
         historyHandle,
         width = presentation.marchWidth,
         height = presentation.marchHeight](
            rhi::CommandList& commands,
            const render_graph::Resources& resources)
        {
            ConfigureFullscreen(
                commands,
                resources.Texture(
                    historyHandle),
                width,
                height);
            commands.SetGraphicsPipeline(
                *impl_->copyPipeline);
            commands.SetGraphicsTexture(
                0U,
                resources.Texture(
                    currentHandle));
            commands.Draw(6U);
        });

    graph.AddPass(
        std::string(prefix) +
            ".VolumeRestoreDepth",
        {
            {
                .texture = depth,
                .state =
                    rhi::ResourceState::
                        DepthWrite,
                .access =
                    render_graph::Access::
                        Write
            }
        },
        [](
            rhi::CommandList&,
            const render_graph::Resources&)
        {
        });

    presentation.historyValid =
        true;

    entry.diagnostics = {
        .rendered = true,
        .historyValid =
            presentation.historyValid,
        .raymarchSteps =
            domain.renderSteps,
        .shadowSteps =
            domain.shadowSteps,
        .localLightCount =
            lightCount,
        .residentTiles =
            fieldDiagnostics.
                residentTiles,
        .historyBytes =
            static_cast<u64>(
                width) *
            height *
            8U,
        .scratchBytes =
            static_cast<u64>(
                width) *
            height *
            8U *
            2U
    };
}

std::string_view
VolumeRenderDebugModeName(
    const VolumeRenderDebugMode mode) noexcept
{
    switch (mode)
    {
    case VolumeRenderDebugMode::Composite:
        return "Composite";
    case VolumeRenderDebugMode::Scattering:
        return "Scattering";
    case VolumeRenderDebugMode::Extinction:
        return "Extinction";
    case VolumeRenderDebugMode::Emission:
        return "Emission";
    case VolumeRenderDebugMode::Shadow:
        return "Shadow";
    }

    return "Unknown";
}
} // namespace orbit::volume_render

