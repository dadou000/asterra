#include <orbit/studio_ui/StudioFlatMap.hpp>

#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/rhi/Pipeline.hpp>
#include <orbit/rhi/Resource.hpp>
#include <orbit/shader/ShaderCompiler.hpp>
#include <orbit/terrain/AnalyticTerrainSource.hpp>
#include <orbit/terrain/GlobalTerrainFields.hpp>
#include <orbit/terrain/TerrainSource.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <numbers>
#include <span>
#include <stdexcept>
#include <vector>

namespace orbit::studio_ui
{
namespace
{
constexpr f64 kRadiansToDegrees = 180.0 / std::numbers::pi;
constexpr f64 kDegreesToRadians = std::numbers::pi / 180.0;

struct Rgb
{
    f32 r{0.0F};
    f32 g{0.0F};
    f32 b{0.0F};
};

[[nodiscard]] f32 Lerp(const f32 a, const f32 b, const f32 t) noexcept
{
    return a + (b - a) * std::clamp(t, 0.0F, 1.0F);
}

[[nodiscard]] Rgb LerpRgb(const Rgb& a, const Rgb& b, const f32 t) noexcept
{
    return {Lerp(a.r, b.r, t), Lerp(a.g, b.g, t), Lerp(a.b, b.b, t)};
}

[[nodiscard]] u32 PackRgba(const Rgb& color) noexcept
{
    const auto channel = [](const f32 value) -> u32
    {
        return static_cast<u32>(std::clamp(value, 0.0F, 1.0F) * 255.0F + 0.5F);
    };
    return channel(color.r) | (channel(color.g) << 8) |
        (channel(color.b) << 16) | (0xFFU << 24);
}

// The layer palettes below are the same ones the sandbox PlanetMapRenderer
// uses (engine/map_render), and the biome colours mirror the flat biome
// colours in engine/terrain_render/src/TerrainSurfaceShader.hpp; none of them
// live in a shared table, so they are kept in sync by hand.
[[nodiscard]] u32 ElevationColor(const terrain::TerrainSample& sample) noexcept
{
    if (sample.standingWaterDepthMeters > 0.0)
    {
        constexpr Rgb kShallow{0.42F, 0.66F, 0.72F};
        constexpr Rgb kDeep{0.02F, 0.06F, 0.22F};
        const f32 t = static_cast<f32>(
            std::clamp(sample.standingWaterDepthMeters / 5'000.0, 0.0, 1.0));
        return PackRgba(LerpRgb(kShallow, kDeep, t));
    }

    constexpr Rgb kLowland{0.20F, 0.45F, 0.18F};
    constexpr Rgb kMidland{0.62F, 0.56F, 0.28F};
    constexpr Rgb kHighland{0.45F, 0.32F, 0.22F};
    constexpr Rgb kPeak{0.96F, 0.96F, 0.98F};

    const f32 elevation = static_cast<f32>(sample.elevationMeters);
    if (elevation < 1'500.0F)
    {
        return PackRgba(
            LerpRgb(kLowland, kMidland, std::max(elevation, 0.0F) / 1'500.0F));
    }
    if (elevation < 4'000.0F)
    {
        return PackRgba(
            LerpRgb(kMidland, kHighland, (elevation - 1'500.0F) / 2'500.0F));
    }
    return PackRgba(
        LerpRgb(kHighland, kPeak, (elevation - 4'000.0F) / 4'000.0F));
}

[[nodiscard]] u32 BiomeColor(const terrain::BiomeWeights& biomes) noexcept
{
    constexpr Rgb kOcean{0.025F, 0.11F, 0.24F};
    constexpr Rgb kDesert{0.72F, 0.56F, 0.31F};
    constexpr Rgb kGrassland{0.26F, 0.42F, 0.16F};
    constexpr Rgb kTemperateForest{0.075F, 0.25F, 0.11F};
    constexpr Rgb kBorealForest{0.08F, 0.20F, 0.16F};
    constexpr Rgb kTundra{0.43F, 0.48F, 0.42F};
    constexpr Rgb kAlpine{0.58F, 0.59F, 0.57F};
    constexpr Rgb kWetland{0.09F, 0.27F, 0.22F};

    Rgb color{};
    color.r = kOcean.r * biomes.ocean + kDesert.r * biomes.desert +
        kGrassland.r * biomes.grassland +
        kTemperateForest.r * biomes.temperateForest +
        kBorealForest.r * biomes.borealForest + kTundra.r * biomes.tundra +
        kAlpine.r * biomes.alpine + kWetland.r * biomes.wetland;
    color.g = kOcean.g * biomes.ocean + kDesert.g * biomes.desert +
        kGrassland.g * biomes.grassland +
        kTemperateForest.g * biomes.temperateForest +
        kBorealForest.g * biomes.borealForest + kTundra.g * biomes.tundra +
        kAlpine.g * biomes.alpine + kWetland.g * biomes.wetland;
    color.b = kOcean.b * biomes.ocean + kDesert.b * biomes.desert +
        kGrassland.b * biomes.grassland +
        kTemperateForest.b * biomes.temperateForest +
        kBorealForest.b * biomes.borealForest + kTundra.b * biomes.tundra +
        kAlpine.b * biomes.alpine + kWetland.b * biomes.wetland;
    return PackRgba(color);
}

[[nodiscard]] u32 TemperatureColor(const f32 temperatureC) noexcept
{
    constexpr Rgb kCold{0.10F, 0.15F, 0.75F};
    constexpr Rgb kMild{0.85F, 0.85F, 0.35F};
    constexpr Rgb kHot{0.80F, 0.08F, 0.08F};

    const f32 t = (temperatureC + 40.0F) / 80.0F;
    return PackRgba(
        t < 0.5F ? LerpRgb(kCold, kMild, t * 2.0F)
                 : LerpRgb(kMild, kHot, (t - 0.5F) * 2.0F));
}

[[nodiscard]] u32 PrecipitationColor(const f32 precipitation) noexcept
{
    constexpr Rgb kDry{0.62F, 0.52F, 0.28F};
    constexpr Rgb kModerate{0.42F, 0.55F, 0.22F};
    constexpr Rgb kWet{0.06F, 0.30F, 0.55F};

    return PackRgba(
        precipitation < 0.5F
            ? LerpRgb(kDry, kModerate, precipitation * 2.0F)
            : LerpRgb(kModerate, kWet, (precipitation - 0.5F) * 2.0F));
}

// Standing water depth: dry land is a dim grey so the water stands out,
// water goes from pale cyan (shallow) to deep navy.
[[nodiscard]] u32 WaterDepthColor(const terrain::TerrainSample& sample) noexcept
{
    if (sample.standingWaterDepthMeters <= 0.0)
    {
        const f32 shade = static_cast<f32>(
            0.16 + 0.18 *
                std::clamp(sample.elevationMeters / 4'000.0, 0.0, 1.0));
        return PackRgba({shade, shade, shade});
    }

    constexpr Rgb kShallow{0.55F, 0.90F, 0.95F};
    constexpr Rgb kDeep{0.02F, 0.05F, 0.30F};
    const f32 t = static_cast<f32>(std::clamp(
        std::sqrt(sample.standingWaterDepthMeters / 6'000.0), 0.0, 1.0));
    return PackRgba(LerpRgb(kShallow, kDeep, t));
}

[[nodiscard]] u32 TectonicColor(
    const terrain::GlobalTerrainFieldSample& sample) noexcept
{
    const f64 hue = std::fmod(
        static_cast<f64>(sample.nearestPlate % terrain::kMaxTectonicPlates) *
            137.50776405003785,
        360.0);
    const f64 sector = hue / 60.0;
    constexpr f64 chroma = 0.72;
    const f64 x = chroma * (1.0 - std::abs(std::fmod(sector, 2.0) - 1.0));
    Rgb color{};
    if (sector < 1.0) color = {static_cast<f32>(chroma), static_cast<f32>(x), 0.0F};
    else if (sector < 2.0) color = {static_cast<f32>(x), static_cast<f32>(chroma), 0.0F};
    else if (sector < 3.0) color = {0.0F, static_cast<f32>(chroma), static_cast<f32>(x)};
    else if (sector < 4.0) color = {0.0F, static_cast<f32>(x), static_cast<f32>(chroma)};
    else if (sector < 5.0) color = {static_cast<f32>(x), 0.0F, static_cast<f32>(chroma)};
    else color = {static_cast<f32>(chroma), 0.0F, static_cast<f32>(x)};
    color = {color.r + 0.12F, color.g + 0.12F, color.b + 0.12F};
    const f64 strongest = std::max({sample.convergenceMask, sample.divergenceMask, sample.transformMask});
    if (strongest > 0.08)
    {
        const Rgb boundary = sample.convergenceMask >= sample.divergenceMask && sample.convergenceMask >= sample.transformMask
            ? Rgb{0.98F,0.22F,0.16F}
            : (sample.divergenceMask >= sample.transformMask
                ? Rgb{0.10F,0.84F,0.96F} : Rgb{1.0F,0.88F,0.20F});
        color = LerpRgb(color, boundary, static_cast<f32>(std::clamp(strongest * 0.95, 0.0, 0.95)));
    }
    return PackRgba(color);
}

[[nodiscard]] u32 LayerColor(
    const FlatMapLayer layer,
    const terrain::TerrainSample& sample,
    const terrain::GlobalTerrainFieldSample* tectonics = nullptr) noexcept
{
    switch (layer)
    {
    case FlatMapLayer::Elevation:
        return ElevationColor(sample);
    case FlatMapLayer::Biomes:
        return BiomeColor(sample.biomes);
    case FlatMapLayer::Temperature:
        return TemperatureColor(sample.climate.temperatureC);
    case FlatMapLayer::Precipitation:
        return PrecipitationColor(sample.climate.precipitation);
    case FlatMapLayer::WaterDepth:
        return WaterDepthColor(sample);
    case FlatMapLayer::Tectonics:
        return tectonics != nullptr ? TectonicColor(*tectonics) : 0xFF202020U;
    }
    return 0xFF000000U;
}

constexpr const char* kVertexShader = R"(
struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput main(uint vertexId : SV_VertexID)
{
    const float2 positions[6] =
    {
        float2(-1.0, -1.0), float2(-1.0, 1.0), float2(1.0, -1.0),
        float2(1.0, -1.0), float2(-1.0, 1.0), float2(1.0, 1.0)
    };
    const float2 uvs[6] =
    {
        float2(0.0, 1.0), float2(0.0, 0.0), float2(1.0, 1.0),
        float2(1.0, 1.0), float2(0.0, 0.0), float2(1.0, 0.0)
    };

    VSOutput output;
    output.position = float4(positions[vertexId], 0.0, 1.0);
    output.uv = uvs[vertexId];
    return output;
}
)";

constexpr const char* kPixelShader = R"(
struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

struct Constants
{
    float viewWidth;
    float viewHeight;
    float rectLeft;
    float rectTop;
    float rectRight;
    float rectBottom;
    float markerU;
    float markerV;
    float hasMap;
    float gridOn;
    float progress;
    float pad;
};

[[vk::push_constant]] Constants g;

[[vk::binding(0, 0)]] [[vk::combinedImageSampler]] Texture2D g_map;
[[vk::binding(0, 0)]] [[vk::combinedImageSampler]] SamplerState g_sampler;

float4 main(VSOutput input) : SV_Target0
{
    const float2 viewSize = float2(g.viewWidth, g.viewHeight);
    const float2 rectMin = float2(g.rectLeft, g.rectTop);
    const float2 rectMax = float2(g.rectRight, g.rectBottom);
    const float2 mapUv = (input.uv - rectMin) / (rectMax - rectMin);
    const bool inside = all(mapUv >= 0.0) && all(mapUv <= 1.0);

    float3 color = float3(0.030, 0.034, 0.042);

    if (inside)
    {
        color = g.hasMap > 0.5
            ? g_map.SampleLevel(g_sampler, mapUv, 0).rgb
            : float3(0.07, 0.08, 0.10);

        if (g.gridOn > 0.5)
        {
            // Graticule every 30 degrees; the equator and prime meridian
            // (the longitude the HUD calls 0) are stronger.
            const float2 grid = float2(mapUv.x * 12.0, mapUv.y * 6.0);
            const float2 cellWidth = max(fwidth(grid), 1.0e-5);
            const float2 gridDistance = abs(frac(grid + 0.5) - 0.5) / cellWidth;
            const float gridLine = 1.0 - saturate(min(gridDistance.x, gridDistance.y));
            const float2 axisPixels =
                abs(mapUv - 0.5) * (rectMax - rectMin) * viewSize;
            const float axisLine =
                (axisPixels.x < 1.0 || axisPixels.y < 1.0) ? 1.0 : 0.0;
            color = lerp(color, float3(1.0, 1.0, 1.0), max(gridLine * 0.22, axisLine * 0.55));
        }
    }

    if (g.markerU >= 0.0)
    {
        const float2 markerView =
            rectMin + float2(g.markerU, g.markerV) * (rectMax - rectMin);
        const float2 delta = (input.uv - markerView) * viewSize;
        const float radius = length(delta);
        const float ring = 1.0 - saturate(abs(radius - 10.0) - 1.0);
        const float crossH =
            (abs(delta.y) < 1.0 && abs(delta.x) < 18.0) ? 1.0 : 0.0;
        const float crossV =
            (abs(delta.x) < 1.0 && abs(delta.y) < 18.0) ? 1.0 : 0.0;
        const float marker = saturate(max(ring, max(crossH, crossV)));
        const float halo = 1.0 - saturate(abs(radius - 10.0) - 3.0);
        color = lerp(color, float3(0.0, 0.0, 0.0), halo * 0.5);
        color = lerp(color, float3(1.0, 0.18, 0.10), marker);
    }

    if (g.progress < 1.0)
    {
        const float barTop = viewSize.y - 4.0;
        if (input.uv.y * viewSize.y >= barTop)
        {
            color = input.uv.x <= g.progress
                ? float3(0.25, 0.55, 1.0)
                : float3(0.10, 0.12, 0.16);
        }
    }

    return float4(color, 1.0);
}
)";

// Globe overlay: a pixel shader that intersects each pixel's view ray with the
// planet sphere (in units of the planet radius, so float precision is no
// issue), then draws a 30-degree graticule and the observer marker where it
// hits. The vertex shader above is shared.
constexpr const char* kGlobePixelShader = R"(
struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

struct Constants
{
    float camX; float camY; float camZ; float tanHalfFov;
    float fwdX; float fwdY; float fwdZ; float aspect;
    float rightX; float rightY; float rightZ; float viewHeight;
    float upX; float upY; float upZ; float markerValid;
    float markerX; float markerY; float markerZ; float pad;
};

[[vk::push_constant]] Constants g;

float4 main(VSOutput input) : SV_Target0
{
    const float3 origin = float3(g.camX, g.camY, g.camZ);
    const float3 forward = float3(g.fwdX, g.fwdY, g.fwdZ);
    const float3 right = float3(g.rightX, g.rightY, g.rightZ);
    const float3 up = float3(g.upX, g.upY, g.upZ);

    const float2 ndc = float2(input.uv.x * 2.0 - 1.0, 1.0 - input.uv.y * 2.0);
    const float3 direction = normalize(
        forward +
        right * (ndc.x * g.aspect * g.tanHalfFov) +
        up * (ndc.y * g.tanHalfFov));

    const float b = dot(origin, direction);
    const float c = dot(origin, origin) - 1.0;
    const float discriminant = b * b - c;
    if (discriminant < 0.0)
    {
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    const float t = -b - sqrt(discriminant);
    if (t < 0.0)
    {
        return float4(0.0, 0.0, 0.0, 0.0);
    }

    const float3 p = normalize(origin + direction * t);
    const float2 degrees = float2(asin(clamp(p.y, -1.0, 1.0)), atan2(p.z, p.x)) * 57.29578;

    const float2 grid = degrees / 30.0;
    const float2 cell = clamp(fwidth(grid), 1.0e-4, 0.08);
    const float2 gridDistance = abs(frac(grid + 0.5) - 0.5) / cell;
    const float gridLine = 1.0 - saturate(min(gridDistance.x, gridDistance.y));

    const float2 axisDistance = abs(degrees) / (cell * 30.0);
    const float axisLine = 1.0 - saturate(min(axisDistance.x, axisDistance.y));

    float alpha = max(gridLine * 0.30, axisLine * 0.55);
    float3 color = float3(1.0, 1.0, 1.0);

    if (g.markerValid > 0.5)
    {
        const float3 marker = float3(g.markerX, g.markerY, g.markerZ);
        const float angle = acos(clamp(dot(p, marker), -1.0, 1.0));
        const float pixelAngle =
            2.0 * g.tanHalfFov / max(g.viewHeight, 1.0) *
            max(length(origin) - 1.0, 0.05);
        const float ring = 1.0 - saturate(abs(angle - pixelAngle * 14.0) / pixelAngle - 1.5);
        const float dotCore = 1.0 - saturate(angle / (pixelAngle * 4.0));
        const float halo = 1.0 - saturate(abs(angle - pixelAngle * 14.0) / pixelAngle - 4.0);
        const float markerAlpha = saturate(max(ring, dotCore));
        color = lerp(color, float3(0.0, 0.0, 0.0), halo * 0.6 * (1.0 - markerAlpha));
        alpha = max(alpha, halo * 0.35);
        color = lerp(color, float3(1.0, 0.18, 0.10), markerAlpha);
        alpha = max(alpha, markerAlpha);
    }

    return float4(color, alpha);
}
)";

[[nodiscard]] u32 FloatBits(const f32 value) noexcept
{
    return std::bit_cast<u32>(value);
}
} // namespace

std::string_view FlatMapLayerName(const FlatMapLayer layer) noexcept
{
    switch (layer)
    {
    case FlatMapLayer::Elevation:
        return "elevation";
    case FlatMapLayer::Biomes:
        return "biomes";
    case FlatMapLayer::Temperature:
        return "temperature";
    case FlatMapLayer::Precipitation:
        return "precipitation";
    case FlatMapLayer::WaterDepth:
        return "water_depth";
    case FlatMapLayer::Tectonics:
        return "tectonics";
    }
    return "elevation";
}

std::optional<FlatMapLayer> ParseFlatMapLayer(
    const std::string_view name) noexcept
{
    for (u32 index = 0U; index < kFlatMapLayerCount; ++index)
    {
        const auto layer = static_cast<FlatMapLayer>(index);
        if (name == FlatMapLayerName(layer))
        {
            return layer;
        }
    }
    return std::nullopt;
}

FlatMapLatLon FlatMapLatLonFromDirection(
    const math::Double3& direction) noexcept
{
    const math::Double3 unit = math::Normalize(direction);
    return {
        .latitudeDegrees =
            std::asin(std::clamp(unit.y, -1.0, 1.0)) * kRadiansToDegrees,
        .longitudeDegrees = std::atan2(unit.z, unit.x) * kRadiansToDegrees
    };
}

math::Double3 FlatMapDirectionFromLatLon(
    const f64 latitudeDegrees,
    const f64 longitudeDegrees) noexcept
{
    const f64 latitude = latitudeDegrees * kDegreesToRadians;
    const f64 longitude = longitudeDegrees * kDegreesToRadians;
    const f64 cosLatitude = std::cos(latitude);
    return {
        cosLatitude * std::cos(longitude),
        std::sin(latitude),
        cosLatitude * std::sin(longitude)
    };
}

math::Double2 FlatMapUvFromLatLon(const FlatMapLatLon& latLon) noexcept
{
    return {
        (latLon.longitudeDegrees + 180.0) / 360.0,
        (90.0 - latLon.latitudeDegrees) / 180.0
    };
}

FlatMapLatLon FlatMapLatLonFromUv(const math::Double2& mapUv) noexcept
{
    return {
        .latitudeDegrees = 90.0 - mapUv.y * 180.0,
        .longitudeDegrees = mapUv.x * 360.0 - 180.0
    };
}

FlatMapRect FlatMapViewRect(
    const u32 viewWidth,
    const u32 viewHeight) noexcept
{
    if (viewWidth == 0U || viewHeight == 0U)
    {
        return {};
    }

    const f64 aspect =
        static_cast<f64>(viewWidth) / static_cast<f64>(viewHeight);
    constexpr f64 kMapAspect = 2.0;

    FlatMapRect rect;
    if (aspect >= kMapAspect)
    {
        const f64 width = kMapAspect / aspect;
        rect.left = 0.5 - width * 0.5;
        rect.right = 0.5 + width * 0.5;
    }
    else
    {
        const f64 height = aspect / kMapAspect;
        rect.top = 0.5 - height * 0.5;
        rect.bottom = 0.5 + height * 0.5;
    }
    return rect;
}

std::optional<math::Double2> FlatMapUvFromViewUv(
    const u32 viewWidth,
    const u32 viewHeight,
    const f64 viewU,
    const f64 viewV) noexcept
{
    const FlatMapRect rect = FlatMapViewRect(viewWidth, viewHeight);
    const f64 width = rect.right - rect.left;
    const f64 height = rect.bottom - rect.top;
    if (width <= 0.0 || height <= 0.0)
    {
        return std::nullopt;
    }

    const f64 u = (viewU - rect.left) / width;
    const f64 v = (viewV - rect.top) / height;
    if (u < 0.0 || u > 1.0 || v < 0.0 || v > 1.0)
    {
        return std::nullopt;
    }
    return math::Double2{u, v};
}

std::optional<math::Double3> GlobePickDirection(
    const render_view::CameraState& camera,
    const f64 planetRadiusMeters,
    const u32 viewWidth,
    const u32 viewHeight,
    const f32 u,
    const f32 v) noexcept
{
    if (planetRadiusMeters <= 0.0)
    {
        return std::nullopt;
    }

    const auto ray = render_view::ViewportRay(camera, viewWidth, viewHeight, u, v);
    if (!ray.has_value())
    {
        return std::nullopt;
    }

    // Work in planet radii so the arithmetic stays well conditioned.
    const math::Double3 origin = ray->origin * (1.0 / planetRadiusMeters);
    const math::Double3 direction = math::Normalize(ray->direction);
    const f64 b = math::Dot(origin, direction);
    const f64 c = math::Dot(origin, origin) - 1.0;
    const f64 discriminant = b * b - c;
    if (discriminant < 0.0)
    {
        return std::nullopt;
    }

    const f64 t = -b - std::sqrt(discriminant);
    if (t < 0.0)
    {
        return std::nullopt;
    }
    return math::Normalize(origin + direction * t);
}

class StudioFlatMapRenderer::Impl
{
public:
    Impl(
        rhi::Device& device,
        const shader::Compiler& compiler,
        const u32 framesInFlight)
        : device_(device),
          framesInFlight_(std::max(framesInFlight, 1U))
    {
        const auto vertex = compiler.Compile({
            .source = kVertexShader,
            .entryPoint = "main",
            .stage = shader::Stage::Vertex,
            .debug = false
        });
        const auto pixel = compiler.Compile({
            .source = kPixelShader,
            .entryPoint = "main",
            .stage = shader::Stage::Pixel,
            .debug = false
        });
        if (vertex.bytecode.empty() || pixel.bytecode.empty())
        {
            throw std::runtime_error(
                "Orbit failed to compile the flat map shaders.");
        }

        pipeline_ = device_.CreateGraphicsPipeline({
            .vertexShader =
                {.data = vertex.bytecode.data(),
                 .size = vertex.bytecode.size()},
            .pixelShader =
                {.data = pixel.bytecode.data(),
                 .size = pixel.bytecode.size()},
            .vertexAttributes = {},
            .vertexStrideBytes = 0U,
            .pushConstantDwords = 12U,
            .shaderResourceBuffers = 0U,
            .sampledTextures = 1U,
            .topology = rhi::PrimitiveTopology::TriangleList,
            .fillMode = rhi::FillMode::Solid,
            .cullMode = rhi::CullMode::None,
            .depthTest = false,
            .depthWrite = false
        });

        const auto globePixel = compiler.Compile({
            .source = kGlobePixelShader,
            .entryPoint = "main",
            .stage = shader::Stage::Pixel,
            .debug = false
        });
        if (globePixel.bytecode.empty())
        {
            throw std::runtime_error(
                "Orbit failed to compile the globe overlay shader.");
        }

        globePipeline_ = device_.CreateGraphicsPipeline({
            .vertexShader =
                {.data = vertex.bytecode.data(),
                 .size = vertex.bytecode.size()},
            .pixelShader =
                {.data = globePixel.bytecode.data(),
                 .size = globePixel.bytecode.size()},
            .vertexAttributes = {},
            .vertexStrideBytes = 0U,
            .pushConstantDwords = 20U,
            .shaderResourceBuffers = 0U,
            .sampledTextures = 0U,
            .topology = rhi::PrimitiveTopology::TriangleList,
            .fillMode = rhi::FillMode::Solid,
            .cullMode = rhi::CullMode::None,
            .blendMode = rhi::BlendMode::Alpha,
            .depthTest = false,
            .depthWrite = false
        });
    }

    void DrawGlobeOverlay(
        rhi::CommandList& commands,
        rhi::Texture& target,
        const u32 width,
        const u32 height,
        const render_view::CameraState& camera,
        const f64 planetRadiusMeters,
        const std::optional<math::Double3>& markerDirection)
    {
        if (width == 0U || height == 0U || planetRadiusMeters <= 0.0)
        {
            return;
        }

        const math::Double3 forward = math::Normalize(math::Double3{
            camera.forward.x, camera.forward.y, camera.forward.z});
        const math::Double3 requestedUp = math::Normalize(math::Double3{
            camera.up.x, camera.up.y, camera.up.z});
        // Same basis as render_view::ViewportRay: screen-right is up x forward.
        const math::Double3 right =
            math::Normalize(math::Cross(requestedUp, forward));
        const math::Double3 up = math::Normalize(math::Cross(forward, right));
        const math::Double3 origin =
            camera.localPositionMeters * (1.0 / planetRadiusMeters);

        const math::Double3 marker = markerDirection.has_value()
            ? math::Normalize(*markerDirection)
            : math::Double3{0.0, 1.0, 0.0};

        const auto bits = [](const f64 value)
        {
            return FloatBits(static_cast<f32>(value));
        };
        const std::array<u32, 20> constants{
            bits(origin.x), bits(origin.y), bits(origin.z),
            bits(std::tan(static_cast<f64>(camera.verticalFovRadians) * 0.5)),
            bits(forward.x), bits(forward.y), bits(forward.z),
            bits(static_cast<f64>(width) / static_cast<f64>(height)),
            bits(right.x), bits(right.y), bits(right.z),
            bits(static_cast<f64>(height)),
            bits(up.x), bits(up.y), bits(up.z),
            FloatBits(markerDirection.has_value() ? 1.0F : 0.0F),
            bits(marker.x), bits(marker.y), bits(marker.z),
            FloatBits(0.0F)
        };

        commands.SetRenderTarget(target);
        commands.SetViewport({
            .x = 0.0F,
            .y = 0.0F,
            .width = static_cast<f32>(width),
            .height = static_cast<f32>(height),
            .minDepth = 0.0F,
            .maxDepth = 1.0F
        });
        commands.SetScissor({
            .left = 0,
            .top = 0,
            .right = static_cast<i32>(width),
            .bottom = static_cast<i32>(height)
        });
        commands.SetGraphicsPipeline(*globePipeline_);
        commands.SetGraphicsConstants(
            std::span<const u32>(constants.data(), constants.size()));
        commands.Draw(6);
    }

    void Advance(
        const std::string_view viewId,
        const SourceBinding* const source,
        const FlatMapLayer layer,
        const std::optional<math::Double3>& markerDirection,
        const f64 budgetMilliseconds)
    {
        auto& map = Entry(viewId);
        map.layer = layer;
        map.marker = markerDirection.has_value()
            ? std::optional<FlatMapLatLon>(
                  FlatMapLatLonFromDirection(*markerDirection))
            : std::nullopt;
        map.hasSource = source != nullptr && source->terrain != nullptr;

        if (!map.hasSource)
        {
            return;
        }

        if (!map.generationStarted ||
            map.revision != source->revision ||
            map.planet != source->planet)
        {
            // Terrain (or the planet) changed: regenerate, keeping the old
            // pixels on screen until each row is overwritten.
            map.generationStarted = true;
            map.revision = source->revision;
            map.planet = source->planet;
            map.nextRow = 0U;
            map.nextColumn = 0U;
            map.complete = false;
            map.dirty = true;
        }

        if (map.complete)
        {
            return;
        }

        const auto started = std::chrono::steady_clock::now();
        const f64 footprintMeters = std::max(
            2.0 * std::numbers::pi * std::max(source->radiusMeters, 1.0) /
            static_cast<f64>(kFlatMapWidth),
            1.0);
        const auto* const analytic =
            dynamic_cast<const terrain::AnalyticTerrainSource*>(source->terrain);

        while (map.nextRow < kFlatMapHeight)
        {
            const f64 v =
                (static_cast<f64>(map.nextRow) + 0.5) /
                static_cast<f64>(kFlatMapHeight);

            for (; map.nextColumn < kFlatMapWidth; ++map.nextColumn)
            {
                const f64 u =
                    (static_cast<f64>(map.nextColumn) + 0.5) /
                    static_cast<f64>(kFlatMapWidth);
                const auto latLon = FlatMapLatLonFromUv({u, v});
                const terrain::TerrainQuery query{
                    .unitDirection = FlatMapDirectionFromLatLon(
                        latLon.latitudeDegrees,
                        latLon.longitudeDegrees),
                    .footprintMeters = footprintMeters,
                    .planet = source->planet,
                    .radialOffsetMeters = 0.0
                };
                const terrain::TerrainSample sample = source->terrain->Sample(query);
                const terrain::GlobalTerrainFieldSample* tectonics = nullptr;
                terrain::GlobalTerrainFieldSample tectonicSample{};
                if (analytic != nullptr)
                {
                    tectonicSample = analytic->GlobalFields().Sample(query);
                    tectonics = &tectonicSample;
                }

                const std::size_t index =
                    static_cast<std::size_t>(map.nextRow) * kFlatMapWidth +
                    map.nextColumn;
                for (u32 layerIndex = 0U;
                     layerIndex < kFlatMapLayerCount;
                     ++layerIndex)
                {
                    map.pixels[layerIndex][index] = LayerColor(
                        static_cast<FlatMapLayer>(layerIndex), sample,
                        static_cast<FlatMapLayer>(layerIndex) == FlatMapLayer::Tectonics
                            ? tectonics : nullptr);
                }

                if ((map.nextColumn & 31U) == 31U &&
                    std::chrono::duration<f64, std::milli>(
                        std::chrono::steady_clock::now() - started)
                            .count() >= budgetMilliseconds)
                {
                    ++map.nextColumn;
                    map.dirty = true;
                    return;
                }
            }

            map.nextColumn = 0U;
            ++map.nextRow;
            map.dirty = true;
        }

        map.complete = true;
        map.dirty = true;
    }

    void Draw(
        rhi::CommandList& commands,
        rhi::Texture& target,
        const u32 width,
        const u32 height,
        const std::string_view viewId,
        const u32 frameSlot)
    {
        if (width == 0U || height == 0U)
        {
            return;
        }

        auto& map = Entry(viewId);

        const bool shownLayerChanged =
            !map.textureDefined || map.uploadedLayer != map.layer;
        if (map.hasPixels && (map.dirty || shownLayerChanged))
        {
            Upload(commands, map, frameSlot);
        }

        const FlatMapRect rect = FlatMapViewRect(width, height);
        f32 markerU = -1.0F;
        f32 markerV = 0.0F;
        if (map.marker.has_value())
        {
            const auto uv = FlatMapUvFromLatLon(*map.marker);
            markerU = static_cast<f32>(std::clamp(uv.x, 0.0, 1.0));
            markerV = static_cast<f32>(std::clamp(uv.y, 0.0, 1.0));
        }

        const f32 progress = map.complete || !map.hasSource
            ? 1.0F
            : static_cast<f32>(map.nextRow) /
                static_cast<f32>(kFlatMapHeight);

        const std::array<u32, 12> constants{
            FloatBits(static_cast<f32>(width)),
            FloatBits(static_cast<f32>(height)),
            FloatBits(static_cast<f32>(rect.left)),
            FloatBits(static_cast<f32>(rect.top)),
            FloatBits(static_cast<f32>(rect.right)),
            FloatBits(static_cast<f32>(rect.bottom)),
            FloatBits(markerU),
            FloatBits(markerV),
            FloatBits(map.textureDefined ? 1.0F : 0.0F),
            FloatBits(1.0F),
            FloatBits(progress),
            FloatBits(0.0F)
        };

        commands.SetRenderTarget(target);
        commands.SetViewport({
            .x = 0.0F,
            .y = 0.0F,
            .width = static_cast<f32>(width),
            .height = static_cast<f32>(height),
            .minDepth = 0.0F,
            .maxDepth = 1.0F
        });
        commands.SetScissor({
            .left = 0,
            .top = 0,
            .right = static_cast<i32>(width),
            .bottom = static_cast<i32>(height)
        });
        commands.SetGraphicsPipeline(*pipeline_);
        commands.SetGraphicsConstants(
            std::span<const u32>(constants.data(), constants.size()));
        if (map.texture == nullptr)
        {
            EnsureTexture(map);
        }
        commands.SetGraphicsTexture(0, *map.texture);
        commands.Draw(6);
    }

    [[nodiscard]] std::optional<StudioFlatMapStatus> Status(
        const std::string_view viewId) const
    {
        const auto found = maps_.find(viewId);
        if (found == maps_.end())
        {
            return std::nullopt;
        }

        const auto& map = found->second;
        StudioFlatMapStatus status;
        status.layer = map.layer;
        status.hasSource = map.hasSource;
        status.rowsGenerated = map.complete ? kFlatMapHeight : map.nextRow;
        status.rowsTotal = kFlatMapHeight;
        status.complete = map.complete;
        status.markerValid = map.marker.has_value();
        if (map.marker.has_value())
        {
            status.marker = *map.marker;
        }
        return status;
    }

    [[nodiscard]] bool Generating() const noexcept
    {
        for (const auto& [id, map] : maps_)
        {
            static_cast<void>(id);
            if (map.hasSource && map.generationStarted && !map.complete)
            {
                return true;
            }
        }
        return false;
    }

    void Forget(const std::string_view viewId)
    {
        const auto found = maps_.find(viewId);
        if (found != maps_.end())
        {
            maps_.erase(found);
        }
    }

private:
    struct ViewMap
    {
        std::array<std::vector<u32>, kFlatMapLayerCount> pixels;
        bool hasPixels{false};
        FlatMapLayer layer{FlatMapLayer::Elevation};
        FlatMapLayer uploadedLayer{FlatMapLayer::Elevation};
        bool hasSource{false};
        bool generationStarted{false};
        bool complete{false};
        bool dirty{false};
        bool textureDefined{false};
        u64 revision{0U};
        world::PlanetId planet{};
        u32 nextRow{0U};
        u32 nextColumn{0U};
        std::optional<FlatMapLatLon> marker;
        std::unique_ptr<rhi::Texture> texture;
        std::vector<std::unique_ptr<rhi::Buffer>> staging;
    };

    ViewMap& Entry(const std::string_view viewId)
    {
        auto found = maps_.find(viewId);
        if (found == maps_.end())
        {
            found = maps_.emplace(std::string(viewId), ViewMap{}).first;
            auto& map = found->second;
            for (auto& layerPixels : map.pixels)
            {
                layerPixels.assign(
                    static_cast<std::size_t>(kFlatMapWidth) * kFlatMapHeight,
                    0xFF120F0FU);
            }
            map.hasPixels = true;
            map.dirty = true;
        }
        return found->second;
    }

    void EnsureTexture(ViewMap& map)
    {
        if (map.texture != nullptr)
        {
            return;
        }
        map.texture = device_.CreateTexture({
            .width = kFlatMapWidth,
            .height = kFlatMapHeight,
            .format = rhi::TextureFormat::RGBA8_UNorm,
            .initialState = rhi::ResourceState::ShaderResource
        });
    }

    // Copies the active layer's pixels into the view's texture. Done inside
    // the frame's command list, before any render target is bound, with one
    // staging buffer per frame in flight so a copy never overwrites data the
    // GPU is still reading.
    void Upload(
        rhi::CommandList& commands,
        ViewMap& map,
        const u32 frameSlot)
    {
        EnsureTexture(map);
        if (map.staging.size() != framesInFlight_)
        {
            map.staging.clear();
            map.staging.resize(framesInFlight_);
        }

        auto& staging = map.staging[frameSlot % framesInFlight_];
        constexpr u64 kBytes =
            static_cast<u64>(kFlatMapWidth) * kFlatMapHeight * 4U;
        if (staging == nullptr)
        {
            staging = device_.CreateBuffer({
                .sizeBytes = kBytes,
                .usage = rhi::BufferUsage::Generic,
                .memory = rhi::MemoryUsage::HostVisible,
                .initialState = rhi::ResourceState::Common
            });
        }

        std::byte* mapped = staging->Map();
        std::memcpy(
            mapped,
            map.pixels[static_cast<u32>(map.layer)].data(),
            static_cast<std::size_t>(kBytes));
        staging->Unmap();

        commands.Transition(
            *map.texture,
            rhi::ResourceState::ShaderResource,
            rhi::ResourceState::CopyDestination);
        commands.CopyBufferToTexture(*staging, 0, *map.texture);
        commands.Transition(
            *map.texture,
            rhi::ResourceState::CopyDestination,
            rhi::ResourceState::ShaderResource);

        map.textureDefined = true;
        map.uploadedLayer = map.layer;
        map.dirty = false;
    }

    rhi::Device& device_;
    u32 framesInFlight_;
    std::unique_ptr<rhi::GraphicsPipeline> pipeline_;
    std::unique_ptr<rhi::GraphicsPipeline> globePipeline_;
    std::map<std::string, ViewMap, std::less<>> maps_;
};

StudioFlatMapRenderer::StudioFlatMapRenderer(
    rhi::Device& device,
    const shader::Compiler& compiler,
    const u32 framesInFlight)
    : impl_(std::make_unique<Impl>(device, compiler, framesInFlight))
{
}

StudioFlatMapRenderer::~StudioFlatMapRenderer() = default;

void StudioFlatMapRenderer::Advance(
    const std::string_view viewId,
    const SourceBinding* const source,
    const FlatMapLayer layer,
    const std::optional<math::Double3>& markerDirection,
    const f64 budgetMilliseconds)
{
    impl_->Advance(viewId, source, layer, markerDirection, budgetMilliseconds);
}

void StudioFlatMapRenderer::Draw(
    rhi::CommandList& commands,
    rhi::Texture& target,
    const u32 width,
    const u32 height,
    const std::string_view viewId,
    const u32 frameSlot)
{
    impl_->Draw(commands, target, width, height, viewId, frameSlot);
}

void StudioFlatMapRenderer::DrawGlobeOverlay(
    rhi::CommandList& commands,
    rhi::Texture& target,
    const u32 width,
    const u32 height,
    const render_view::CameraState& camera,
    const f64 planetRadiusMeters,
    const std::optional<math::Double3>& markerDirection)
{
    impl_->DrawGlobeOverlay(
        commands,
        target,
        width,
        height,
        camera,
        planetRadiusMeters,
        markerDirection);
}

std::optional<StudioFlatMapStatus> StudioFlatMapRenderer::Status(
    const std::string_view viewId) const
{
    return impl_->Status(viewId);
}

bool StudioFlatMapRenderer::Generating() const noexcept
{
    return impl_->Generating();
}

void StudioFlatMapRenderer::Forget(const std::string_view viewId)
{
    impl_->Forget(viewId);
}
} // namespace orbit::studio_ui
