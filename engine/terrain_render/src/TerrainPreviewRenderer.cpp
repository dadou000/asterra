#include <orbit/terrain_render/TerrainPreviewRenderer.hpp>
#include "TerrainSurfaceShader.hpp"

#include <orbit/math/Matrix.hpp>
#include <orbit/math/Vector.hpp>
#include <orbit/terrain_stream/TerrainMorphRefresh.hpp>
#include <orbit/terrain_stream/TerrainSampleStreamer.hpp>
#include <orbit/terrain_stream/ToroidalResidency.hpp>
#include <orbit/terrain_view/ClipmapTracker.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace orbit::terrain_render
{
namespace
{
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

[[nodiscard]] std::array<u32, 42> BuildDrawConstants(
    const math::Mat4& matrix,
    const f32 planetRadiusMeters,
    const f32 observerRadiusMeters,
    const terrain_view::ClipmapLevel& level,
    const terrain_view::ClipmapLevel* coarserLevel,
    const terrain_view::ClipmapLevelMotion& motion,
    const terrain_view::ClipmapLevelMotion* finerMotion,
    const terrain_stream::LevelResidencyUpdate& residency,
    const world::SurfaceFrame& observerFrame,
    const u32 levelIndex,
    const bool debugLodColorEnabled,
    const bool debugSideCutEnabled) noexcept
{
    std::array<u32, 42> result{};

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

    math::Double2 innerHoleCenterOffset{};

    if (finerMotion != nullptr &&
        level.innerHoleHalfExtentMeters > 0.0)
    {
        // All LODs share one stable spherical lattice. Their independently
        // snapped centers are therefore expressed in the same chart, so the
        // coarse ring hole is an exact integer-lattice offset with no
        // spherical reprojection/phase ambiguity.
        innerHoleCenterOffset = {
            finerMotion->centerOffsetMeters.x -
                motion.centerOffsetMeters.x,
            finerMotion->centerOffsetMeters.y -
                motion.centerOffsetMeters.y
        };
    }

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
    store(
        33,
        static_cast<f32>(
            innerHoleCenterOffset.x));
    store(
        34,
        coarserLevel != nullptr
            ? 1.0F
            : 0.0F);
    store(
        35,
        static_cast<f32>(
            level.innerHoleHalfExtentMeters));

    // Debug-visuals block (F2 menu) -- see the vertex shader's use of
    // g_debug. Zero/false by default, so this is a no-op unless a
    // caller explicitly turns one of these on.
    store(36, static_cast<f32>(levelIndex));
    store(37, debugLodColorEnabled ? 1.0F : 0.0F);
    store(38, debugSideCutEnabled ? 1.0F : 0.0F);
    store(
        39,
        static_cast<f32>(
            innerHoleCenterOffset.y));

    store(
        40,
        static_cast<f32>(
            motion.centerOffsetMeters.x));
    store(
        41,
        static_cast<f32>(
            motion.centerOffsetMeters.y));

    return result;
}

constexpr const char* kVertexShader = R"(
struct DrawConstants
{
    row_major float4x4 g_mvp;

    float4 g_planet;
    float4 g_centerUpAndOriginX;
    float4 g_centerEastAndOriginY;
    float4 g_centerNorthAndMorphStart;
    // x = morph end, y = inner-hole center X in this level's local
    // tangent frame, z = has coarser level, w = inner-hole half extent.
    float4 g_morph;
    // Debug-visuals block (F2 menu): x = this level's index (as a
    // float), y = LOD-color override enabled, z = side-cut enabled,
    // w = inner-hole center Y in this level's local tangent frame.
    float4 g_debug;
    // Stable spherical-lattice offset of this LOD window center.
    float2 g_centerOffsetMeters;
};
[[vk::push_constant]] DrawConstants g_pc;

[[vk::binding(0, 0)]]
ByteAddressBuffer g_samples : register(t0);

struct VSOutput
{
    float4 position : SV_Position;
    float elevation : TEXCOORD0;
    float4 biome0 : TEXCOORD1;
    float4 biome1 : TEXCOORD2;
    float3 terrainNormal : TEXCOORD3;
    float3 surfaceDirection : TEXCOORD4;
    float waterDepth : TEXCOORD5;
    float3 localPosition : TEXCOORD6;
    // Shared pixel-shader interface with UniformPlanetRenderer's
    // whole-planet mesh (see TerrainSurfaceShader.hpp / ApplyDetailNormal)
    // -- always zero here, since this renderer's terrainNormal is
    // already real ground truth (see LoadFineSlope below) and doesn't
    // need or want the shared fake-bump fallback.
    float spacingMeters : TEXCOORD7;
    float3 worldPosition : TEXCOORD8;
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

// Ground-truth slope baked in at sample-generation time from the
// finest active clipmap level's own footprint/epsilon (see
// TerrainSampleRequest::fineNormalFootprintMeters), independent of
// this ring's own (possibly much wider) sample spacing -- this is
// what lets a far ring's shading normal still read like real L0
// ground detail instead of a blocky normal faceted by wide geometric
// sample spacing.
float2 LoadFineSlope(
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

    const uint address = sampleIndex * 32u;
    return asfloat(g_samples.Load2(address + 24u));
}

// Walks `offsetMeters` (a tangent-plane displacement from the tile
// center) along the sphere's curvature, starting from the given
// up/east/north basis (the observer-relative one, g_centerUpAndOriginX
// etc, for render-time positioning precision near a moving camera --
// see SurfaceDirectionForOffset below).
float3 SurfaceDirectionForOffsetFromBasis(
    float2 offsetMeters,
    float planetRadius,
    float3 up,
    float3 east,
    float3 north)
{
    float3 direction = up;

    const float distanceMeters =
        length(offsetMeters);

    if (distanceMeters > 0.0001)
    {
        const float3 tangentDirection =
            normalize(
                east * offsetMeters.x +
                north * offsetMeters.y);

        const float angle =
            distanceMeters /
            planetRadius;

        direction =
            normalize(
                up * cos(angle) +
                tangentDirection *
                    sin(angle));
    }

    return direction;
}

float3 SurfaceDirectionForOffset(
    float2 offsetMeters,
    float planetRadius)
{
    return SurfaceDirectionForOffsetFromBasis(
        offsetMeters,
        planetRadius,
        g_pc.g_centerUpAndOriginX.xyz,
        g_pc.g_centerEastAndOriginY.xyz,
        g_pc.g_centerNorthAndMorphStart.xyz);
}

VSOutput main(uint vertexId : SV_VertexID)
{
    const float planetRadius =
        g_pc.g_planet.x;

    const float observerRadius =
        g_pc.g_planet.y;

    const float spacing =
        g_pc.g_planet.z;

    const uint resolution =
        (uint)round(g_pc.g_planet.w);

    // Index-free grid: SV_VertexID directly encodes (cell, corner)
    // instead of a unique grid vertex resolved through an index
    // buffer, so each of the 6 corners per cell runs the full vertex
    // shader independently. Trades some redundant ALU (no shared-
    // vertex reuse across triangles) for zero index-buffer storage
    // or rebuild cost.
    const uint cellsPerAxis =
        resolution - 1u;

    const uint cellIndex =
        vertexId / 6u;

    const uint cornerIndex =
        vertexId % 6u;

    const uint cellX =
        cellIndex % cellsPerAxis;

    const uint cellY =
        cellIndex / cellsPerAxis;

    // Matches the winding of the original CPU-built index buffer:
    // triangle 0 = (0,0)-(0,1)-(1,0), triangle 1 = (1,0)-(0,1)-(1,1).
    uint2 cornerOffset = uint2(0u, 0u);

    if (cornerIndex == 0u) cornerOffset = uint2(0u, 0u);
    else if (cornerIndex == 1u) cornerOffset = uint2(0u, 1u);
    else if (cornerIndex == 2u) cornerOffset = uint2(1u, 0u);
    else if (cornerIndex == 3u) cornerOffset = uint2(1u, 0u);
    else if (cornerIndex == 4u) cornerOffset = uint2(0u, 1u);
    else cornerOffset = uint2(1u, 1u);

    const uint logicalX =
        cellX + cornerOffset.x;

    const uint logicalY =
        cellY + cornerOffset.y;

    const uint originX =
        (uint)round(
            g_pc.g_centerUpAndOriginX.w);

    const uint originY =
        (uint)round(
            g_pc.g_centerEastAndOriginY.w);

    const uint physicalIndex =
        PhysicalSampleIndex(
            logicalX,
            logicalY,
            resolution,
            originX,
            originY);

    const uint sampleByteOffset =
        physicalIndex * 32u;

    const float waterDepth = asfloat(g_samples.Load(sampleByteOffset + 20u));
    // Bed + depth is linear through page filtering and parent morphing.
    // Land, ocean and lakes therefore use exactly the same triangles.
    const float elevation = asfloat(g_samples.Load(sampleByteOffset)) + waterDepth;

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

    float2 localOffsetMeters =
        (float2(
            (float)logicalX,
            (float)logicalY) -
         halfCells) *
        spacing;

    float2 offsetMeters =
        g_pc.g_centerOffsetMeters +
        localOffsetMeters;

    const float morphStart =
        g_pc.g_centerNorthAndMorphStart.w;

    const float morphEnd =
        g_pc.g_morph.x;

    const float hasCoarser =
        g_pc.g_morph.z;

    if (hasCoarser > 0.5)
    {
        const float edgeDistance =
            max(
                abs(localOffsetMeters.x),
                abs(localOffsetMeters.y));

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

    const float innerHoleHalfExtentMeters =
        g_pc.g_morph.w;

    const float2 innerHoleCenterOffsetMeters =
        float2(
            g_pc.g_morph.y,
            g_pc.g_debug.w);

    // The coarse ring overlaps the finer patch. Sink only the innermost
    // coarse cell under that overlap, then fade smoothly back to the true
    // surface. This prevents z-fighting/tiny raster cracks without changing
    // the authoritative terrain or creating a visible broad depression.
    float seamSinkMeters = 0.0;

    if (innerHoleHalfExtentMeters > 0.0)
    {
        const float holeDistance =
            max(
                abs(
                    localOffsetMeters.x -
                    innerHoleCenterOffsetMeters.x),
                abs(
                    localOffsetMeters.y -
                    innerHoleCenterOffsetMeters.y));

        const float seamT =
            saturate(
                (holeDistance -
                 innerHoleHalfExtentMeters) /
                max(spacing, 0.0001));

        const float seamWeight =
            1.0 -
            seamT * seamT *
                (3.0 - 2.0 * seamT);

        seamSinkMeters =
            min(
                spacing * 0.05,
                8.0) *
            seamWeight;
    }

    const float3 surfaceDirection =
        SurfaceDirectionForOffset(
            offsetMeters,
            planetRadius);

    const float displacedElevation =
        elevation -
        seamSinkMeters;

    const float displacedRadius =
        planetRadius +
        displacedElevation;

    // Do not form Y as direction.y * ~6,000,000 - ~6,000,000.
    // At near-field angles direction.y rounds extremely close to 1 and that
    // subtraction quantizes away the centimeter/sub-meter spherical drop.
    // x/z retain sin(theta) accurately, so recover cos(theta)-1 through the
    // cancellation-free identity:
    //
    //   cos(theta) - 1 = -sin^2(theta) / (1 + cos(theta)).
    //
    // Visible terrain is on the observer-facing hemisphere. For a negative
    // cosine (already beyond the horizon) the ordinary expression is far from
    // the cancellation regime and is safe.
    const float sinSquaredFromObserver =
        saturate(
            surfaceDirection.x *
                surfaceDirection.x +
            surfaceDirection.z *
                surfaceDirection.z);

    const float positiveCosine =
        sqrt(
            max(
                1.0 -
                    sinSquaredFromObserver,
                0.0));

    const float cosineMinusOne =
        surfaceDirection.y >= 0.0
            ? -sinSquaredFromObserver /
                max(
                    1.0 +
                        positiveCosine,
                    0.000001)
            : surfaceDirection.y -
                1.0;

    const float observerAltitude =
        observerRadius -
        planetRadius;

    const float3 localPosition =
        float3(
            surfaceDirection.x *
                displacedRadius,
            planetRadius *
                    cosineMinusOne +
                displacedElevation *
                    surfaceDirection.y -
                observerAltitude,
            surfaceDirection.z *
                displacedRadius);

    // Ground-truth slope baked in at the finest active level's own
    // resolution (see LoadFineSlope) -- not a finite difference across
    // this ring's own (possibly much wider) neighboring samples, so
    // distant rings shade with real L0 micro-relief instead of a
    // faceted/pixelated normal.
    const float2 fineSlope =
        LoadFineSlope(
            logicalX,
            logicalY,
            resolution,
            originX,
            originY);

    const float slopeEast = fineSlope.x;
    const float slopeNorth = fineSlope.y;

    float3 tangentEast =
        g_pc.g_centerEastAndOriginY.xyz -
        surfaceDirection *
            dot(
                g_pc.g_centerEastAndOriginY.xyz,
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
        g_pc.g_centerNorthAndMorphStart.xyz -
        surfaceDirection *
            dot(
                g_pc.g_centerNorthAndMorphStart.xyz,
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
            g_pc.g_mvp);

    output.elevation = elevation;
    output.waterDepth = waterDepth;
    output.localPosition = localPosition;
    // Disables the shared pixel shader's fake detail-normal bump --
    // see the comment on these fields in VSOutput above.
    output.spacingMeters = 0.0;
    output.worldPosition = float3(0.0, 0.0, 0.0);

    output.biome0 =
        UnpackUnorm4x8(
            packedBiome0);

    output.biome1 =
        UnpackUnorm4x8(
            packedBiome1);

    // F2 debug menu: LOD lattice inspection. Overrides the real
    // biome weights with a one-hot vector selecting one of the 8
    // palette colors the pixel shader already knows how to blend
    // (see kTerrainSurfacePixelShader) -- cycling through them by
    // level index, so two adjacent rings never land on the same
    // color, with zero changes needed to the (shared) pixel shader.
    if (g_pc.g_debug.y > 0.5)
    {
        const uint colorIndex =
            ((uint)round(g_pc.g_debug.x)) % 8u;

        output.biome0 = float4(
            colorIndex == 0u ? 1.0 : 0.0,
            colorIndex == 1u ? 1.0 : 0.0,
            colorIndex == 2u ? 1.0 : 0.0,
            colorIndex == 3u ? 1.0 : 0.0);

        output.biome1 = float4(
            colorIndex == 4u ? 1.0 : 0.0,
            colorIndex == 5u ? 1.0 : 0.0,
            colorIndex == 6u ? 1.0 : 0.0,
            colorIndex == 7u ? 1.0 : 0.0);
    }

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
        0.000002 +
        positiveReliefPadding;

    // F2 debug menu: side cut. Discards (via the same clip-distance
    // mechanism the horizon test already uses -- SV_ClipDistance0 is
    // negative here) everything on one side of a vertical plane
    // through the observer, so the clipmap's LOD ring structure is
    // visible in cross-section instead of occluded by the near side.
    if (g_pc.g_debug.z > 0.5 &&
        localPosition.x < 0.0)
    {
        output.horizonClip = -1.0;
    }

    // Reject complete hole cells explicitly through the existing clip
    // distance instead of manufacturing a homogeneous (0,0,0,0) vertex.
    // The cell-center predicate is shared by all six invocations for a cell,
    // so no triangle is partially clipped by this hole test.
    const float cellCenterX =
        ((float)cellX +
         0.5 -
         (float)cellsPerAxis *
             0.5) *
        spacing;

    const float cellCenterY =
        ((float)cellY +
         0.5 -
         (float)cellsPerAxis *
             0.5) *
        spacing;

    const bool insideHole =
        innerHoleHalfExtentMeters >
            0.0 &&
        abs(
            cellCenterX -
            innerHoleCenterOffsetMeters.x) <
            innerHoleHalfExtentMeters &&
        abs(
            cellCenterY -
            innerHoleCenterOffsetMeters.y) <
            innerHoleHalfExtentMeters;

    if (insideHole)
    {
        output.horizonClip = -1.0;
    }

    return output;
}
)";

constexpr auto kPixelShader = detail::kTerrainSurfacePixelShader;
} // namespace

class TerrainPreviewRenderer::Impl
{
public:
    Impl(
        rhi::Device& device,
        const shader::Compiler& shaderCompiler,
        const world::PlanetDefinition& planet,
        terrain_gpu::GpuFieldGenerator& gpuFieldGenerator,
        const world::WorldPosition& observer,
        TerrainPreviewConfig config,
        terrain_gpu::GpuRegionDelta* regionDeltaComposite,
        terrain_region::DerivedTerrainRegionCache* hydrologyRegionCache)
        : device_(device),
          planet_(planet),
          gpuFieldGenerator_(gpuFieldGenerator),
          regionDeltaComposite_(regionDeltaComposite),
          hydrologyRegionCache_(hydrologyRegionCache),
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

    ~Impl() = default;

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

    void SetDebugVisuals(
        const bool lodColorEnabled,
        const bool sideCutEnabled) noexcept
    {
        debugLodColorEnabled_ = lodColorEnabled;
        debugSideCutEnabled_ = sideCutEnabled;
    }

    void SetGenerationFrozen(
        const bool frozen) noexcept
    {
        generationFrozen_ = frozen;
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
        if (stats_.rebaseCount > 0)
        {
            stats_.secondsSinceLastRebase = std::chrono::duration<f64>(
                std::chrono::steady_clock::now() - lastRebaseTime_).count();
        }
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

            const terrain_view::ClipmapLevelMotion*
                finerMotion =
                    levelIndex > 0U
                        ? &motion_.levels[
                            levelIndex - 1U]
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
                    finerMotion,
                    residencyUpdate_.levels[
                        levelIndex],
                    observerFrame_,
                    levelIndex,
                    debugLodColorEnabled_,
                    debugSideCutEnabled_);

            commandList.
                SetGraphicsConstants(
                    constants);

            commandList.
                SetGraphicsBuffer(
                    0,
                    *levels_[levelIndex].
                        frameGpuSampleBuffers[
                            frameIndex]);

            // Same vertex count for the center patch and every ring
            // -- the vertex shader itself decides, per invocation,
            // whether it's inside level 0's full grid or a ring's
            // hole (see g_morph.w / innerHoleHalfExtentMeters).
            commandList.Draw(
                patchVertexCount_);

            ++stats_.drawCallsLastFrame;
        }

        stats_.cumulativeUploadedBytes +=
            stats_.uploadedBytesLastFrame;
    }

    // Index-free rendering: every one of these is a vertex shader
    // invocation (SV_VertexID), not a unique stored vertex -- there
    // is no index buffer any more, so IndexCount() is always 0.
    [[nodiscard]] u32 VertexCount() const noexcept
    {
        const u64 total =
            static_cast<u64>(
                patchVertexCount_) *
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
        return 0U;
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

    // A single dirty-region batch recorded against a level, carrying the
    // full request it came from (surface frame/origin/spacing can all
    // change between updates, e.g. on recentring) so it can be replayed
    // as a GPU dispatch independently for each frame-in-flight ring slot
    // whenever that slot's own catch-up serial falls behind.
    struct DirtyUpdate
    {
        u64 serial{0};
        terrain_stream::TerrainSampleRequest request;
    };

    struct LevelGpuState
    {
        std::vector<
            std::unique_ptr<rhi::Buffer>>
            frameGpuSampleBuffers;

        std::vector<u64> frameSerials;
        std::deque<DirtyUpdate> dirtyUpdates;
        u64 currentSerial{0};
    };

    void CreateSharedTopology()
    {
        // Index-free: every patch (center or ring) shares the same
        // resolution, so a single vertex count covers all of them --
        // SV_VertexID alone determines which cell/corner/hole status
        // a given invocation belongs to (see the vertex shader).
        const u64 cellsPerAxis =
            static_cast<u64>(
                config_.clipmap.
                    gridResolution) -
            1ULL;

        const u64 vertexCount =
            cellsPerAxis *
            cellsPerAxis *
            6ULL;

        patchVertexCount_ =
            static_cast<u32>(
                std::min<u64>(
                    vertexCount,
                    std::numeric_limits<
                        u32>::max()));
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
            level.frameSerials.assign(
                config_.framesInFlight,
                0);

            level.frameGpuSampleBuffers.reserve(
                config_.framesInFlight);

            for (u32 frameIndex = 0;
                 frameIndex <
                    config_.framesInFlight;
                 ++frameIndex)
            {
                // BufferUsage::Structured already carries
                // VK_BUFFER_USAGE_STORAGE_BUFFER_BIT unconditionally
                // (see VulkanResources.cpp), so this same buffer is
                // both the compute shader's UAV write target
                // (PrepareLevelFrame) and the vertex shader's SRV read
                // (Draw) -- no separate upload/staging buffer needed
                // now that generation happens on the GPU.
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
                    .pushConstantDwords = 42,
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
                    : tracker_.Reconfigured(candidateConfig),
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

        terrain_stream::RefreshTerrainMorphRegions(
            candidate.layout,
            candidate.motion,
            candidate.residencyUpdate);

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
                // Shading normals always resolve the finest active
                // level's own ground detail, however coarse this
                // particular ring's own geometry is -- see the
                // comment on TerrainSampleRequest.
                .fineNormalFootprintMeters =
                    candidate.layout.levels[0].
                        sampleSpacingMeters,
                .fineNormalEpsilonMeters =
                    candidate.layout.levels[0].
                        sampleSpacingMeters,
                .centerOffsetMeters =
                    candidate.motion.levels[
                        levelIndex].
                        centerOffsetMeters,
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
        CandidateState&& candidate)
    {
        ResetCommitStats();

        const bool coverageTierChanged =
            candidate.coverageTier !=
                activeCoverageTier_;

        const auto fullLevels = static_cast<u32>(std::count_if(
            candidate.residencyUpdate.levels.begin(), candidate.residencyUpdate.levels.end(),
            [](const auto& level) { return level.fullRefresh; }));
        if (stats_.committedBatches > 0 && fullLevels > 0)
        {
            ++stats_.rebaseCount;
            stats_.lastRebaseLevels = fullLevels;
            stats_.lastRebaseReason = coverageTierChanged ? "LOD" : "MOVE";
            lastRebaseTime_ = std::chrono::steady_clock::now();
            stats_.secondsSinceLastRebase = 0.0;
        }

        // Requests describe the (still-pending) work, not generated
        // output -- record them as dirty now, but the actual GPU
        // dispatches happen later, lazily, in PrepareLevelFrame as each
        // frame-in-flight ring slot comes due for its own draw.
        const std::vector<terrain_stream::TerrainSampleRequest>
            requests = std::move(candidate.requests);

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

        for (const auto& request : requests)
        {
            if (request.regions.empty())
            {
                continue;
            }

            ++stats_.
                levelsTouchedLastUpdate;

            stats_.
                refreshedRegionsLastUpdate +=
                    static_cast<u32>(
                        request.regions.size());

            u64 requestSampleCount = 0;
            for (const auto& region : request.regions)
            {
                requestSampleCount +=
                    static_cast<u64>(region.width) * region.height;
            }

            stats_.
                generatedSamplesLastUpdate +=
                    requestSampleCount;

            RecordDirtyRequest(request);
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

    // Unlike the old CPU-job path, there is no asynchronous batch to
    // poll here: BuildCandidate's requests are recorded as dirty
    // immediately, and the actual GPU generation is deferred to
    // PrepareLevelFrame, which runs on the very next Draw() before any
    // vertex shader reads the buffer it just wrote -- so committing
    // "blocking" or from steady-state streaming is the same operation.
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

        CandidateState candidate =
            BuildCandidate(observer);

        CommitCandidate(
            std::move(candidate));

        committedGeneration_ =
            desiredGeneration_;

        ++stats_.committedBatches;
        stats_.updatePending = false;
    }

    void ServiceStreaming()
    {
        // F2 debug menu: freeze generation. Observer motion still
        // tracks normally (so the camera can keep moving to inspect
        // the frozen lattice from any angle) -- only committing new
        // dirty regions is suppressed, leaving desiredGeneration_
        // ahead of committedGeneration_ until unfrozen.
        if (generationFrozen_)
        {
            return;
        }

        if (committedGeneration_ ==
            desiredGeneration_)
        {
            return;
        }

        CandidateState candidate =
            BuildCandidate(
                desiredObserver_);

        CommitCandidate(
            std::move(candidate));

        committedGeneration_ =
            desiredGeneration_;

        ++stats_.committedBatches;
        stats_.updatePending = false;
    }

    void RecordDirtyRequest(
        const terrain_stream::
            TerrainSampleRequest& request)
    {
        if (request.levelIndex >=
            levels_.size())
        {
            throw std::out_of_range(
                "Orbit terrain sample request references an invalid clipmap level.");
        }

        LevelGpuState& state =
            levels_[request.levelIndex];

        ++state.currentSerial;

        state.dirtyUpdates.push_back({
            .serial = state.currentSerial,
            .request = request
        });
    }

    // Unlike the old CPU path (which memcpy'd from one shared,
    // already-generated cpuSamples mirror into each ring slot's own
    // upload buffer), there is no CPU-side mirror any more -- each of
    // the framesInFlight ring buffers independently replays whichever
    // dirty requests it hasn't caught up to yet as real GPU dispatches.
    // A ring slot that lagged behind several updates redoes several
    // dispatches the first time it's used again, exactly mirroring how
    // the CPU path redid several regions' worth of memcpy/CopyBuffer in
    // that same situation.
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

        u64 generatedBytes = 0;
        bool touchedBuffer = false;

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
                 update.request.regions)
            {
                if (!touchedBuffer)
                {
                    commandList.Transition(
                        gpuBuffer,
                        rhi::ResourceState::
                            ShaderResource,
                        rhi::ResourceState::
                            UnorderedAccess);
                    touchedBuffer = true;
                }

                terrain_gpu::GpuFieldRequest
                    gpuRequest{
                        .resolution =
                            update.request.
                                resolution,
                        .spacingMeters =
                            update.request.
                                spacingMeters,
                        .footprintMeters =
                            update.request.
                                footprintMeters,
                        .morphToCoarser =
                            update.request.
                                morphToCoarser,
                        .morphStartHalfExtentMeters =
                            update.request.
                                morphStartHalfExtentMeters,
                        .morphEndHalfExtentMeters =
                            update.request.
                                morphEndHalfExtentMeters,
                        .coarseSpacingMeters =
                            update.request.
                                coarseSpacingMeters,
                        .coarseFootprintMeters =
                            update.request.
                                coarseFootprintMeters,
                        .fineNormalFootprintMeters =
                            update.request.
                                fineNormalFootprintMeters,
                        .fineNormalEpsilonMeters =
                            update.request.
                                fineNormalEpsilonMeters,
                        .centerOffsetMeters =
                            update.request.
                                centerOffsetMeters,
                        .surfaceFrame =
                            update.request.
                                surfaceFrame,
                        .coarseSurfaceFrame =
                            update.request.
                                coarseSurfaceFrame,
                        .originX =
                            update.request.
                                originX,
                        .originY =
                            update.request.
                                originY,
                        .region = {
                            .x = region.x,
                            .y = region.y,
                            .width = region.width,
                            .height = region.height
                        }
                    };

                gpuFieldGenerator_.Dispatch(
                    commandList,
                    gpuRequest,
                    gpuBuffer);

                CompositeRegionDelta(
                    commandList,
                    update.request,
                    region,
                    gpuBuffer);

                generatedBytes +=
                    static_cast<u64>(
                        region.width) *
                    region.height *
                    sizeof(
                        terrain_stream::
                            TerrainSampleValue);
            }
        }

        if (touchedBuffer)
        {
            commandList.UavBarrier(
                gpuBuffer);

            commandList.Transition(
                gpuBuffer,
                rhi::ResourceState::
                    UnorderedAccess,
                rhi::ResourceState::
                    ShaderResource);
        }

        frameSerial =
            state.currentSerial;

        PruneDirtyHistory(state);

        return generatedBytes;
    }

    // No-op when regionDeltaComposite_/hydrologyRegionCache_ are null
    // (the default). Otherwise, composites whichever hydrology region
    // tile covers `request`'s surface frame -- if one is ready -- into
    // the samples this Dispatch call just wrote for `region`, in
    // place. `gpuBuffer` must already be
    // ResourceState::UnorderedAccess (true here: the caller only ever
    // calls this immediately after gpuFieldGenerator_.Dispatch, still
    // inside the same UAV window).
    void CompositeRegionDelta(
        rhi::CommandList& commandList,
        const terrain_stream::TerrainSampleRequest& request,
        const terrain_stream::PhysicalRegion& region,
        rhi::Buffer& gpuBuffer)
    {
        if (regionDeltaComposite_ == nullptr ||
            hydrologyRegionCache_ == nullptr)
        {
            return;
        }

        const terrain_region::DerivedTerrainRegionId regionId =
            hydrologyRegionCache_->IdForDirection(
                world::DirectionAtSurfaceOffset(
                    planet_,
                    request.surfaceFrame,
                    request.centerOffsetMeters));

        const auto hydrologyRegion =
            hydrologyRegionCache_->TryGet(regionId);

        if (!hydrologyRegion)
        {
            return;
        }

        rhi::Buffer* deltaBuffer =
            GetOrCreateRegionDeltaBuffer(*hydrologyRegion);

        if (deltaBuffer == nullptr)
        {
            return;
        }

        terrain_gpu::GpuRegionDeltaRequest deltaRequest{};
        deltaRequest.resolution = request.resolution;
        deltaRequest.spacingMeters = request.spacingMeters;
        deltaRequest.surfaceFrame = request.surfaceFrame;
        deltaRequest.centerOffsetMeters =
            request.centerOffsetMeters;
        deltaRequest.originX = request.originX;
        deltaRequest.originY = request.originY;
        deltaRequest.region = {
            .x = region.x,
            .y = region.y,
            .width = region.width,
            .height = region.height
        };
        deltaRequest.planetRadiusMeters = planet_.radiusMeters;
        deltaRequest.regionSurfaceFrame =
            hydrologyRegion->elevationDelta.surfaceFrame;
        deltaRequest.regionHalfExtentMeters =
            hydrologyRegion->elevationDelta.halfExtentMeters;
        deltaRequest.regionSpacingMeters =
            hydrologyRegion->elevationDelta.spacingMeters;
        deltaRequest.regionResolution =
            hydrologyRegion->elevationDelta.resolution;
        deltaRequest.edgeFadeStartDot = 0.75F;

        regionDeltaComposite_->Dispatch(
            commandList, deltaRequest, *deltaBuffer, gpuBuffer);
    }

    [[nodiscard]] rhi::Buffer* GetOrCreateRegionDeltaBuffer(
        const terrain_region::DerivedTerrainRegion& hydrologyRegion)
    {
        const auto existing =
            regionDeltaBuffers_.find(hydrologyRegion.id);

        if (existing != regionDeltaBuffers_.end())
        {
            return existing->second.get();
        }

        const auto& deltas =
            hydrologyRegion.elevationDelta.elevationDeltaMeters;

        if (deltas.empty())
        {
            return nullptr;
        }

        auto buffer = device_.CreateBuffer({
            .sizeBytes = deltas.size() * sizeof(f32),
            .usage = rhi::BufferUsage::Structured,
            .memory = rhi::MemoryUsage::HostVisible,
            .initialState = rhi::ResourceState::ShaderResource
        });

        std::byte* mapped = buffer->Map();
        std::memcpy(
            mapped, deltas.data(), deltas.size() * sizeof(f32));
        buffer->Unmap();

        rhi::Buffer* ptr = buffer.get();

        regionDeltaBuffers_.emplace(
            hydrologyRegion.id, std::move(buffer));

        return ptr;
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
    terrain_gpu::GpuFieldGenerator&
        gpuFieldGenerator_;

    // Optional GPU hydrology compositing -- see the constructor's own
    // comment. Null means "off"; every use below is guarded on this.
    terrain_gpu::GpuRegionDelta* regionDeltaComposite_{nullptr};
    terrain_region::DerivedTerrainRegionCache*
        hydrologyRegionCache_{nullptr};

    // Lazily uploaded once per ready region tile (its elevation-delta
    // field never changes after BuildDerivedTerrainRegionFromGpuReadback
    // produces it, so one upload covers every clipmap dispatch that
    // tile ever composites into) -- HostVisible so no transfer/copy
    // dispatch is needed, matching GpuFieldGenerator's own
    // platesBuffer_/hotspotsBuffer_ convention. Reset whenever a
    // region's entry is replaced (new sourceRevision/generatorVersion),
    // never otherwise -- this cache can only grow across a run, but
    // stays bounded by the region cache's own maxEntries in practice.
    std::unordered_map<
        terrain_region::DerivedTerrainRegionId,
        std::unique_ptr<rhi::Buffer>,
        terrain_region::DerivedTerrainRegionIdHash>
        regionDeltaBuffers_;

    TerrainPreviewConfig config_;
    terrain_view::ClipmapConfig
        baseClipmapConfig_{};
    terrain_view::ClipmapLayout layout_;
    terrain_view::ClipmapTracker tracker_;
    terrain_stream::ToroidalResidency
        residency_;

    std::vector<LevelGpuState> levels_;

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

    // F2 debug menu state -- see SetDebugVisuals/SetGenerationFrozen.
    bool debugLodColorEnabled_{false};
    bool debugSideCutEnabled_{false};
    bool generationFrozen_{false};

    u64 desiredGeneration_{0};
    u64 committedGeneration_{0};

    u32 activeCoverageTier_{0};
    u32 desiredCoverageTier_{0};

    TerrainStreamingStats stats_{};
    std::chrono::steady_clock::time_point lastRebaseTime_{};

    // Vertex-shader invocation count for one patch (center or ring,
    // identical for both -- see CreateSharedTopology).
    u32 patchVertexCount_{0};
};

TerrainPreviewRenderer::
TerrainPreviewRenderer(
    rhi::Device& device,
    const shader::Compiler& shaderCompiler,
    const world::PlanetDefinition& planet,
    terrain_gpu::GpuFieldGenerator& gpuFieldGenerator,
    const world::WorldPosition& observer,
    TerrainPreviewConfig config,
    terrain_gpu::GpuRegionDelta* regionDeltaComposite,
    terrain_region::DerivedTerrainRegionCache* hydrologyRegionCache)
    : impl_(std::make_unique<Impl>(
        device,
        shaderCompiler,
        planet,
        gpuFieldGenerator,
        observer,
        std::move(config),
        regionDeltaComposite,
        hydrologyRegionCache))
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

void TerrainPreviewRenderer::SetDebugVisuals(
    const bool lodColorEnabled,
    const bool sideCutEnabled)
{
    impl_->SetDebugVisuals(lodColorEnabled, sideCutEnabled);
}

void TerrainPreviewRenderer::SetGenerationFrozen(
    const bool frozen)
{
    impl_->SetGenerationFrozen(frozen);
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
