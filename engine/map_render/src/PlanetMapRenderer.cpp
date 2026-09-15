#include <orbit/map_render/PlanetMapRenderer.hpp>

#include <orbit/rhi/Command.hpp>
#include <orbit/rhi/Device.hpp>
#include <orbit/rhi/Pipeline.hpp>
#include <orbit/rhi/Queue.hpp>
#include <orbit/rhi/Resource.hpp>
#include <orbit/shader/ShaderCompiler.hpp>
#include <orbit/terrain/AnalyticTerrainSource.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <future>
#include <numbers>
#include <vector>

namespace orbit::map_render
{
namespace
{
constexpr u32 kMapWidth = 1024;
constexpr u32 kMapHeight = 512;

[[nodiscard]] u32 PackRgba(
    const f32 r, const f32 g, const f32 b, const f32 a = 1.0F) noexcept
{
    const auto channel = [](const f32 value) -> u32
    {
        return static_cast<u32>(
            std::clamp(value, 0.0F, 1.0F) * 255.0F + 0.5F);
    };

    return channel(r) | (channel(g) << 8) | (channel(b) << 16) |
        (channel(a) << 24);
}

[[nodiscard]] f32 Lerp(const f32 a, const f32 b, const f32 t) noexcept
{
    return a + (b - a) * std::clamp(t, 0.0F, 1.0F);
}

struct Rgb
{
    f32 r{0.0F};
    f32 g{0.0F};
    f32 b{0.0F};
};

[[nodiscard]] Rgb LerpRgb(const Rgb& a, const Rgb& b, const f32 t) noexcept
{
    return {Lerp(a.r, b.r, t), Lerp(a.g, b.g, t), Lerp(a.b, b.b, t)};
}

[[nodiscard]] u32 ElevationColor(
    const terrain::TerrainSample& sample) noexcept
{
    if (sample.standingWaterDepthMeters > 0.0)
    {
        constexpr Rgb kShallow{0.42F, 0.66F, 0.72F};
        constexpr Rgb kDeep{0.02F, 0.06F, 0.22F};
        const f32 t = static_cast<f32>(
            std::clamp(sample.standingWaterDepthMeters / 5'000.0, 0.0, 1.0));
        const Rgb color = LerpRgb(kShallow, kDeep, t);
        return PackRgba(color.r, color.g, color.b);
    }

    constexpr Rgb kLowland{0.20F, 0.45F, 0.18F};
    constexpr Rgb kMidland{0.62F, 0.56F, 0.28F};
    constexpr Rgb kHighland{0.45F, 0.32F, 0.22F};
    constexpr Rgb kPeak{0.96F, 0.96F, 0.98F};

    const f32 elevation = static_cast<f32>(sample.elevationMeters);
    Rgb color;
    if (elevation < 1'500.0F)
    {
        color = LerpRgb(kLowland, kMidland, elevation / 1'500.0F);
    }
    else if (elevation < 4'000.0F)
    {
        color = LerpRgb(kMidland, kHighland, (elevation - 1'500.0F) / 2'500.0F);
    }
    else
    {
        color = LerpRgb(kHighland, kPeak, (elevation - 4'000.0F) / 4'000.0F);
    }

    return PackRgba(color.r, color.g, color.b);
}

// Plate-boundary subtypes a pixel's dominant tectonic signal can be
// classified into, matching real-world plate-tectonics vocabulary --
// see ClassifyBoundary below and its color table in TectonicsColor.
enum class BoundaryType
{
    None,
    // Convergent, both plates continental: continental collision (e.g.
    // the Himalaya).
    Orogeny,
    // Convergent, at least one plate oceanic: oceanic plate sinks under
    // the other, building a volcanic arc/trench (e.g. the Andes, Japan).
    Subduction,
    // Divergent, both plates oceanic: seafloor spreading (e.g. the
    // Mid-Atlantic Ridge).
    Ridge,
    // Divergent, at least one plate continental: continental rifting
    // (e.g. the East African Rift).
    Rift,
    // Lateral shear, neither strongly converging nor diverging:
    // strike-slip transform fault (e.g. the San Andreas).
    Transform
};

struct BoundaryClassification
{
    BoundaryType type{BoundaryType::None};
    f64 strength{0.0};
};

[[nodiscard]] BoundaryClassification ClassifyBoundary(
    const f64 convergenceMask,
    const f64 divergenceMask,
    const f64 transformMask,
    const bool nearestContinental,
    const bool secondContinental) noexcept
{
    // boundaryWidthDot (see TectonicFieldDesc) is shared with real
    // mountain-range shaping and can't be narrowed just for map styling
    // -- see the comment there. This threshold, and the power-curve
    // sharpening below, are the map-only knobs: tuned by rendering the
    // actual Tectonics layer's pixels for a threshold that keeps
    // boundaries continuous (a much stricter cutoff starts cutting real
    // gaps into weaker stretches of an otherwise-continuous boundary).
    constexpr f64 kThreshold = 0.45;

    const f64 dominant =
        std::max(convergenceMask, std::max(divergenceMask, transformMask));

    if (dominant <= kThreshold)
    {
        return {};
    }

    if (dominant == convergenceMask)
    {
        return {
            (nearestContinental && secondContinental)
                ? BoundaryType::Orogeny
                : BoundaryType::Subduction,
            dominant};
    }

    if (dominant == divergenceMask)
    {
        return {
            (nearestContinental || secondContinental)
                ? BoundaryType::Rift
                : BoundaryType::Ridge,
            dominant};
    }

    return {BoundaryType::Transform, dominant};
}

[[nodiscard]] u32 TectonicsColor(
    const terrain::TerrainSample& sample,
    const f64 convergenceMask,
    const f64 divergenceMask,
    const f64 transformMask,
    const bool nearestContinental,
    const bool secondContinental,
    const f64 hotspotElevationMeters) noexcept
{
    // Muted grayscale base so land/ocean shapes stay legible under the
    // boundary/hotspot overlay.
    const f32 gray = sample.standingWaterDepthMeters > 0.0
        ? 0.18F
        : static_cast<f32>(
              0.35 +
              0.35 * std::clamp(sample.elevationMeters / 6'000.0, 0.0, 1.0));

    Rgb color{gray, gray, gray};

    if (hotspotElevationMeters > 200.0)
    {
        constexpr Rgb kHotspot{0.85F, 0.15F, 0.85F};
        const f32 t = static_cast<f32>(
            std::clamp(hotspotElevationMeters / 3'000.0, 0.0, 1.0));
        color = LerpRgb(color, kHotspot, 0.5F + 0.5F * t);
        return PackRgba(color.r, color.g, color.b);
    }

    const BoundaryClassification boundary = ClassifyBoundary(
        convergenceMask, divergenceMask, transformMask,
        nearestContinental, secondContinental);

    Rgb boundaryColor{};
    switch (boundary.type)
    {
        case BoundaryType::None:
            return PackRgba(color.r, color.g, color.b);
        case BoundaryType::Orogeny:
            boundaryColor = {0.85F, 0.12F, 0.12F};
            break;
        case BoundaryType::Subduction:
            boundaryColor = {0.95F, 0.45F, 0.05F};
            break;
        case BoundaryType::Ridge:
            boundaryColor = {0.15F, 0.80F, 0.90F};
            break;
        case BoundaryType::Rift:
            boundaryColor = {0.25F, 0.85F, 0.35F};
            break;
        case BoundaryType::Transform:
            boundaryColor = {0.85F, 0.85F, 0.15F};
            break;
    }

    // Steepens the blend near the classification cutoff (boundary.strength
    // is already 0..1) so a boundary reads as a comparatively crisp line
    // instead of a soft glow, without introducing the gaps a stricter
    // kThreshold alone would cut into weaker stretches of a real boundary.
    const f64 sharpened = std::pow(std::clamp(boundary.strength, 0.0, 1.0), 2.2);

    color = LerpRgb(color, boundaryColor, static_cast<f32>(sharpened));

    return PackRgba(color.r, color.g, color.b);
}

[[nodiscard]] u32 BiomeColor(const terrain::BiomeWeights& biomes) noexcept
{
    // Kept in sync by hand with the flat biome colors baked into
    // engine/terrain_render/src/TerrainSurfaceShader.hpp -- there's no
    // shared constant table since those live inside a raw HLSL string.
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

    return PackRgba(color.r, color.g, color.b);
}

[[nodiscard]] u32 TemperatureColor(const f32 temperatureC) noexcept
{
    constexpr Rgb kCold{0.10F, 0.15F, 0.75F};
    constexpr Rgb kMild{0.85F, 0.85F, 0.35F};
    constexpr Rgb kHot{0.80F, 0.08F, 0.08F};

    const f32 t = (temperatureC + 40.0F) / 80.0F;
    const Rgb color = t < 0.5F
        ? LerpRgb(kCold, kMild, t * 2.0F)
        : LerpRgb(kMild, kHot, (t - 0.5F) * 2.0F);

    return PackRgba(color.r, color.g, color.b);
}

[[nodiscard]] u32 PrecipitationColor(const f32 precipitation) noexcept
{
    constexpr Rgb kDry{0.62F, 0.52F, 0.28F};
    constexpr Rgb kModerate{0.42F, 0.55F, 0.22F};
    constexpr Rgb kWet{0.06F, 0.30F, 0.55F};

    const Rgb color = precipitation < 0.5F
        ? LerpRgb(kDry, kModerate, precipitation * 2.0F)
        : LerpRgb(kModerate, kWet, (precipitation - 0.5F) * 2.0F);

    return PackRgba(color.r, color.g, color.b);
}

using LayerBuffers = std::array<std::vector<u32>, kMapLayerCount>;

[[nodiscard]] LayerBuffers GenerateLayers(
    const terrain::AnalyticTerrainSource& terrainSource)
{
    LayerBuffers buffers;
    for (auto& buffer : buffers)
    {
        buffer.resize(static_cast<std::size_t>(kMapWidth) * kMapHeight);
    }

    // Coarse enough that per-pixel detail octaves and fine mountain
    // ridge noise are skipped via the existing footprint-based
    // DetailWeight gating (see ProceduralNoise.hpp) -- this is a
    // whole-planet overview, not a close-up.
    constexpr f64 footprintMeters = 150'000.0;

    for (u32 y = 0; y < kMapHeight; ++y)
    {
        for (u32 x = 0; x < kMapWidth; ++x)
        {
            const math::Double2 uv{
                (static_cast<f64>(x) + 0.5) / kMapWidth,
                (static_cast<f64>(y) + 0.5) / kMapHeight
            };

            const math::Double3 direction = EquirectDirectionFromUv(uv);

            const terrain::TerrainSample sample = terrainSource.Sample(
                {.unitDirection = direction, .footprintMeters = footprintMeters});

            const terrain::GlobalTerrainFieldSample globalSample =
                terrainSource.GlobalFields().Sample(
                    {.unitDirection = direction, .footprintMeters = footprintMeters});

            const std::size_t index =
                static_cast<std::size_t>(y) * kMapWidth + x;

            buffers[static_cast<u32>(MapLayer::Elevation)][index] =
                ElevationColor(sample);
            buffers[static_cast<u32>(MapLayer::Tectonics)][index] =
                TectonicsColor(
                    sample,
                    globalSample.convergenceMask,
                    globalSample.divergenceMask,
                    globalSample.transformMask,
                    globalSample.nearestPlateContinental,
                    globalSample.secondPlateContinental,
                    globalSample.hotspotElevationMeters);
            buffers[static_cast<u32>(MapLayer::Biomes)][index] =
                BiomeColor(sample.biomes);
            buffers[static_cast<u32>(MapLayer::Temperature)][index] =
                TemperatureColor(sample.climate.temperatureC);
            buffers[static_cast<u32>(MapLayer::Precipitation)][index] =
                PrecipitationColor(sample.climate.precipitation);
        }
    }

    return buffers;
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

[[vk::binding(0, 0)]] [[vk::combinedImageSampler]] Texture2D g_mapTexture;
[[vk::binding(0, 0)]] [[vk::combinedImageSampler]] SamplerState g_mapSampler;

float4 main(VSOutput input) : SV_Target0
{
    return g_mapTexture.Sample(g_mapSampler, input.uv);
}
)";
} // namespace

math::Double3 EquirectDirectionFromUv(const math::Double2 uv) noexcept
{
    const f64 longitude = uv.x * 2.0 * std::numbers::pi - std::numbers::pi;
    const f64 latitude =
        std::numbers::pi * 0.5 - uv.y * std::numbers::pi;

    const f64 cosLatitude = std::cos(latitude);

    return {
        cosLatitude * std::sin(longitude),
        std::sin(latitude),
        cosLatitude * std::cos(longitude)
    };
}

class PlanetMapRenderer::Impl
{
public:
    Impl(
        rhi::Device& device,
        const shader::Compiler& shaderCompiler,
        rhi::Queue& graphicsQueue,
        std::shared_ptr<const terrain::AnalyticTerrainSource> terrain)
        : device_(device),
          graphicsQueue_(graphicsQueue),
          terrain_(std::move(terrain))
    {
        const shader::Binary vertexShader = shaderCompiler.Compile(
            {.source = kVertexShader,
             .entryPoint = "main",
             .stage = shader::Stage::Vertex,
             .debug = false});

        const shader::Binary pixelShader = shaderCompiler.Compile(
            {.source = kPixelShader,
             .entryPoint = "main",
             .stage = shader::Stage::Pixel,
             .debug = false});

        pipeline_ = device_.CreateGraphicsPipeline({
            .vertexShader =
                {.data = vertexShader.bytecode.data(),
                 .size = vertexShader.bytecode.size()},
            .pixelShader =
                {.data = pixelShader.bytecode.data(),
                 .size = pixelShader.bytecode.size()},
            .vertexAttributes = {},
            .vertexStrideBytes = 0,
            .pushConstantDwords = 0,
            .shaderResourceBuffers = 0,
            .sampledTextures = 1,
            .topology = rhi::PrimitiveTopology::TriangleList,
            .fillMode = rhi::FillMode::Solid,
            .cullMode = rhi::CullMode::None,
            .depthTest = false,
            .depthWrite = false
        });

        uploadAllocator_ =
            device_.CreateCommandAllocator(rhi::QueueType::Graphics);
        uploadCommandList_ = device_.CreateCommandList(*uploadAllocator_);
        uploadFence_ = device_.CreateFence(0);

        const terrain::AnalyticTerrainSource* terrainPointer = terrain_.get();
        generationFuture_ = std::async(
            std::launch::async,
            [terrainPointer]() { return GenerateLayers(*terrainPointer); });
    }

    void Poll()
    {
        if (uploaded_ || !generationFuture_.valid())
        {
            return;
        }

        if (generationFuture_.wait_for(std::chrono::seconds(0)) !=
            std::future_status::ready)
        {
            return;
        }

        const LayerBuffers buffers = generationFuture_.get();
        UploadLayers(buffers);
        uploaded_ = true;
    }

    [[nodiscard]] bool Ready() const noexcept
    {
        return uploaded_;
    }

    void SetActiveLayer(const MapLayer layer) noexcept
    {
        activeLayer_ = layer;
    }

    [[nodiscard]] MapLayer ActiveLayer() const noexcept
    {
        return activeLayer_;
    }

    void CycleLayer(const bool forward) noexcept
    {
        const u32 count = kMapLayerCount;
        const u32 current = static_cast<u32>(activeLayer_);
        const u32 next =
            forward ? (current + 1) % count : (current + count - 1) % count;
        activeLayer_ = static_cast<MapLayer>(next);
    }

    void Draw(
        rhi::CommandList& commandList,
        rhi::Texture& target,
        const u32 targetWidth,
        const u32 targetHeight)
    {
        if (!uploaded_ || targetWidth == 0 || targetHeight == 0)
        {
            return;
        }

        commandList.ClearColorTarget(target, {0.0F, 0.0F, 0.0F, 1.0F});
        commandList.SetRenderTarget(target);

        commandList.SetViewport({
            .x = 0.0F,
            .y = 0.0F,
            .width = static_cast<f32>(targetWidth),
            .height = static_cast<f32>(targetHeight),
            .minDepth = 0.0F,
            .maxDepth = 1.0F
        });

        commandList.SetScissor({
            .left = 0,
            .top = 0,
            .right = static_cast<i32>(targetWidth),
            .bottom = static_cast<i32>(targetHeight)
        });

        commandList.SetGraphicsPipeline(*pipeline_);
        commandList.SetGraphicsTexture(
            0, *layerTextures_[static_cast<u32>(activeLayer_)]);
        commandList.Draw(6);
    }

private:
    void UploadLayers(const LayerBuffers& buffers)
    {
        constexpr u64 kBytesPerLayer =
            static_cast<u64>(kMapWidth) * kMapHeight * 4U;

        std::vector<std::unique_ptr<rhi::Buffer>> stagingBuffers;
        stagingBuffers.reserve(kMapLayerCount);

        uploadAllocator_->Reset();
        uploadCommandList_->Reset(*uploadAllocator_);

        for (u32 layer = 0; layer < kMapLayerCount; ++layer)
        {
            layerTextures_[layer] = device_.CreateTexture({
                .width = kMapWidth,
                .height = kMapHeight,
                .format = rhi::TextureFormat::RGBA8_UNorm,
                .initialState = rhi::ResourceState::Common
            });

            auto staging = device_.CreateBuffer({
                .sizeBytes = kBytesPerLayer,
                .usage = rhi::BufferUsage::Generic,
                .memory = rhi::MemoryUsage::HostVisible,
                .initialState = rhi::ResourceState::Common
            });

            std::byte* mapped = staging->Map();
            std::memcpy(
                mapped, buffers[layer].data(), static_cast<std::size_t>(kBytesPerLayer));
            staging->Unmap();

            uploadCommandList_->Transition(
                *layerTextures_[layer],
                rhi::ResourceState::Common,
                rhi::ResourceState::CopyDestination);

            uploadCommandList_->CopyBufferToTexture(
                *staging, 0, *layerTextures_[layer]);

            uploadCommandList_->Transition(
                *layerTextures_[layer],
                rhi::ResourceState::CopyDestination,
                rhi::ResourceState::ShaderResource);

            stagingBuffers.push_back(std::move(staging));
        }

        uploadCommandList_->Close();
        graphicsQueue_.Submit(*uploadCommandList_);
        graphicsQueue_.Signal(*uploadFence_, 1);
        uploadFence_->Wait(1);
        // stagingBuffers freed here, safe: the fence wait above already
        // guarantees the GPU copy that reads them has completed.
    }

    rhi::Device& device_;
    rhi::Queue& graphicsQueue_;
    std::shared_ptr<const terrain::AnalyticTerrainSource> terrain_;

    std::unique_ptr<rhi::GraphicsPipeline> pipeline_;
    std::array<std::unique_ptr<rhi::Texture>, kMapLayerCount> layerTextures_;
    MapLayer activeLayer_{MapLayer::Elevation};

    std::unique_ptr<rhi::CommandAllocator> uploadAllocator_;
    std::unique_ptr<rhi::CommandList> uploadCommandList_;
    std::unique_ptr<rhi::Fence> uploadFence_;

    std::future<LayerBuffers> generationFuture_;
    bool uploaded_{false};
};

PlanetMapRenderer::PlanetMapRenderer(
    rhi::Device& device,
    const shader::Compiler& shaderCompiler,
    rhi::Queue& graphicsQueue,
    std::shared_ptr<const terrain::AnalyticTerrainSource> terrain)
    : impl_(std::make_unique<Impl>(
          device, shaderCompiler, graphicsQueue, std::move(terrain)))
{
}

PlanetMapRenderer::~PlanetMapRenderer() = default;

void PlanetMapRenderer::Poll()
{
    impl_->Poll();
}

bool PlanetMapRenderer::Ready() const noexcept
{
    return impl_->Ready();
}

void PlanetMapRenderer::SetActiveLayer(const MapLayer layer) noexcept
{
    impl_->SetActiveLayer(layer);
}

MapLayer PlanetMapRenderer::ActiveLayer() const noexcept
{
    return impl_->ActiveLayer();
}

void PlanetMapRenderer::CycleLayer(const bool forward) noexcept
{
    impl_->CycleLayer(forward);
}

void PlanetMapRenderer::Draw(
    rhi::CommandList& commandList,
    rhi::Texture& target,
    const u32 targetWidth,
    const u32 targetHeight)
{
    impl_->Draw(commandList, target, targetWidth, targetHeight);
}
} // namespace orbit::map_render
