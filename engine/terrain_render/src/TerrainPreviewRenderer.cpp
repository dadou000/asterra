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
    float4 biome0 : TEXCOORD1;
    float4 biome1 : TEXCOORD2;
    float3 terrainNormal : TEXCOORD3;
    float3 surfaceDirection : TEXCOORD4;
    float horizonClip : SV_ClipDistance0;
};

float4 UnpackUnorm4x8(uint packed)
{
    return float4(
        (packed & 0xFFu),
        ((packed >> 8u) & 0xFFu),
        ((packed >> 16u) & 0xFFu),
        ((packed >> 24u) & 0xFFu)) /
        255.0;
}

uint PhysicalSampleIndex(
    uint logicalX,
    uint logicalY,
    uint resolution,
    uint originX,
    uint originY)
{
    const uint physicalX =
        (logicalX + originX) %
        resolution;

    const uint physicalY =
        (logicalY + originY) %
        resolution;

    return
        physicalY * resolution +
        physicalX;
}

float LoadElevation(
    uint logicalX,
    uint logicalY,
    uint resolution,
    uint originX,
    uint originY)
{
    const uint sampleIndex =
        PhysicalSampleIndex(
            logicalX,
            logicalY,
            resolution,
            originX,
            originY);

    return asfloat(
        g_samples.Load(
            sampleIndex * 20u));
}

float3 SurfaceDirectionForOffset(
    float2 offsetMeters,
    float planetRadius)
{
    const float distanceMeters =
        length(offsetMeters);

    if (distanceMeters <= 0.0001)
    {
        return
            g_centerUpAndOriginX.xyz;
    }

    const float3 tangentDirection =
        normalize(
            g_centerEastAndOriginY.xyz *
                offsetMeters.x +
            g_centerNorthAndMorphStart.xyz *
                offsetMeters.y);

    const float angle =
        distanceMeters /
        planetRadius;

    return normalize(
        g_centerUpAndOriginX.xyz *
            cos(angle) +
        tangentDirection *
            sin(angle));
}

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

    const uint physicalIndex =
        PhysicalSampleIndex(
            logicalX,
            logicalY,
            resolution,
            originX,
            originY);

    const uint sampleByteOffset =
        physicalIndex * 20u;

    const float elevation =
        asfloat(
            g_samples.Load(
                sampleByteOffset));

    const float2 morphTargetOffset =
        asfloat(
            g_samples.Load2(
                sampleByteOffset + 4u));

    const uint packedBiome0 =
        g_samples.Load(
            sampleByteOffset + 12u);

    const uint packedBiome1 =
        g_samples.Load(
            sampleByteOffset + 16u);

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

    const float3 surfaceDirection =
        SurfaceDirectionForOffset(
            offsetMeters,
            planetRadius);

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

    const uint leftX =
        logicalX > 0u
            ? logicalX - 1u
            : logicalX;

    const uint rightX =
        logicalX + 1u < resolution
            ? logicalX + 1u
            : logicalX;

    const uint downY =
        logicalY > 0u
            ? logicalY - 1u
            : logicalY;

    const uint upY =
        logicalY + 1u < resolution
            ? logicalY + 1u
            : logicalY;

    const float elevationLeft =
        LoadElevation(
            leftX,
            logicalY,
            resolution,
            originX,
            originY);

    const float elevationRight =
        LoadElevation(
            rightX,
            logicalY,
            resolution,
            originX,
            originY);

    const float elevationDown =
        LoadElevation(
            logicalX,
            downY,
            resolution,
            originX,
            originY);

    const float elevationUp =
        LoadElevation(
            logicalX,
            upY,
            resolution,
            originX,
            originY);

    const float xDistance =
        max(
            (float)(rightX - leftX) *
                spacing,
            0.0001);

    const float yDistance =
        max(
            (float)(upY - downY) *
                spacing,
            0.0001);

    const float slopeEast =
        (elevationRight -
         elevationLeft) /
        xDistance;

    const float slopeNorth =
        (elevationUp -
         elevationDown) /
        yDistance;

    float3 tangentEast =
        g_centerEastAndOriginY.xyz -
        surfaceDirection *
            dot(
                g_centerEastAndOriginY.xyz,
                surfaceDirection);

    const float tangentEastLength =
        length(tangentEast);

    if (tangentEastLength > 0.0001)
    {
        tangentEast /=
            tangentEastLength;
    }
    else
    {
        tangentEast =
            float3(
                1.0,
                0.0,
                0.0);
    }

    float3 tangentNorth =
        g_centerNorthAndMorphStart.xyz -
        surfaceDirection *
            dot(
                g_centerNorthAndMorphStart.xyz,
                surfaceDirection);

    tangentNorth -=
        tangentEast *
        dot(
            tangentNorth,
            tangentEast);

    const float tangentNorthLength =
        length(tangentNorth);

    if (tangentNorthLength > 0.0001)
    {
        tangentNorth /=
            tangentNorthLength;
    }
    else
    {
        tangentNorth =
            float3(
                0.0,
                0.0,
                1.0);
    }

    const float3 terrainNormal =
        normalize(
            surfaceDirection -
            tangentEast *
                slopeEast -
            tangentNorth *
                slopeNorth);

    VSOutput output;

    output.position =
        mul(
            float4(
                localPosition,
                1.0),
            g_mvp);

    output.elevation =
        elevation;

    output.biome0 =
        UnpackUnorm4x8(
            packedBiome0);

    output.biome1 =
        UnpackUnorm4x8(
            packedBiome1);

    output.terrainNormal =
        terrainNormal;

    output.surfaceDirection =
        surfaceDirection;

    const float horizonCosine =
        saturate(
            planetRadius /
            observerRadius);

    const float positiveReliefPadding =
        max(
            elevation,
            0.0) /
        planetRadius;

    output.horizonClip =
        surfaceDirection.y -
        horizonCosine +
        0.0025 +
        positiveReliefPadding;

    return output;
}
)";

constexpr const char* kPixelShader = R"(
struct VSOutput
{
    float4 position : SV_Position;
    float elevation : TEXCOORD0;
    float4 biome0 : TEXCOORD1;
    float4 biome1 : TEXCOORD2;
    float3 terrainNormal : TEXCOORD3;
    float3 surfaceDirection : TEXCOORD4;
    float horizonClip : SV_ClipDistance0;
};

float4 main(VSOutput input) : SV_Target0
{
    float4 biome0 =
        max(
            input.biome0,
            0.0);

    float4 biome1 =
        max(
            input.biome1,
            0.0);

    const float weightSum =
        biome0.x +
        biome0.y +
        biome0.z +
        biome0.w +
        biome1.x +
        biome1.y +
        biome1.z +
        biome1.w;

    const float inverseWeight =
        1.0 /
        max(
            weightSum,
            0.0001);

    biome0 *= inverseWeight;
    biome1 *= inverseWeight;

    const float3 oceanColor =
        float3(
            0.025,
            0.11,
            0.24);

    const float3 desertColor =
        float3(
            0.72,
            0.56,
            0.31);

    const float3 grasslandColor =
        float3(
            0.26,
            0.42,
            0.16);

    const float3 temperateForestColor =
        float3(
            0.075,
            0.25,
            0.11);

    const float3 borealForestColor =
        float3(
            0.08,
            0.20,
            0.16);

    const float3 tundraColor =
        float3(
            0.43,
            0.48,
            0.42);

    const float3 alpineColor =
        float3(
            0.58,
            0.59,
            0.57);

    const float3 wetlandColor =
        float3(
            0.09,
            0.27,
            0.22);

    float3 color =
        oceanColor *
            biome0.x +
        desertColor *
            biome0.y +
        grasslandColor *
            biome0.z +
        temperateForestColor *
            biome0.w +
        borealForestColor *
            biome1.x +
        tundraColor *
            biome1.y +
        alpineColor *
            biome1.z +
        wetlandColor *
            biome1.w;

    const float3 terrainNormal =
        normalize(
            input.terrainNormal);

    const float3 surfaceDirection =
        normalize(
            input.surfaceDirection);

    const float slopeCosine =
        saturate(
            dot(
                terrainNormal,
                surfaceDirection));

    const float slopeStrength =
        1.0 -
        slopeCosine;

    const float landWeight =
        saturate(
            1.0 -
            biome0.x);

    const float rockBlend =
        smoothstep(
            0.06,
            0.34,
            slopeStrength) *
        landWeight *
        0.72;

    const float3 rockColor =
        float3(
            0.30,
            0.295,
            0.285);

    color =
        lerp(
            color,
            rockColor,
            rockBlend);

    const float3 previewLightDirection =
        normalize(
            float3(
                -0.42,
                0.78,
                0.46));

    const float diffuse =
        saturate(
            dot(
                terrainNormal,
                previewLightDirection));

    const float hemispheric =
        0.58 +
        0.42 *
        slopeCosine;

    const float lighting =
        (0.36 +
         diffuse * 0.64) *
        hemispheric;

    const float elevationLight =
        saturate(
            input.elevation /
                8000.0);

    color *=
        lighting *
        (0.92 +
         elevationLight *
            0.15);

    return float4(
        color,
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
          observedSourceRevision_(
              sampleStreamer.SourceRevision()),
          config_(std::move(config)),
          baseClipmapConfig_(
              config_.clipmap),
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

        desiredCoverageTier_ =
            SelectCoverageTier(
                observer,
                desiredCoverageTier_);

        ++desiredGeneration_;

        ServiceStreaming();
    }

    void Draw(
        rhi::CommandList& commandList,
        const u32 frameIndex,
        const u32 targetWidth,
        const u32 targetHeight,
        const TerrainPreviewCamera& camera)
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

        math::Float3 cameraForward =
            math::Normalize(
                camera.forward);

        if (math::LengthSquared(
                cameraForward) <=
            1.0e-8F)
        {
            cameraForward = {
                0.0F,
                -0.28F,
                1.0F
            };

            cameraForward =
                math::Normalize(
                    cameraForward);
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

        const math::Mat4 view =
            math::LookAtLH(
                {0.0F, 0.0F, 0.0F},
                cameraForward,
                cameraUp);

        const math::Mat4 projection =
            math::PerspectiveReverseZLH(
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
                    commandList,
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
                        frameGpuSampleBuffers[
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
        u32 coverageTier{0};
        terrain_view::ClipmapConfig clipmapConfig{};
        terrain_view::ClipmapLayout layout;
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
        std::vector<
            terrain_stream::TerrainSampleValue>
            cpuSamples;

        std::vector<
            std::unique_ptr<rhi::Buffer>>
            frameGpuSampleBuffers;

        std::vector<
            std::unique_ptr<rhi::Buffer>>
            frameUploadBuffers;

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

            level.frameGpuSampleBuffers.reserve(
                config_.framesInFlight);

            level.frameUploadBuffers.reserve(
                config_.framesInFlight);

            for (u32 frameIndex = 0;
                 frameIndex <
                    config_.framesInFlight;
                 ++frameIndex)
            {
                level.frameGpuSampleBuffers.push_back(
                    device_.CreateBuffer({
                        .sizeBytes = bytes,
                        .usage =
                            rhi::BufferUsage::
                                Structured,
                        .memory =
                            rhi::MemoryUsage::
                                GpuOnly,
                        .initialState =
                            rhi::ResourceState::
                                ShaderResource
                    }));

                level.frameUploadBuffers.push_back(
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
                                CopySource
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
                    .depthCompare =
                        rhi::DepthCompare::GreaterEqual,
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

        observerRadiusMeters_ =
            static_cast<f32>(
                observerRadius);
    }

    [[nodiscard]] u32 SelectCoverageTier(
        const world::WorldPosition& observer,
        const u32 currentTier) const
    {
        const f64 observerRadiusMeters =
            math::Length(
                observer.meters);

        const f64 horizonArcMeters =
            world::HorizonArcDistanceMeters(
                planet_.radiusMeters,
                observerRadiusMeters);

        return terrain_view::
            SelectAdaptiveClipmapTierForHalfExtent(
                baseClipmapConfig_,
                config_.adaptiveCoverage,
                horizonArcMeters,
                currentTier);
    }

    [[nodiscard]] CandidateState BuildCandidate(
        const world::WorldPosition& observer)
    {
        const terrain_view::ClipmapConfig
            candidateConfig =
                terrain_view::
                    ClipmapConfigForTier(
                        baseClipmapConfig_,
                        desiredCoverageTier_);

        const bool reuseCommittedState =
            desiredCoverageTier_ ==
                activeCoverageTier_;

        CandidateState candidate{
            .coverageTier =
                desiredCoverageTier_,
            .clipmapConfig =
                candidateConfig,
            .layout =
                terrain_view::
                    BuildClipmapLayout(
                        candidateConfig,
                        observer),
            .tracker =
                reuseCommittedState
                    ? tracker_
                    : terrain_view::
                        ClipmapTracker(
                            planet_,
                            candidateConfig),
            .residency =
                reuseCommittedState
                    ? residency_
                    : terrain_stream::
                        ToroidalResidency(
                            candidateConfig)
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
                candidate.layout.levels[
                    levelIndex];

            const bool hasCoarser =
                levelIndex + 1U <
                static_cast<u32>(
                    levels_.size());

            const terrain_view::ClipmapLevel*
                coarserLevel =
                    hasCoarser
                        ? &candidate.layout.levels[
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

        const bool coverageTierChanged =
            candidate.coverageTier !=
                activeCoverageTier_;

        config_.clipmap =
            candidate.clipmapConfig;

        layout_ =
            std::move(
                candidate.layout);

        activeCoverageTier_ =
            candidate.coverageTier;

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

        if (coverageTierChanged)
        {
            ++stats_.coverageTierChanges;
        }

        stats_.adaptiveCoverageTier =
            activeCoverageTier_;

        stats_.activeBaseSpacingMeters =
            config_.clipmap.
                baseSpacingMeters;

        stats_.activeOuterHalfExtentMeters =
            terrain_view::
                ClipmapOuterHalfExtentMeters(
                    config_.clipmap);
    }

    void InitializeBlocking(
        const world::WorldPosition& observer)
    {
        SetObserverView(observer);

        desiredObserver_ = observer;

        desiredCoverageTier_ =
            SelectCoverageTier(
                observer,
                activeCoverageTier_);

        desiredGeneration_ = 1;

        for (;;)
        {
            CandidateState candidate =
                BuildCandidate(observer);

            auto batch =
                sampleStreamer_.Submit(
                    candidate.requests);

            const u64 sourceRevision =
                batch.SourceRevision();

            auto results =
                sampleStreamer_.WaitCollect(
                    batch);

            if (sourceRevision !=
                sampleStreamer_.SourceRevision())
            {
                DetectSourceRevisionChange();
                ++stats_.staleRevisionBatches;
                continue;
            }

            observedSourceRevision_ =
                sourceRevision;

            CommitCandidate(
                std::move(candidate),
                results);

            committedGeneration_ =
                desiredGeneration_;

            ++stats_.committedBatches;
            stats_.updatePending = false;
            break;
        }
    }

    void DetectSourceRevisionChange()
    {
        const u64 currentRevision =
            sampleStreamer_.SourceRevision();

        if (currentRevision ==
            observedSourceRevision_)
        {
            return;
        }

        observedSourceRevision_ =
            currentRevision;

        tracker_.Reset();
        residency_.Reset();

        ++desiredGeneration_;
        ++stats_.revisionInvalidations;
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
        DetectSourceRevisionChange();

        if (pendingUpdate_.has_value())
        {
            if (!pendingUpdate_->
                    batch.IsComplete())
            {
                stats_.updatePending = true;
                return;
            }

            const u64 batchSourceRevision =
                pendingUpdate_->
                    batch.SourceRevision();

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

            DetectSourceRevisionChange();

            const u64 generation =
                pendingUpdate_->generation;

            CandidateState candidate =
                std::move(
                    pendingUpdate_->
                        candidate);

            pendingUpdate_.reset();
            stats_.updatePending = false;

            const bool staleRevision =
                batchSourceRevision !=
                    observedSourceRevision_;

            if (staleRevision)
            {
                ResetCommitStats();
                ++stats_.staleRevisionBatches;
            }
            else
            {
                const bool superseded =
                    generation !=
                        desiredGeneration_;

                const bool obsoleteCoverageTier =
                    superseded &&
                    candidate.coverageTier !=
                        desiredCoverageTier_;

                if (obsoleteCoverageTier)
                {
                    ResetCommitStats();
                    ++stats_.supersededBatches;
                }
                else
                {
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
        rhi::CommandList& commandList,
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

        rhi::Buffer& gpuBuffer =
            *state.frameGpuSampleBuffers[
                frameIndex];

        rhi::Buffer& uploadBuffer =
            *state.frameUploadBuffers[
                frameIndex];

        std::byte* mapped =
            uploadBuffer.Map();

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

        uploadBuffer.Unmap();

        commandList.Transition(
            gpuBuffer,
            rhi::ResourceState::
                ShaderResource,
            rhi::ResourceState::
                CopyDestination);

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
                    const u64 elementOffset =
                        static_cast<u64>(
                            region.y + row) *
                            resolution +
                        region.x;

                    const u64 byteOffset =
                        elementOffset *
                        sizeof(
                            terrain_stream::
                                TerrainSampleValue);

                    const u64 rowBytes =
                        static_cast<u64>(
                            region.width) *
                        sizeof(
                            terrain_stream::
                                TerrainSampleValue);

                    commandList.CopyBuffer(
                        uploadBuffer,
                        byteOffset,
                        gpuBuffer,
                        byteOffset,
                        rowBytes);
                }
            }
        }

        commandList.Transition(
            gpuBuffer,
            rhi::ResourceState::
                CopyDestination,
            rhi::ResourceState::
                ShaderResource);

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

    u64 observedSourceRevision_{0};

    TerrainPreviewConfig config_;
    terrain_view::ClipmapConfig
        baseClipmapConfig_{};
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
    bool observerFrameInitialized_{false};

    terrain_view::ClipmapMotionUpdate
        motion_;

    terrain_stream::ResidencyUpdate
        residencyUpdate_;

    f32 observerRadiusMeters_{0.0F};

    u64 desiredGeneration_{0};
    u64 committedGeneration_{0};

    u32 activeCoverageTier_{0};
    u32 desiredCoverageTier_{0};

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
    const u32 targetHeight,
    const TerrainPreviewCamera& camera)
{
    impl_->Draw(
        commandList,
        frameIndex,
        targetWidth,
        targetHeight,
        camera);
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
