#include <orbit/terrain_render/TerrainPreviewRenderer.hpp>

#include <orbit/math/Matrix.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/terrain_view/ClipmapTracker.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace orbit::terrain_render
{
namespace
{
struct TerrainVertex
{
    f32 x{};
    f32 y{};
    f32 z{};
    f32 elevation{};
};

[[nodiscard]] math::Double3 Blend(
    const math::Double3& a,
    const math::Double3& b,
    const f64 t) noexcept
{
    return a * (1.0 - t) + b * t;
}

[[nodiscard]] math::Double3 SampleSurfacePosition(
    const world::PlanetDefinition& planet,
    const terrain::TerrainSource& terrainSource,
    const world::SurfaceFrame& frame,
    const math::Double2& offsetMeters,
    const f64 footprintMeters)
{
    const math::Double3 direction =
        world::DirectionAtSurfaceOffset(
            planet,
            frame,
            offsetMeters);

    const terrain::TerrainSample sample =
        terrainSource.Sample({
            .unitDirection = direction,
            .footprintMeters = footprintMeters
        });

    return direction *
        (planet.radiusMeters + sample.elevationMeters);
}

[[nodiscard]] TerrainVertex MakeVertex(
    const world::PlanetDefinition& planet,
    const terrain::TerrainSource& terrainSource,
    const world::WorldPosition& observer,
    const world::SurfaceFrame& observerFrame,
    const terrain_view::ClipmapLevel& level,
    const world::SurfaceFrame& levelFrame,
    const terrain_view::ClipmapLevel* coarserLevel,
    const math::Double2& offsetMeters)
{
    const math::Double3 finePosition =
        SampleSurfacePosition(
            planet,
            terrainSource,
            levelFrame,
            offsetMeters,
            level.terrainFootprintMeters);

    math::Double3 worldPosition = finePosition;

    const f64 edgeDistance =
        std::max(
            std::abs(offsetMeters.x),
            std::abs(offsetMeters.y));

    const f64 morph =
        coarserLevel != nullptr
            ? terrain_view::LodMorphFactor(
                level,
                edgeDistance)
            : 0.0;

    if (morph > 0.0 &&
        coarserLevel != nullptr)
    {
        const f64 coarseSpacing =
            coarserLevel->sampleSpacingMeters;

        const math::Double2 snappedOffset{
            std::round(offsetMeters.x / coarseSpacing) *
                coarseSpacing,
            std::round(offsetMeters.y / coarseSpacing) *
                coarseSpacing
        };

        const math::Double3 coarsePosition =
            SampleSurfacePosition(
                planet,
                terrainSource,
                levelFrame,
                snappedOffset,
                coarserLevel->terrainFootprintMeters);

        worldPosition =
            Blend(finePosition, coarsePosition, morph);
    }

    const math::Double3 delta =
        worldPosition - observer.meters;

    const f64 elevation =
        math::Length(worldPosition) -
        planet.radiusMeters;

    return {
        .x = static_cast<f32>(
            math::Dot(delta, observerFrame.east)),
        .y = static_cast<f32>(
            math::Dot(delta, observerFrame.up)),
        .z = static_cast<f32>(
            math::Dot(delta, observerFrame.north)),
        .elevation = static_cast<f32>(elevation)
    };
}

void AppendLevelIndices(
    const terrain_view::ClipmapLevel& level,
    const u32 vertexBase,
    std::vector<u32>& indices)
{
    const u32 resolution = level.gridResolution;
    const u32 cells = resolution - 1U;
    const f64 halfCells =
        static_cast<f64>(cells) * 0.5;

    for (u32 y = 0; y < cells; ++y)
    {
        const f64 centerY =
            (static_cast<f64>(y) + 0.5 - halfCells) *
            level.sampleSpacingMeters;

        for (u32 x = 0; x < cells; ++x)
        {
            const f64 centerX =
                (static_cast<f64>(x) + 0.5 - halfCells) *
                level.sampleSpacingMeters;

            const bool insideHole =
                level.index > 0 &&
                std::abs(centerX) <
                    level.innerHoleHalfExtentMeters &&
                std::abs(centerY) <
                    level.innerHoleHalfExtentMeters;

            if (insideHole)
            {
                continue;
            }

            const u32 row0 = vertexBase + y * resolution;
            const u32 row1 =
                vertexBase + (y + 1U) * resolution;

            const u32 v00 = row0 + x;
            const u32 v10 = row0 + x + 1U;
            const u32 v01 = row1 + x;
            const u32 v11 = row1 + x + 1U;

            indices.push_back(v00);
            indices.push_back(v01);
            indices.push_back(v10);

            indices.push_back(v10);
            indices.push_back(v01);
            indices.push_back(v11);
        }
    }
}

[[nodiscard]] std::array<u32, 16> MatrixConstants(
    const math::Mat4& matrix) noexcept
{
    std::array<u32, 16> result{};
    static_assert(
        sizeof(result) == sizeof(matrix.values));

    std::memcpy(
        result.data(),
        matrix.values.data(),
        sizeof(result));

    return result;
}

constexpr const char* kVertexShader = R"(
cbuffer DrawConstants : register(b0)
{
    row_major float4x4 g_mvp;
};

struct VSInput
{
    float4 data : TEXCOORD0;
};

struct VSOutput
{
    float4 position : SV_Position;
    float elevation : TEXCOORD0;
};

VSOutput main(VSInput input)
{
    VSOutput output;
    output.position = mul(float4(input.data.xyz, 1.0), g_mvp);
    output.elevation = input.data.w;
    return output;
}
)";

constexpr const char* kPixelShader = R"(
struct VSOutput
{
    float4 position : SV_Position;
    float elevation : TEXCOORD0;
};

float4 main(VSOutput input) : SV_Target0
{
    float normalizedHeight =
        saturate(input.elevation / 6000.0 + 0.35);

    float3 lowColor = float3(0.08, 0.18, 0.12);
    float3 highColor = float3(0.72, 0.78, 0.72);

    return float4(
        lerp(lowColor, highColor, normalizedHeight),
        1.0);
}
)";
} // namespace

class TerrainPreviewRenderer::Impl
{
public:
    Impl(
        rhi::Device& device,
        const shader::Compiler& shaderCompiler,
        const world::PlanetDefinition& planet,
        const terrain::TerrainSource& terrainSource,
        const world::WorldPosition& observer,
        TerrainPreviewConfig config)
        : config_(std::move(config))
    {
        if (math::Length(observer.meters) <= planet.radiusMeters)
        {
            throw std::invalid_argument(
                "Orbit terrain preview observer must be above the planet surface.");
        }

        const terrain_view::ClipmapLayout layout =
            terrain_view::BuildClipmapLayout(
                config_.clipmap,
                observer);

        terrain_view::ClipmapTracker tracker(
            planet,
            config_.clipmap);

        const terrain_view::ClipmapMotionUpdate motion =
            tracker.Update(observer);

        const world::SurfaceFrame observerFrame =
            world::MakeSurfaceFrame(observer.meters);

        std::vector<TerrainVertex> vertices;
        std::vector<u32> indices;

        std::size_t totalVertexCount = 0;

        for (const auto& level : layout.levels)
        {
            const std::size_t resolution =
                static_cast<std::size_t>(
                    level.gridResolution);

            totalVertexCount +=
                resolution * resolution;
        }

        if (totalVertexCount >
            static_cast<std::size_t>(
                std::numeric_limits<u32>::max()))
        {
            throw std::overflow_error(
                "Orbit terrain preview vertex count exceeds 32-bit indexing.");
        }

        vertices.reserve(totalVertexCount);

        for (u32 levelIndex = 0;
             levelIndex <
                static_cast<u32>(layout.levels.size());
             ++levelIndex)
        {
            const terrain_view::ClipmapLevel& level =
                layout.levels[levelIndex];

            const terrain_view::ClipmapLevel* coarserLevel =
                levelIndex + 1U <
                    static_cast<u32>(layout.levels.size())
                    ? &layout.levels[levelIndex + 1U]
                    : nullptr;

            const u32 vertexBase =
                static_cast<u32>(vertices.size());

            const u32 cells =
                level.gridResolution - 1U;

            const f64 halfCells =
                static_cast<f64>(cells) * 0.5;

            for (u32 y = 0;
                 y < level.gridResolution;
                 ++y)
            {
                for (u32 x = 0;
                     x < level.gridResolution;
                     ++x)
                {
                    const math::Double2 offsetMeters{
                        (static_cast<f64>(x) - halfCells) *
                            level.sampleSpacingMeters,
                        (static_cast<f64>(y) - halfCells) *
                            level.sampleSpacingMeters
                    };

                    vertices.push_back(
                        MakeVertex(
                            planet,
                            terrainSource,
                            observer,
                            observerFrame,
                            level,
                            motion.levels[levelIndex]
                                .surfaceFrame,
                            coarserLevel,
                            offsetMeters));
                }
            }

            AppendLevelIndices(
                level,
                vertexBase,
                indices);
        }

        if (indices.size() >
            static_cast<std::size_t>(
                std::numeric_limits<u32>::max()))
        {
            throw std::overflow_error(
                "Orbit terrain preview index count exceeds 32-bit draw limits.");
        }

        vertexCount_ =
            static_cast<u32>(vertices.size());

        indexCount_ =
            static_cast<u32>(indices.size());

        const u64 vertexBytes =
            static_cast<u64>(vertices.size()) *
            static_cast<u64>(sizeof(TerrainVertex));

        const u64 indexBytes =
            static_cast<u64>(indices.size()) *
            static_cast<u64>(sizeof(u32));

        vertexBuffer_ = device.CreateBuffer({
            .sizeBytes = vertexBytes,
            .usage = rhi::BufferUsage::Vertex,
            .memory = rhi::MemoryUsage::HostVisible,
            .initialState =
                rhi::ResourceState::VertexOrConstantBuffer
        });

        indexBuffer_ = device.CreateBuffer({
            .sizeBytes = indexBytes,
            .usage = rhi::BufferUsage::Index,
            .memory = rhi::MemoryUsage::HostVisible,
            .initialState =
                rhi::ResourceState::IndexBuffer
        });

        {
            std::byte* destination =
                vertexBuffer_->Map();

            std::memcpy(
                destination,
                vertices.data(),
                static_cast<std::size_t>(vertexBytes));

            vertexBuffer_->Unmap();
        }

        {
            std::byte* destination =
                indexBuffer_->Map();

            std::memcpy(
                destination,
                indices.data(),
                static_cast<std::size_t>(indexBytes));

            indexBuffer_->Unmap();
        }

        const shader::Binary vertexShader =
            shaderCompiler.Compile({
                .source = kVertexShader,
                .entryPoint = "main",
                .stage = shader::Stage::Vertex,
                .debug = false
            });

        const shader::Binary pixelShader =
            shaderCompiler.Compile({
                .source = kPixelShader,
                .entryPoint = "main",
                .stage = shader::Stage::Pixel,
                .debug = false
            });

        const std::array<rhi::VertexAttribute, 1>
            attributes{{
                {
                    .location = 0,
                    .format = rhi::VertexFormat::Float4,
                    .offsetBytes = 0
                }
            }};

        pipeline_ =
            device.CreateGraphicsPipeline({
                .vertexShader = {
                    .data = vertexShader.bytecode.data(),
                    .size = vertexShader.bytecode.size()
                },
                .pixelShader = {
                    .data = pixelShader.bytecode.data(),
                    .size = pixelShader.bytecode.size()
                },
                .vertexAttributes = attributes,
                .vertexStrideBytes =
                    static_cast<u32>(
                        sizeof(TerrainVertex)),
                .pushConstantDwords = 16,
                .topology =
                    rhi::PrimitiveTopology::TriangleList,
                .fillMode =
                    config_.wireframe
                        ? rhi::FillMode::Wireframe
                        : rhi::FillMode::Solid,
                .cullMode = rhi::CullMode::None,
                .depthTest = true,
                .depthWrite = true
            });
    }

    void Draw(
        rhi::CommandList& commandList,
        const u32 targetWidth,
        const u32 targetHeight)
    {
        if (targetWidth == 0 ||
            targetHeight == 0)
        {
            return;
        }

        const f32 aspect =
            static_cast<f32>(targetWidth) /
            static_cast<f32>(targetHeight);

        const math::Mat4 view =
            math::LookAtLH(
                {0.0F, 0.0F, 0.0F},
                {0.0F, -0.28F, 1.0F},
                {0.0F, 1.0F, 0.0F});

        const math::Mat4 projection =
            math::PerspectiveLH(
                config_.verticalFovRadians,
                aspect,
                config_.nearPlaneMeters,
                config_.farPlaneMeters);

        const math::Mat4 mvp =
            math::Multiply(view, projection);

        const auto constants =
            MatrixConstants(mvp);

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
        commandList.SetGraphicsConstants(constants);

        commandList.SetVertexBuffer(
            *vertexBuffer_,
            static_cast<u32>(
                sizeof(TerrainVertex)));

        commandList.SetIndexBuffer(
            *indexBuffer_,
            rhi::IndexFormat::UInt32);

        commandList.DrawIndexed(indexCount_);
    }

    [[nodiscard]] u32 VertexCount() const noexcept
    {
        return vertexCount_;
    }

    [[nodiscard]] u32 IndexCount() const noexcept
    {
        return indexCount_;
    }

private:
    TerrainPreviewConfig config_;
    std::unique_ptr<rhi::Buffer> vertexBuffer_;
    std::unique_ptr<rhi::Buffer> indexBuffer_;
    std::unique_ptr<rhi::GraphicsPipeline> pipeline_;
    u32 vertexCount_{0};
    u32 indexCount_{0};
};

TerrainPreviewRenderer::TerrainPreviewRenderer(
    rhi::Device& device,
    const shader::Compiler& shaderCompiler,
    const world::PlanetDefinition& planet,
    const terrain::TerrainSource& terrainSource,
    const world::WorldPosition& observer,
    TerrainPreviewConfig config)
    : impl_(std::make_unique<Impl>(
        device,
        shaderCompiler,
        planet,
        terrainSource,
        observer,
        std::move(config)))
{
}

TerrainPreviewRenderer::~TerrainPreviewRenderer() = default;

TerrainPreviewRenderer::TerrainPreviewRenderer(
    TerrainPreviewRenderer&&) noexcept = default;

TerrainPreviewRenderer&
TerrainPreviewRenderer::operator=(
    TerrainPreviewRenderer&&) noexcept = default;

void TerrainPreviewRenderer::Draw(
    rhi::CommandList& commandList,
    const u32 targetWidth,
    const u32 targetHeight)
{
    impl_->Draw(
        commandList,
        targetWidth,
        targetHeight);
}

u32 TerrainPreviewRenderer::VertexCount() const noexcept
{
    return impl_->VertexCount();
}

u32 TerrainPreviewRenderer::IndexCount() const noexcept
{
    return impl_->IndexCount();
}
} // namespace orbit::terrain_render
