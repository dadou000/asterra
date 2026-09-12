#include <orbit/terrain_render/TerrainPreviewRenderer.hpp>

#include <orbit/math/Matrix.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/terrain_stream/ToroidalResidency.hpp>
#include <orbit/terrain_view/ClipmapTracker.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <deque>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace orbit::terrain_render
{
namespace
{
void AppendCell(
    std::vector<u32>& indices,
    const u32 resolution,
    const u32 x,
    const u32 y)
{
    const u32 row0 =
        y * resolution;

    const u32 row1 =
        (y + 1U) * resolution;

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

[[nodiscard]] std::vector<u32> BuildCenterIndices(
    const u32 resolution)
{
    std::vector<u32> indices;

    const u32 cells =
        resolution - 1U;

    indices.reserve(
        static_cast<std::size_t>(cells) *
        static_cast<std::size_t>(cells) *
        6U);

    for (u32 y = 0; y < cells; ++y)
    {
        for (u32 x = 0; x < cells; ++x)
        {
            AppendCell(
                indices,
                resolution,
                x,
                y);
        }
    }

    return indices;
}

[[nodiscard]] std::vector<u32> BuildRingIndices(
    const terrain_view::ClipmapLevel& ringLevel)
{
    std::vector<u32> indices;

    const u32 resolution =
        ringLevel.gridResolution;

    const u32 cells =
        resolution - 1U;

    const f64 halfCells =
        static_cast<f64>(cells) * 0.5;

    indices.reserve(
        static_cast<std::size_t>(cells) *
        static_cast<std::size_t>(cells) *
        6U);

    for (u32 y = 0; y < cells; ++y)
    {
        const f64 centerY =
            (static_cast<f64>(y) +
             0.5 -
             halfCells) *
            ringLevel.sampleSpacingMeters;

        for (u32 x = 0; x < cells; ++x)
        {
            const f64 centerX =
                (static_cast<f64>(x) +
                 0.5 -
                 halfCells) *
                ringLevel.sampleSpacingMeters;

            const bool insideHole =
                std::abs(centerX) <
                    ringLevel.innerHoleHalfExtentMeters &&
                std::abs(centerY) <
                    ringLevel.innerHoleHalfExtentMeters;

            if (!insideHole)
            {
                AppendCell(
                    indices,
                    resolution,
                    x,
                    y);
            }
        }
    }

    return indices;
}

void UploadBuffer(
    rhi::Buffer& buffer,
    const void* source,
    const std::size_t bytes)
{
    std::byte* destination =
        buffer.Map();

    std::memcpy(
        destination,
        source,
        bytes);

    buffer.Unmap();
}

[[nodiscard]] math::Float3 ToObserverLocal(
    const math::Double3& vector,
    const world::SurfaceFrame& observerFrame) noexcept
{
    return {
        static_cast<f32>(
            math::Dot(
                vector,
                observerFrame.east)),
        static_cast<f32>(
            math::Dot(
                vector,
                observerFrame.up)),
        static_cast<f32>(
            math::Dot(
                vector,
                observerFrame.north))
    };
}

[[nodiscard]] std::array<u32, 36> BuildDrawConstants(
    const math::Mat4& matrix,
    const f32 planetRadiusMeters,
    const f32 observerRadiusMeters,
    const terrain_view::ClipmapLevel& level,
    const terrain_view::ClipmapLevel* coarserLevel,
    const terrain_view::ClipmapLevelMotion& motion,
    const terrain_stream::LevelResidencyUpdate& residency,
    const world::SurfaceFrame& observerFrame) noexcept
{
    std::array<u32, 36> result{};

    static_assert(
        sizeof(matrix.values) ==
        16U * sizeof(u32));

    std::memcpy(
        result.data(),
        matrix.values.data(),
        sizeof(matrix.values));

    const math::Float3 centerUp =
        ToObserverLocal(
            motion.surfaceFrame.up,
            observerFrame);

    const math::Float3 centerEast =
        ToObserverLocal(
            motion.surfaceFrame.east,
            observerFrame);

    const math::Float3 centerNorth =
        ToObserverLocal(
            motion.surfaceFrame.north,
            observerFrame);

    const f32 coarseSpacing =
        coarserLevel != nullptr
            ? static_cast<f32>(
                coarserLevel->
                    sampleSpacingMeters)
            : static_cast<f32>(
                level.sampleSpacingMeters);

    const auto store =
        [&result](
            const u32 index,
            const f32 value)
        {
            result[index] =
                std::bit_cast<u32>(value);
        };

    store(16, planetRadiusMeters);
    store(17, observerRadiusMeters);
    store(
        18,
        static_cast<f32>(
            level.sampleSpacingMeters));
    store(
        19,
        static_cast<f32>(
            level.gridResolution));

    store(20, centerUp.x);
    store(21, centerUp.y);
    store(22, centerUp.z);
    store(
        23,
        static_cast<f32>(
            residency.originX));

    store(24, centerEast.x);
    store(25, centerEast.y);
    store(26, centerEast.z);
    store(
        27,
        static_cast<f32>(
            residency.originY));

    store(28, centerNorth.x);
    store(29, centerNorth.y);
    store(30, centerNorth.z);
    store(
        31,
        static_cast<f32>(
            level.morphStartHalfExtentMeters));

    store(
        32,
        static_cast<f32>(
            level.morphEndHalfExtentMeters));
    store(33, coarseSpacing);
    store(
        34,
        coarserLevel != nullptr
            ? 1.0F
            : 0.0F);
    store(35, 0.0F);

    return result;
}

constexpr const char* kVertexShader = R"(
cbuffer DrawConstants : register(b0)
{
    row_major float4x4 g_mvp;

    float4 g_planet;
    float4 g_centerUpAndOriginX;
    float4 g_centerEastAndOriginY;
    float4 g_centerNorthAndMorphStart;
    float4 g_morph;
};

ByteAddressBuffer g_samples : register(t0);

struct VSOutput
{
    float4 position : SV_Position;
    float elevation : TEXCOORD0;
};

VSOutput main(uint vertexId : SV_VertexID)
{
    const float planetRadius =
        g_planet.x;

    const float observerRadius =
        g_planet.y;

    const float spacing =
        g_planet.z;

    const uint resolution =
        (uint)round(g_planet.w);

    const uint logicalX =
        vertexId % resolution;

    const uint logicalY =
        vertexId / resolution;

    const uint originX =
        (uint)round(
            g_centerUpAndOriginX.w);

    const uint originY =
        (uint)round(
            g_centerEastAndOriginY.w);

    const uint physicalX =
        (logicalX + originX) %
        resolution;

    const uint physicalY =
        (logicalY + originY) %
        resolution;

    const uint physicalIndex =
        physicalY * resolution +
        physicalX;

    const uint sampleByteOffset =
        physicalIndex * 12;

    const float elevation =
        asfloat(
            g_samples.Load(
                sampleByteOffset));

    const float2 morphTargetOffset =
        asfloat(
            g_samples.Load2(
                sampleByteOffset + 4));

    const float halfCells =
        ((float)resolution - 1.0) *
        0.5;

    float2 offsetMeters =
        (float2(
            (float)logicalX,
            (float)logicalY) -
         halfCells) *
        spacing;

    const float morphStart =
        g_centerNorthAndMorphStart.w;

    const float morphEnd =
        g_morph.x;

    const float coarseSpacing =
        g_morph.y;

    const float hasCoarser =
        g_morph.z;

    if (hasCoarser > 0.5)
    {
        const float edgeDistance =
            max(
                abs(offsetMeters.x),
                abs(offsetMeters.y));

        const float normalized =
            saturate(
                (edgeDistance -
                 morphStart) /
                max(
                    morphEnd -
                        morphStart,
                    0.0001));

        const float morph =
            normalized *
            normalized *
            (3.0 -
             2.0 * normalized);

        offsetMeters =
            lerp(
                offsetMeters,
                morphTargetOffset,
                morph);
    }

    const float distanceMeters =
        length(offsetMeters);

    float3 tangentDirection =
        float3(0.0, 0.0, 0.0);

    if (distanceMeters > 0.0001)
    {
        tangentDirection =
            normalize(
                g_centerEastAndOriginY.xyz *
                    offsetMeters.x +
                g_centerNorthAndMorphStart.xyz *
                    offsetMeters.y);
    }

    const float angle =
        distanceMeters /
        planetRadius;

    const float sinAngle =
        sin(angle);

    const float cosAngle =
        cos(angle);

    float3 surfaceDirection =
        g_centerUpAndOriginX.xyz *
        cosAngle;

    if (distanceMeters > 0.0001)
    {
        surfaceDirection +=
            tangentDirection *
            sinAngle;
    }

    const float displacedRadius =
        planetRadius +
        elevation;

    const float3 localPosition =
        surfaceDirection *
            displacedRadius -
        float3(
            0.0,
            observerRadius,
            0.0);

    VSOutput output;

    output.position =
        mul(
            float4(
                localPosition,
                1.0),
            g_mvp);

    output.elevation =
        elevation;

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
    const float normalizedHeight =
        saturate(
            input.elevation /
                6000.0 +
            0.35);

    const float3 lowColor =
        float3(
            0.08,
            0.18,
            0.12);

    const float3 highColor =
        float3(
            0.72,
            0.78,
            0.72);

    return float4(
        lerp(
            lowColor,
            highColor,
            normalizedHeight),
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
        terrain_stream::TerrainSampleStreamer& sampleStreamer,
        const world::WorldPosition& observer,
        TerrainPreviewConfig config)
        : device_(device),
          planet_(planet),
          sampleStreamer_(sampleStreamer),
          config_(std::move(config)),
          layout_(
              terrain_view::BuildClipmapLayout(
                  config_.clipmap,
                  observer)),
          tracker_(
              planet_,
              config_.clipmap),
          residency_(
              config_.clipmap),
          levels_(
              config_.clipmap.levelCount)
    {
        if (math::Length(observer.meters) <=
            planet_.radiusMeters)
        {
            throw std::invalid_argument(
                "Orbit terrain preview observer must be above the planet surface.");
        }

        if (config_.framesInFlight == 0)
        {
            throw std::invalid_argument(
                "Orbit terrain preview requires at least one frame in flight.");
        }

        CreateSharedTopology();
        CreateLevelBuffers();
        CreatePipeline(
            shaderCompiler);

        InitializeBlocking(observer);
    }

    ~Impl()
    {
        if (pendingUpdate_.has_value() &&
            pendingUpdate_->batch.IsValid())
        {
            try
            {
                static_cast<void>(
                    sampleStreamer_.
                        WaitCollect(
                            pendingUpdate_->
                                batch));
            }
            catch (...)
            {
            }
        }
    }

    void UpdateObserver(
        const world::WorldPosition& observer)
    {
        SetObserverView(observer);

        desiredObserver_ = observer;
        ++desiredGeneration_;

        ServiceStreaming();
    }

    void Draw(
        rhi::CommandList& commandList,
        const u32 frameIndex,
        const u32 targetWidth,
        const u32 targetHeight)
    {
        ServiceStreaming();

        stats_.uploadedBytesLastFrame = 0;
        stats_.drawCallsLastFrame = 0;

        if (frameIndex >=
            config_.framesInFlight)
        {
            throw std::out_of_range(
                "Orbit terrain frame index exceeds configured frames in flight.");
        }

        if (targetWidth == 0 ||
            targetHeight == 0)
        {
            return;
        }

        const f32 aspect =
            static_cast<f32>(
                targetWidth) /
            static_cast<f32>(
                targetHeight);

        const math::Mat4 view =
            math::LookAtLH(
                {0.0F, 0.0F, 0.0F},
                {0.0F, -0.28F, 1.0F},
                {0.0F, 1.0F, 0.0F});

        const math::Mat4 projection =
            math::PerspectiveLH(
                config_.
                    verticalFovRadians,
                aspect,
                config_.
                    nearPlaneMeters,
                config_.
                    farPlaneMeters);

        const math::Mat4 mvp =
            math::Multiply(
                view,
                projection);

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

        commandList.
            SetGraphicsPipeline(
                *pipeline_);

        for (u32 levelIndex = 0;
             levelIndex <
                static_cast<u32>(
                    levels_.size());
             ++levelIndex)
        {
            stats_.uploadedBytesLastFrame +=
                PrepareLevelFrame(
                    levelIndex,
                    frameIndex);

            const terrain_view::ClipmapLevel&
                level =
                    layout_.levels[
                        levelIndex];

            const terrain_view::ClipmapLevel*
                coarserLevel =
                    levelIndex + 1U <
                        static_cast<u32>(
                            levels_.size())
                        ? &layout_.levels[
                            levelIndex +
                            1U]
                        : nullptr;

            const auto constants =
                BuildDrawConstants(
                    mvp,
                    static_cast<f32>(
                        planet_.
                            radiusMeters),
                    observerRadiusMeters_,
                    level,
                    coarserLevel,
                    motion_.levels[
                        levelIndex],
                    residencyUpdate_.levels[
                        levelIndex],
                    observerFrame_);

            commandList.
                SetGraphicsConstants(
                    constants);

            commandList.
                SetGraphicsBuffer(
                    0,
                    *levels_[levelIndex].
                        frameSampleBuffers[
                            frameIndex]);

            if (levelIndex == 0)
            {
                commandList.
                    SetIndexBuffer(
                        *centerIndexBuffer_,
                        rhi::IndexFormat::
                            UInt32);

                commandList.
                    DrawIndexed(
                        centerIndexCount_);

                ++stats_.drawCallsLastFrame;
            }
            else
            {
                commandList.
                    SetIndexBuffer(
                        *ringIndexBuffer_,
                        rhi::IndexFormat::
                            UInt32);

                commandList.
                    DrawIndexed(
                        ringIndexCount_);

                ++stats_.drawCallsLastFrame;
            }
        }

        stats_.cumulativeUploadedBytes +=
            stats_.uploadedBytesLastFrame;
    }

    [[nodiscard]] u32 VertexCount() const noexcept
    {
        const u64 perLevel =
            static_cast<u64>(
                config_.clipmap.
                    gridResolution) *
            static_cast<u64>(
                config_.clipmap.
                    gridResolution);

        const u64 total =
            perLevel *
            static_cast<u64>(
                config_.clipmap.
                    levelCount);

        return static_cast<u32>(
            std::min<u64>(
                total,
                std::numeric_limits<u32>::
                    max()));
    }

    [[nodiscard]] u32 IndexCount() const noexcept
    {
        const u64 total =
            static_cast<u64>(
                centerIndexCount_) +
            static_cast<u64>(
                ringIndexCount_) *
            static_cast<u64>(
                config_.clipmap.
                    levelCount - 1U);

        return static_cast<u32>(
            std::min<u64>(
                total,
                std::numeric_limits<u32>::
                    max()));
    }

    [[nodiscard]] const TerrainStreamingStats&
    StreamingStats() const noexcept
    {
        return stats_;
    }

private:
    struct CandidateState
    {
        terrain_view::ClipmapTracker tracker;
        terrain_stream::ToroidalResidency residency;
        terrain_view::ClipmapMotionUpdate motion;
        terrain_stream::ResidencyUpdate residencyUpdate;
        std::vector<
            terrain_stream::TerrainSampleRequest>
            requests;
    };

    struct PendingUpdate
    {
        u64 generation{0};
        CandidateState candidate;
        terrain_stream::TerrainSampleBatch batch;
    };

    struct DirtyUpdate
    {
        u64 serial{0};
        std::vector<
            terrain_stream::PhysicalRegion>
            regions;
    };

    struct LevelGpuState
    {
        std::vector<f32> cpuSamples;

        std::vector<
            std::unique_ptr<rhi::Buffer>>
            frameSampleBuffers;

        std::vector<u64> frameSerials;
        std::deque<DirtyUpdate> dirtyUpdates;
        u64 currentSerial{0};
    };

    void CreateSharedTopology()
    {
        const std::vector<u32>
            centerIndices =
                BuildCenterIndices(
                    config_.clipmap.
                        gridResolution);

        centerIndexCount_ =
            static_cast<u32>(
                centerIndices.size());

        const u64 centerBytes =
            static_cast<u64>(
                centerIndices.size()) *
            sizeof(u32);

        centerIndexBuffer_ =
            device_.CreateBuffer({
                .sizeBytes = centerBytes,
                .usage =
                    rhi::BufferUsage::
                        Index,
                .memory =
                    rhi::MemoryUsage::
                        HostVisible,
                .initialState =
                    rhi::ResourceState::
                        IndexBuffer
            });

        UploadBuffer(
            *centerIndexBuffer_,
            centerIndices.data(),
            static_cast<std::size_t>(
                centerBytes));

        if (config_.clipmap.levelCount > 1)
        {
            const std::vector<u32>
                ringIndices =
                    BuildRingIndices(
                        layout_.levels[1]);

            ringIndexCount_ =
                static_cast<u32>(
                    ringIndices.size());

            const u64 ringBytes =
                static_cast<u64>(
                    ringIndices.size()) *
                sizeof(u32);

            ringIndexBuffer_ =
                device_.CreateBuffer({
                    .sizeBytes =
                        ringBytes,
                    .usage =
                        rhi::BufferUsage::
                            Index,
                    .memory =
                        rhi::MemoryUsage::
                            HostVisible,
                    .initialState =
                        rhi::ResourceState::
                            IndexBuffer
                });

            UploadBuffer(
                *ringIndexBuffer_,
                ringIndices.data(),
                static_cast<std::size_t>(
                    ringBytes));
        }
    }

    void CreateLevelBuffers()
    {
        const u64 sampleCount =
            static_cast<u64>(
                config_.clipmap.
                    gridResolution) *
            static_cast<u64>(
                config_.clipmap.
                    gridResolution);

        const u64 bytes =
            sampleCount *
            sizeof(
                terrain_stream::
                    TerrainSampleValue);

        for (LevelGpuState& level :
             levels_)
        {
            level.cpuSamples.resize(
                static_cast<std::size_t>(
                    sampleCount));

            level.frameSerials.assign(
                config_.framesInFlight,
                0);

            level.frameSampleBuffers.reserve(
                config_.framesInFlight);

            for (u32 frameIndex = 0;
                 frameIndex <
                    config_.framesInFlight;
                 ++frameIndex)
            {
                level.frameSampleBuffers.push_back(
                    device_.CreateBuffer({
                        .sizeBytes = bytes,
                        .usage =
                            rhi::BufferUsage::
                                Structured,
                        .memory =
                            rhi::MemoryUsage::
                                HostVisible,
                        .initialState =
                            rhi::ResourceState::
                                ShaderResource
                    }));
            }
        }
    }

    void CreatePipeline(
        const shader::Compiler& shaderCompiler)
    {
        const shader::Binary
            vertexShader =
                shaderCompiler.Compile({
                    .source =
                        kVertexShader,
                    .entryPoint =
                        "main",
                    .stage =
                        shader::Stage::
                            Vertex,
                    .debug = false
                });

        const shader::Binary
            pixelShader =
                shaderCompiler.Compile({
                    .source =
                        kPixelShader,
                    .entryPoint =
                        "main",
                    .stage =
                        shader::Stage::
                            Pixel,
                    .debug = false
                });

        pipeline_ =
            device_.
                CreateGraphicsPipeline({
                    .vertexShader = {
                        .data =
                            vertexShader.
                                bytecode.
                                data(),
                        .size =
                            vertexShader.
                                bytecode.
                                size()
                    },
                    .pixelShader = {
                        .data =
                            pixelShader.
                                bytecode.
                                data(),
                        .size =
                            pixelShader.
                                bytecode.
                                size()
                    },
                    .vertexAttributes = {},
                    .vertexStrideBytes = 0,
                    .pushConstantDwords = 36,
                    .shaderResourceBuffers = 1,
                    .topology =
                        rhi::
                            PrimitiveTopology::
                                TriangleList,
                    .fillMode =
                        config_.wireframe
                            ? rhi::
                                FillMode::
                                    Wireframe
                            : rhi::
                                FillMode::
                                    Solid,
                    .cullMode =
                        rhi::CullMode::
                            None,
                    .depthTest = true,
                    .depthWrite = true
                });
    }

    void SetObserverView(
        const world::WorldPosition& observer)
    {
        const f64 observerRadius =
            math::Length(
                observer.meters);

        if (observerRadius <=
            planet_.radiusMeters)
        {
            throw std::invalid_argument(
                "Orbit terrain preview observer must be above the planet surface.");
        }

        observer_ = observer;
        observerFrame_ =
            world::MakeSurfaceFrame(
                observer.meters);

        observerRadiusMeters_ =
            static_cast<f32>(
                observerRadius);
    }

    [[nodiscard]] CandidateState BuildCandidate(
        const world::WorldPosition& observer)
    {
        CandidateState candidate{
            .tracker = tracker_,
            .residency = residency_
        };

        candidate.motion =
            candidate.tracker.Update(
                observer);

        candidate.residencyUpdate =
            candidate.residency.Apply(
                candidate.motion);

        candidate.requests.reserve(
            levels_.size());

        for (u32 levelIndex = 0;
             levelIndex <
                static_cast<u32>(
                    levels_.size());
             ++levelIndex)
        {
            const auto& levelUpdate =
                candidate.residencyUpdate.
                    levels[levelIndex];

            if (levelUpdate.
                    refreshRegions.empty())
            {
                continue;
            }

            const auto& level =
                layout_.levels[
                    levelIndex];

            const bool hasCoarser =
                levelIndex + 1U <
                static_cast<u32>(
                    levels_.size());

            const terrain_view::ClipmapLevel*
                coarserLevel =
                    hasCoarser
                        ? &layout_.levels[
                            levelIndex + 1U]
                        : nullptr;

            candidate.requests.push_back({
                .levelIndex = levelIndex,
                .resolution =
                    level.gridResolution,
                .spacingMeters =
                    level.sampleSpacingMeters,
                .footprintMeters =
                    level.terrainFootprintMeters,
                .morphToCoarser =
                    hasCoarser,
                .morphStartHalfExtentMeters =
                    level.
                        morphStartHalfExtentMeters,
                .morphEndHalfExtentMeters =
                    level.
                        morphEndHalfExtentMeters,
                .coarseSpacingMeters =
                    coarserLevel != nullptr
                        ? coarserLevel->
                            sampleSpacingMeters
                        : 0.0,
                .coarseFootprintMeters =
                    coarserLevel != nullptr
                        ? coarserLevel->
                            terrainFootprintMeters
                        : 0.0,
                .surfaceFrame =
                    candidate.motion.levels[
                        levelIndex].
                        surfaceFrame,
                .coarseSurfaceFrame =
                    hasCoarser
                        ? candidate.motion.
                            levels[
                                levelIndex +
                                1U].
                            surfaceFrame
                        : candidate.motion.
                            levels[
                                levelIndex].
                            surfaceFrame,
                .originX =
                    levelUpdate.originX,
                .originY =
                    levelUpdate.originY,
                .regions =
                    levelUpdate.
                        refreshRegions
            });
        }

        return candidate;
    }

    void ResetCommitStats() noexcept
    {
        stats_.generatedSamplesLastUpdate = 0;
        stats_.refreshedRegionsLastUpdate = 0;
        stats_.levelsTouchedLastUpdate = 0;
    }

    void CommitCandidate(
        CandidateState&& candidate,
        const std::vector<
            terrain_stream::TerrainSampleResult>&
            results)
    {
        ResetCommitStats();

        tracker_ =
            std::move(
                candidate.tracker);

        residency_ =
            std::move(
                candidate.residency);

        motion_ =
            std::move(
                candidate.motion);

        residencyUpdate_ =
            std::move(
                candidate.residencyUpdate);

        for (const auto& result :
             results)
        {
            if (result.patches.empty())
            {
                continue;
            }

            ++stats_.
                levelsTouchedLastUpdate;

            stats_.
                refreshedRegionsLastUpdate +=
                    static_cast<u32>(
                        result.patches.size());

            stats_.
                generatedSamplesLastUpdate +=
                    result.sampleCount;

            ApplySampleResult(result);
        }

        stats_.
            cumulativeGeneratedSamples +=
                stats_.
                    generatedSamplesLastUpdate;
    }

    void InitializeBlocking(
        const world::WorldPosition& observer)
    {
        SetObserverView(observer);

        desiredObserver_ = observer;
        desiredGeneration_ = 1;

        CandidateState candidate =
            BuildCandidate(observer);

        const auto results =
            sampleStreamer_.
                GenerateBlocking(
                    candidate.requests);

        CommitCandidate(
            std::move(candidate),
            results);

        committedGeneration_ =
            desiredGeneration_;

        ++stats_.committedBatches;
        stats_.updatePending = false;
    }

    void LaunchLatestUpdate()
    {
        if (pendingUpdate_.has_value() ||
            committedGeneration_ ==
                desiredGeneration_)
        {
            return;
        }

        CandidateState candidate =
            BuildCandidate(
                desiredObserver_);

        if (candidate.requests.empty())
        {
            CommitCandidate(
                std::move(candidate),
                {});

            committedGeneration_ =
                desiredGeneration_;

            ++stats_.committedBatches;
            stats_.updatePending = false;
            return;
        }

        terrain_stream::TerrainSampleBatch batch =
            sampleStreamer_.Submit(
                candidate.requests);

        pendingUpdate_.emplace(
            PendingUpdate{
                .generation =
                    desiredGeneration_,
                .candidate =
                    std::move(candidate),
                .batch =
                    std::move(batch)
            });

        ++stats_.submittedBatches;
        stats_.updatePending = true;
    }

    void ServiceStreaming()
    {
        if (pendingUpdate_.has_value())
        {
            if (!pendingUpdate_->
                    batch.IsComplete())
            {
                stats_.updatePending = true;
                return;
            }

            std::vector<
                terrain_stream::
                    TerrainSampleResult>
                results;

            if (!sampleStreamer_.TryCollect(
                    pendingUpdate_->batch,
                    results))
            {
                stats_.updatePending = true;
                return;
            }

            const u64 generation =
                pendingUpdate_->generation;

            CandidateState candidate =
                std::move(
                    pendingUpdate_->
                        candidate);

            pendingUpdate_.reset();
            stats_.updatePending = false;

            const bool superseded =
                generation !=
                    desiredGeneration_;

            CommitCandidate(
                std::move(candidate),
                results);

            committedGeneration_ =
                generation;

            ++stats_.committedBatches;

            if (superseded)
            {
                ++stats_.supersededBatches;
            }
        }

        if (!pendingUpdate_.has_value() &&
            committedGeneration_ !=
                desiredGeneration_)
        {
            LaunchLatestUpdate();
        }
    }

    void ApplySampleResult(
        const terrain_stream::
            TerrainSampleResult& result)
    {
        if (result.levelIndex >=
            levels_.size())
        {
            throw std::out_of_range(
                "Orbit terrain sample result references an invalid clipmap level.");
        }

        LevelGpuState& state =
            levels_[result.levelIndex];

        const u32 resolution =
            config_.clipmap.
                gridResolution;

        std::vector<
            terrain_stream::PhysicalRegion>
            dirtyRegions;

        dirtyRegions.reserve(
            result.patches.size());

        for (const auto& patch :
             result.patches)
        {
            const std::size_t expectedSamples =
                static_cast<std::size_t>(
                    patch.region.width) *
                patch.region.height;

            if (patch.samples.size() !=
                expectedSamples)
            {
                throw std::runtime_error(
                    "Orbit terrain sample patch size does not match its physical region.");
            }

            for (u32 row = 0;
                 row < patch.region.height;
                 ++row)
            {
                const std::size_t destinationOffset =
                    static_cast<std::size_t>(
                        patch.region.y + row) *
                        resolution +
                    patch.region.x;

                const std::size_t sourceOffset =
                    static_cast<std::size_t>(
                        row) *
                    patch.region.width;

                std::memcpy(
                    state.cpuSamples.data() +
                        destinationOffset,
                    patch.samples.data() +
                        sourceOffset,
                    static_cast<std::size_t>(
                        patch.region.width) *
                        sizeof(
                            terrain_stream::
                                TerrainSampleValue));
            }

            dirtyRegions.push_back(
                patch.region);
        }

        ++state.currentSerial;

        state.dirtyUpdates.push_back({
            .serial =
                state.currentSerial,
            .regions =
                std::move(dirtyRegions)
        });
    }

    [[nodiscard]] u64 PrepareLevelFrame(
        const u32 levelIndex,
        const u32 frameIndex)
    {
        LevelGpuState& state =
            levels_[levelIndex];

        u64& frameSerial =
            state.frameSerials[
                frameIndex];

        if (frameSerial ==
            state.currentSerial)
        {
            return 0;
        }

        rhi::Buffer& buffer =
            *state.frameSampleBuffers[
                frameIndex];

        std::byte* mapped =
            buffer.Map();

        auto* destination =
            reinterpret_cast<
                terrain_stream::
                    TerrainSampleValue*>(
                        mapped);

        u64 uploadedBytes = 0;

        const u32 resolution =
            config_.clipmap.
                gridResolution;

        for (const DirtyUpdate& update :
             state.dirtyUpdates)
        {
            if (update.serial <=
                frameSerial)
            {
                continue;
            }

            for (const terrain_stream::
                     PhysicalRegion& region :
                 update.regions)
            {
                for (u32 row = 0;
                     row < region.height;
                     ++row)
                {
                    const std::size_t offset =
                        static_cast<std::size_t>(
                            region.y + row) *
                            resolution +
                        region.x;

                    const std::size_t rowBytes =
                        static_cast<std::size_t>(
                            region.width) *
                        sizeof(
                            terrain_stream::
                                TerrainSampleValue);

                    std::memcpy(
                        destination + offset,
                        state.cpuSamples.data() +
                            offset,
                        rowBytes);

                    uploadedBytes +=
                        static_cast<u64>(
                            rowBytes);
                }
            }
        }

        buffer.Unmap();

        frameSerial =
            state.currentSerial;

        PruneDirtyHistory(state);

        return uploadedBytes;
    }

    static void PruneDirtyHistory(
        LevelGpuState& state)
    {
        if (state.frameSerials.empty())
        {
            return;
        }

        const u64 minimumAppliedSerial =
            *std::min_element(
                state.frameSerials.begin(),
                state.frameSerials.end());

        while (!state.dirtyUpdates.empty() &&
               state.dirtyUpdates.front().
                   serial <=
                   minimumAppliedSerial)
        {
            state.dirtyUpdates.pop_front();
        }
    }

    rhi::Device& device_;
    world::PlanetDefinition planet_;
    terrain_stream::TerrainSampleStreamer&
        sampleStreamer_;

    TerrainPreviewConfig config_;
    terrain_view::ClipmapLayout layout_;
    terrain_view::ClipmapTracker tracker_;
    terrain_stream::ToroidalResidency
        residency_;

    std::vector<LevelGpuState> levels_;

    std::unique_ptr<rhi::Buffer>
        centerIndexBuffer_;

    std::unique_ptr<rhi::Buffer>
        ringIndexBuffer_;

    std::unique_ptr<
        rhi::GraphicsPipeline>
        pipeline_;

    world::WorldPosition observer_{};
    world::WorldPosition desiredObserver_{};
    world::SurfaceFrame observerFrame_{};

    terrain_view::ClipmapMotionUpdate
        motion_;

    terrain_stream::ResidencyUpdate
        residencyUpdate_;

    f32 observerRadiusMeters_{0.0F};

    u64 desiredGeneration_{0};
    u64 committedGeneration_{0};

    std::optional<PendingUpdate>
        pendingUpdate_;

    TerrainStreamingStats stats_{};

    u32 centerIndexCount_{0};
    u32 ringIndexCount_{0};
};

TerrainPreviewRenderer::
TerrainPreviewRenderer(
    rhi::Device& device,
    const shader::Compiler& shaderCompiler,
    const world::PlanetDefinition& planet,
    terrain_stream::TerrainSampleStreamer& sampleStreamer,
    const world::WorldPosition& observer,
    TerrainPreviewConfig config)
    : impl_(std::make_unique<Impl>(
        device,
        shaderCompiler,
        planet,
        sampleStreamer,
        observer,
        std::move(config)))
{
}

TerrainPreviewRenderer::
~TerrainPreviewRenderer() = default;

TerrainPreviewRenderer::
TerrainPreviewRenderer(
    TerrainPreviewRenderer&&) noexcept =
    default;

TerrainPreviewRenderer&
TerrainPreviewRenderer::operator=(
    TerrainPreviewRenderer&&) noexcept =
    default;

void TerrainPreviewRenderer::UpdateObserver(
    const world::WorldPosition& observer)
{
    impl_->UpdateObserver(observer);
}

void TerrainPreviewRenderer::Draw(
    rhi::CommandList& commandList,
    const u32 frameIndex,
    const u32 targetWidth,
    const u32 targetHeight)
{
    impl_->Draw(
        commandList,
        frameIndex,
        targetWidth,
        targetHeight);
}

u32 TerrainPreviewRenderer::
VertexCount() const noexcept
{
    return impl_->VertexCount();
}

u32 TerrainPreviewRenderer::
IndexCount() const noexcept
{
    return impl_->IndexCount();
}

const TerrainStreamingStats&
TerrainPreviewRenderer::
StreamingStats() const noexcept
{
    return impl_->StreamingStats();
}
} // namespace orbit::terrain_render
