#include <orbit/water_render/RiverWaterRenderer.hpp>

#include <orbit/math/Matrix.hpp>
#include <orbit/terrain_water/LakeWater.hpp>
#include <orbit/terrain_water/RiverWater.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <utility>
#include <vector>

namespace orbit::water_render
{
namespace
{
struct WaterVertex
{
    math::Float3 position{};
    math::Float2 water{};
};

static_assert(
    sizeof(WaterVertex) ==
    5U * sizeof(f32));

void UploadBuffer(
    rhi::Buffer& buffer,
    const void* source,
    const std::size_t bytes)
{
    if (bytes == 0)
    {
        return;
    }

    std::byte* destination =
        buffer.Map();

    std::memcpy(
        destination,
        source,
        bytes);

    buffer.Unmap();
}

[[nodiscard]] math::Float3 ToObserverLocal(
    const math::Double3& position,
    const world::WorldPosition& observer,
    const world::SurfaceFrame& observerFrame) noexcept
{
    const math::Double3 relative =
        position -
        observer.meters;

    return {
        static_cast<f32>(
            math::Dot(
                relative,
                observerFrame.east)),
        static_cast<f32>(
            math::Dot(
                relative,
                observerFrame.up)),
        static_cast<f32>(
            math::Dot(
                relative,
                observerFrame.north))
    };
}

[[nodiscard]] math::Double3 WaterWorldPosition(
    const world::PlanetDefinition& planet,
    const terrain_water::RiverWaterNetwork& water,
    const terrain_water::RiverWaterNode& node,
    const f64 surfaceOffsetMeters) noexcept
{
    const math::Double3 direction =
        world::DirectionAtSurfaceOffset(
            planet,
            water.surfaceFrame,
            node.offsetMeters);

    return
        direction *
        (planet.radiusMeters +
         static_cast<f64>(
             node.surfaceElevationMeters) +
         surfaceOffsetMeters);
}

[[nodiscard]] std::array<u32, 16>
BuildConstants(
    const math::Mat4& matrix) noexcept
{
    std::array<u32, 16> result{};

    static_assert(
        sizeof(matrix.values) ==
        sizeof(result));

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
    float3 position : TEXCOORD0;
    float2 water : TEXCOORD1;
};

struct VSOutput
{
    float4 position : SV_Position;
    float lateral : TEXCOORD0;
    float velocity : TEXCOORD1;
};

VSOutput main(VSInput input)
{
    VSOutput output;

    output.position =
        mul(
            float4(
                input.position,
                1.0),
            g_mvp);

    output.lateral =
        input.water.x;

    output.velocity =
        input.water.y;

    return output;
}
)";

constexpr const char* kPixelShader = R"(
struct VSOutput
{
    float4 position : SV_Position;
    float lateral : TEXCOORD0;
    float velocity : TEXCOORD1;
};

float4 main(VSOutput input) : SV_Target0
{
    const float edge =
        abs(
            input.lateral *
                2.0 -
            1.0);

    const float center =
        1.0 -
        saturate(edge);

    const float flow =
        saturate(
            input.velocity /
            6.0);

    const float3 deepWater =
        float3(
            0.018,
            0.095,
            0.16);

    const float3 movingWater =
        float3(
            0.035,
            0.24,
            0.33);

    float3 color =
        lerp(
            deepWater,
            movingWater,
            0.30 +
            flow *
                0.55);

    color *=
        0.82 +
        center *
            0.22;

    return float4(
        color,
        1.0);
}
)";
} // namespace

class RiverWaterRenderer::Impl
{
public:
    Impl(
        rhi::Device& device,
        const shader::Compiler& shaderCompiler,
        const world::PlanetDefinition planet,
        std::shared_ptr<
            terrain_region::DerivedTerrainRegionCache> regionCache,
        const world::WorldPosition& observer,
        const RiverWaterRendererConfig config)
        : device_(device),
          planet_(planet),
          regionCache_(std::move(regionCache)),
          config_(config)
    {
        if (!regionCache_)
        {
            throw std::invalid_argument(
                "Orbit river water renderer requires a derived region cache.");
        }

        if (planet_.radiusMeters <= 0.0 ||
            config_.framesInFlight == 0 ||
            (config_.maximumSegments == 0 &&
             config_.maximumLakeCells == 0) ||
            config_.nearPlaneMeters <= 0.0F ||
            config_.farPlaneMeters <=
                config_.nearPlaneMeters ||
            config_.maximumDrawDistanceMeters <=
                0.0 ||
            config_.surfaceOffsetMeters < 0.0)
        {
            throw std::invalid_argument(
                "Orbit river water renderer configuration is invalid.");
        }

        SetObserver(observer);
        CreatePipeline(shaderCompiler);
        CreateBuffers();

        scratchVertices_.reserve(
            (static_cast<std::size_t>(
                 config_.maximumSegments) +
             static_cast<std::size_t>(
                 config_.maximumLakeCells)) *
            4U);
    }

    void UpdateObserver(
        const world::WorldPosition& observer)
    {
        SetObserver(observer);
    }

    void Draw(
        rhi::CommandList& commandList,
        rhi::Texture& colorTarget,
        rhi::Texture& depthTarget,
        const u32 frameIndex,
        const u32 targetWidth,
        const u32 targetHeight,
        const RiverWaterCamera& camera)
    {
        stats_ = {};

        if (frameIndex >=
            config_.framesInFlight)
        {
            throw std::out_of_range(
                "Orbit river water frame index exceeds configured frames in flight.");
        }

        if (targetWidth == 0 ||
            targetHeight == 0)
        {
            return;
        }

        BuildVisibleVertices();

        if (scratchVertices_.empty())
        {
            return;
        }

        const std::size_t bytes =
            scratchVertices_.size() *
            sizeof(WaterVertex);

        UploadBuffer(
            *frameVertexBuffers_[
                frameIndex],
            scratchVertices_.data(),
            bytes);

        stats_.uploadedBytesLastFrame =
            static_cast<u64>(bytes);

        math::Float3 cameraForward =
            math::Normalize(
                camera.forward);

        if (math::LengthSquared(
                cameraForward) <=
            1.0e-8F)
        {
            cameraForward =
                math::Normalize(
                    math::Float3{
                        0.0F,
                        -0.28F,
                        1.0F
                    });
        }

        math::Float3 cameraUp =
            math::Normalize(
                camera.up);

        if (math::LengthSquared(
                cameraUp) <=
                1.0e-8F ||
            math::LengthSquared(
                math::Cross(
                    cameraUp,
                    cameraForward)) <=
                1.0e-8F)
        {
            cameraUp = {
                0.0F,
                1.0F,
                0.0F
            };
        }

        const f32 aspect =
            static_cast<f32>(
                targetWidth) /
            static_cast<f32>(
                targetHeight);

        const math::Mat4 view =
            math::LookAtLH(
                {0.0F, 0.0F, 0.0F},
                cameraForward,
                cameraUp);

        const math::Mat4 projection =
            math::PerspectiveReverseZLH(
                config_.verticalFovRadians,
                aspect,
                config_.nearPlaneMeters,
                config_.farPlaneMeters);

        const auto constants =
            BuildConstants(
                math::Multiply(
                    view,
                    projection));

        commandList.SetRenderTargets(
            colorTarget,
            depthTarget);

        commandList.SetViewport({
            .x = 0.0F,
            .y = 0.0F,
            .width =
                static_cast<f32>(
                    targetWidth),
            .height =
                static_cast<f32>(
                    targetHeight),
            .minDepth = 0.0F,
            .maxDepth = 1.0F
        });

        commandList.SetScissor({
            .left = 0,
            .top = 0,
            .right =
                static_cast<i32>(
                    targetWidth),
            .bottom =
                static_cast<i32>(
                    targetHeight)
        });

        commandList.SetGraphicsPipeline(
            *pipeline_);

        commandList.SetGraphicsConstants(
            constants);

        commandList.SetVertexBuffer(
            *frameVertexBuffers_[
                frameIndex],
            sizeof(WaterVertex));

        commandList.SetIndexBuffer(
            *indexBuffer_,
            rhi::IndexFormat::UInt32);

        commandList.DrawIndexed(
            (stats_.
                 visibleSegmentsLastFrame +
             stats_.
                 visibleLakeCellsLastFrame) *
            6U);

        stats_.drawCallsLastFrame = 1;
    }

    [[nodiscard]] const RiverWaterRenderStats&
    Stats() const noexcept
    {
        return stats_;
    }

private:
    void SetObserver(
        const world::WorldPosition& observer)
    {
        if (math::Length(
                observer.meters) <=
            planet_.radiusMeters)
        {
            throw std::invalid_argument(
                "Orbit river water observer must be above the planet surface.");
        }

        observer_ = observer;

        if (!observerFrameInitialized_)
        {
            observerFrame_ =
                world::MakeSurfaceFrame(
                    observer.meters);

            observerFrameInitialized_ =
                true;
        }
        else
        {
            observerFrame_ =
                world::
                    TransportSurfaceFrameToDirection(
                        observerFrame_,
                        observer.meters);
        }
    }

    void CreatePipeline(
        const shader::Compiler& shaderCompiler)
    {
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

        constexpr std::array<
            rhi::VertexAttribute,
            2>
            attributes{{
                {
                    .location = 0,
                    .format =
                        rhi::VertexFormat::Float3,
                    .offsetBytes = 0
                },
                {
                    .location = 1,
                    .format =
                        rhi::VertexFormat::Float2,
                    .offsetBytes =
                        3U * sizeof(f32)
                }
            }};

        pipeline_ =
            device_.CreateGraphicsPipeline({
                .vertexShader = {
                    .data =
                        vertexShader.bytecode.data(),
                    .size =
                        vertexShader.bytecode.size()
                },
                .pixelShader = {
                    .data =
                        pixelShader.bytecode.data(),
                    .size =
                        pixelShader.bytecode.size()
                },
                .vertexAttributes =
                    attributes,
                .vertexStrideBytes =
                    sizeof(WaterVertex),
                .pushConstantDwords = 16,
                .shaderResourceBuffers = 0,
                .topology =
                    rhi::PrimitiveTopology::
                        TriangleList,
                .fillMode =
                    rhi::FillMode::Solid,
                .cullMode =
                    rhi::CullMode::None,
                .depthCompare =
                    rhi::DepthCompare::GreaterEqual,
                .depthTest = true,
                .depthWrite = false
            });
    }

    void CreateBuffers()
    {
        const u64 maximumQuads =
            static_cast<u64>(
                config_.maximumSegments) +
            static_cast<u64>(
                config_.maximumLakeCells);

        const u64 vertexBytes =
            maximumQuads *
            4ULL *
            sizeof(WaterVertex);

        frameVertexBuffers_.reserve(
            config_.framesInFlight);

        for (u32 frameIndex = 0;
             frameIndex <
                config_.framesInFlight;
             ++frameIndex)
        {
            frameVertexBuffers_.push_back(
                device_.CreateBuffer({
                    .sizeBytes =
                        vertexBytes,
                    .usage =
                        rhi::BufferUsage::Vertex,
                    .memory =
                        rhi::MemoryUsage::
                            HostVisible,
                    .initialState =
                        rhi::ResourceState::
                            VertexOrConstantBuffer
                }));
        }

        std::vector<u32> indices;
        indices.resize(
            static_cast<std::size_t>(
                maximumQuads) *
            6U);

        for (u32 quad = 0;
             quad <
                static_cast<u32>(
                    maximumQuads);
             ++quad)
        {
            const u32 vertex =
                quad * 4U;

            const std::size_t index =
                static_cast<std::size_t>(
                    quad) *
                6U;

            indices[index + 0U] =
                vertex + 0U;
            indices[index + 1U] =
                vertex + 2U;
            indices[index + 2U] =
                vertex + 1U;
            indices[index + 3U] =
                vertex + 1U;
            indices[index + 4U] =
                vertex + 2U;
            indices[index + 5U] =
                vertex + 3U;
        }

        indexBuffer_ =
            device_.CreateBuffer({
                .sizeBytes =
                    static_cast<u64>(
                        indices.size()) *
                    sizeof(u32),
                .usage =
                    rhi::BufferUsage::Index,
                .memory =
                    rhi::MemoryUsage::
                        HostVisible,
                .initialState =
                    rhi::ResourceState::
                        IndexBuffer
            });

        UploadBuffer(
            *indexBuffer_,
            indices.data(),
            indices.size() *
                sizeof(u32));
    }

    void BuildVisibleVertices()
    {
        scratchVertices_.clear();

        const f64 observerRadiusMeters =
            math::Length(
                observer_.meters);

        const f64 horizonCosine =
            std::clamp(
                planet_.radiusMeters /
                    observerRadiusMeters,
                0.0,
                1.0);

        const auto aboveHorizon =
            [this, horizonCosine](
                const math::Double3& worldPosition)
            {
                const math::Double3 direction =
                    math::Normalize(
                        worldPosition);

                if (math::LengthSquared(
                        direction) <=
                    1.0e-12)
                {
                    return false;
                }

                return
                    math::Dot(
                        direction,
                        observerFrame_.up) +
                        0.000002 >=
                    horizonCosine;
            };

        const auto regions =
            regionCache_->
                ReadyRegionsSnapshot();

        if (!regions)
        {
            return;
        }

        stats_.readyRegionsLastFrame =
            static_cast<u32>(
                regions->size());

        for (const auto& region :
             *regions)
        {
            if (!region)
            {
                continue;
            }

            const auto currentId =
                regionCache_->
                    IdForTile(
                        region->id.tile);

            if (currentId.sourceRevision !=
                    region->id.sourceRevision ||
                currentId.generatorVersion !=
                    region->id.generatorVersion)
            {
                continue;
            }

            const auto& water =
                region->water;

            for (const auto& segment :
                 water.segments)
            {
                if (stats_.
                        visibleSegmentsLastFrame >=
                    config_.maximumSegments)
                {
                    ++stats_.
                        truncatedSegmentsLastFrame;
                    continue;
                }

                if (segment.upstreamNode >=
                        water.nodes.size() ||
                    segment.downstreamNode >=
                        water.nodes.size())
                {
                    continue;
                }

                const auto& upstream =
                    water.nodes[
                        segment.upstreamNode];

                const auto& downstream =
                    water.nodes[
                        segment.downstreamNode];

                const math::Double3 upstreamWorld =
                    WaterWorldPosition(
                        planet_,
                        water,
                        upstream,
                        config_.
                            surfaceOffsetMeters);

                const math::Double3 downstreamWorld =
                    WaterWorldPosition(
                        planet_,
                        water,
                        downstream,
                        config_.
                            surfaceOffsetMeters);

                const math::Double3 midpoint =
                    (upstreamWorld +
                     downstreamWorld) *
                    0.5;

                if (math::Length(
                        midpoint -
                        observer_.meters) >
                    config_.
                        maximumDrawDistanceMeters ||
                    !aboveHorizon(
                        midpoint))
                {
                    continue;
                }

                const math::Double3 segmentDirection =
                    math::Normalize(
                        downstreamWorld -
                        upstreamWorld);

                math::Double3 surfaceNormal =
                    math::Normalize(
                        upstreamWorld +
                        downstreamWorld);

                if (math::LengthSquared(
                        surfaceNormal) <=
                    1.0e-12)
                {
                    surfaceNormal =
                        math::Normalize(
                            upstreamWorld);
                }

                math::Double3 side =
                    math::Normalize(
                        math::Cross(
                            surfaceNormal,
                            segmentDirection));

                if (math::LengthSquared(
                        side) <=
                    1.0e-12)
                {
                    continue;
                }

                const math::Double3 upstreamLeft =
                    upstreamWorld -
                    side *
                        static_cast<f64>(
                            upstream.
                                halfWidthMeters);

                const math::Double3 upstreamRight =
                    upstreamWorld +
                    side *
                        static_cast<f64>(
                            upstream.
                                halfWidthMeters);

                const math::Double3 downstreamLeft =
                    downstreamWorld -
                    side *
                        static_cast<f64>(
                            downstream.
                                halfWidthMeters);

                const math::Double3 downstreamRight =
                    downstreamWorld +
                    side *
                        static_cast<f64>(
                            downstream.
                                halfWidthMeters);

                const f32 velocity =
                    segment.
                        velocityMetersPerSecond;

                scratchVertices_.push_back({
                    .position =
                        ToObserverLocal(
                            upstreamLeft,
                            observer_,
                            observerFrame_),
                    .water = {
                        0.0F,
                        velocity
                    }
                });

                scratchVertices_.push_back({
                    .position =
                        ToObserverLocal(
                            upstreamRight,
                            observer_,
                            observerFrame_),
                    .water = {
                        1.0F,
                        velocity
                    }
                });

                scratchVertices_.push_back({
                    .position =
                        ToObserverLocal(
                            downstreamLeft,
                            observer_,
                            observerFrame_),
                    .water = {
                        0.0F,
                        velocity
                    }
                });

                scratchVertices_.push_back({
                    .position =
                        ToObserverLocal(
                            downstreamRight,
                            observer_,
                            observerFrame_),
                    .water = {
                        1.0F,
                        velocity
                    }
                });

                ++stats_.
                    visibleSegmentsLastFrame;
            }

            const auto& lakes =
                region->lakes;

            for (const auto& cell :
                 lakes.cells)
            {
                if (stats_.
                        visibleLakeCellsLastFrame >=
                    config_.maximumLakeCells)
                {
                    ++stats_.
                        truncatedLakeCellsLastFrame;
                    continue;
                }

                const f64 halfCell =
                    lakes.cellSpacingMeters *
                    0.5;

                const math::Double3 centerDirection =
                    world::DirectionAtSurfaceOffset(
                        planet_,
                        lakes.surfaceFrame,
                        cell.offsetMeters);

                const math::Double3 centerWorld =
                    centerDirection *
                    (planet_.radiusMeters +
                     static_cast<f64>(
                         cell.
                             surfaceElevationMeters) +
                     config_.
                         surfaceOffsetMeters);

                if (math::Length(
                        centerWorld -
                        observer_.meters) >
                    config_.
                        maximumDrawDistanceMeters ||
                    !aboveHorizon(
                        centerWorld))
                {
                    continue;
                }

                const std::array<
                    math::Double2,
                    4>
                    offsets{{
                        {
                            cell.offsetMeters.x -
                                halfCell,
                            cell.offsetMeters.y -
                                halfCell
                        },
                        {
                            cell.offsetMeters.x +
                                halfCell,
                            cell.offsetMeters.y -
                                halfCell
                        },
                        {
                            cell.offsetMeters.x -
                                halfCell,
                            cell.offsetMeters.y +
                                halfCell
                        },
                        {
                            cell.offsetMeters.x +
                                halfCell,
                            cell.offsetMeters.y +
                                halfCell
                        }
                    }};

                for (const auto& offset :
                     offsets)
                {
                    const math::Double3 direction =
                        world::DirectionAtSurfaceOffset(
                            planet_,
                            lakes.surfaceFrame,
                            offset);

                    const math::Double3 worldPosition =
                        direction *
                        (planet_.radiusMeters +
                         static_cast<f64>(
                             cell.
                                 surfaceElevationMeters) +
                         config_.
                             surfaceOffsetMeters);

                    scratchVertices_.push_back({
                        .position =
                            ToObserverLocal(
                                worldPosition,
                                observer_,
                                observerFrame_),
                        .water = {
                            0.5F,
                            0.0F
                        }
                    });
                }

                ++stats_.
                    visibleLakeCellsLastFrame;
            }
        }
    }

    rhi::Device& device_;
    world::PlanetDefinition planet_{};

    std::shared_ptr<
        terrain_region::
            DerivedTerrainRegionCache>
        regionCache_;

    RiverWaterRendererConfig config_{};

    world::WorldPosition observer_{};
    world::SurfaceFrame observerFrame_{};
    bool observerFrameInitialized_{false};

    std::unique_ptr<rhi::GraphicsPipeline>
        pipeline_;

    std::vector<
        std::unique_ptr<rhi::Buffer>>
        frameVertexBuffers_;

    std::unique_ptr<rhi::Buffer>
        indexBuffer_;

    std::vector<WaterVertex>
        scratchVertices_;

    RiverWaterRenderStats stats_{};
};

RiverWaterRenderer::RiverWaterRenderer(
    rhi::Device& device,
    const shader::Compiler& shaderCompiler,
    const world::PlanetDefinition planet,
    std::shared_ptr<
        terrain_region::DerivedTerrainRegionCache> regionCache,
    const world::WorldPosition& observer,
    const RiverWaterRendererConfig config)
    : impl_(
        std::make_unique<Impl>(
            device,
            shaderCompiler,
            planet,
            std::move(regionCache),
            observer,
            config))
{
}

RiverWaterRenderer::~RiverWaterRenderer() =
    default;

void RiverWaterRenderer::UpdateObserver(
    const world::WorldPosition& observer)
{
    impl_->UpdateObserver(observer);
}

void RiverWaterRenderer::Draw(
    rhi::CommandList& commandList,
    rhi::Texture& colorTarget,
    rhi::Texture& depthTarget,
    const u32 frameIndex,
    const u32 targetWidth,
    const u32 targetHeight,
    const RiverWaterCamera& camera)
{
    impl_->Draw(
        commandList,
        colorTarget,
        depthTarget,
        frameIndex,
        targetWidth,
        targetHeight,
        camera);
}

const RiverWaterRenderStats&
RiverWaterRenderer::Stats() const noexcept
{
    return impl_->Stats();
}
} // namespace orbit::water_render
