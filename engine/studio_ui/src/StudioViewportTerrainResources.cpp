#include "StudioViewportInternals.hpp"

#include <orbit/terrain_water/LakeWater.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace orbit::studio_ui::viewport_detail
{
namespace
{
[[nodiscard]] f32 RiverWaterDepthAt(
    const terrain_erosion::RiverNetwork* network,
    const u32 x,
    const u32 y,
    const u32 resolution,
    const f64 spacingMeters,
    const f32 bedElevationMeters)
{
    if (network == nullptr || network->resolution != resolution ||
        network->spacingMeters <= 0.0 || network->segments.empty())
        return 0.0F;

    const f64 half = static_cast<f64>(resolution - 1U) * 0.5;
    const math::Double2 point{
        (static_cast<f64>(x) - half) * spacingMeters,
        (static_cast<f64>(y) - half) * spacingMeters};
    f64 waterSurface = -std::numeric_limits<f64>::infinity();

    for (const auto& segment : network->segments)
    {
        if (!segment.active || segment.upstreamNode >= network->nodes.size() ||
            segment.downstreamNode >= network->nodes.size())
            continue;
        const auto& a = network->nodes[segment.upstreamNode];
        const auto& b = network->nodes[segment.downstreamNode];
        const f64 dx = b.channelOffsetMeters.x - a.channelOffsetMeters.x;
        const f64 dy = b.channelOffsetMeters.y - a.channelOffsetMeters.y;
        const f64 lengthSquared = dx * dx + dy * dy;
        const f64 t = lengthSquared > 1.0e-8
            ? std::clamp(((point.x - a.channelOffsetMeters.x) * dx +
                          (point.y - a.channelOffsetMeters.y) * dy) / lengthSquared, 0.0, 1.0)
            : 0.0;
        const f64 nearestX = a.channelOffsetMeters.x + dx * t;
        const f64 nearestY = a.channelOffsetMeters.y + dy * t;
        const f64 distance = std::hypot(point.x - nearestX, point.y - nearestY);
        const f64 width = std::max(
            0.5 * (static_cast<f64>(a.channelWidthMeters) + b.channelWidthMeters),
            0.0);
        // Include the nearest cell footprint so sub-cell streams remain visible at coarse LOD.
        if (distance > width * 0.5 + spacingMeters * 0.55)
            continue;

        const f64 bankElevation =
            (1.0 - t) * static_cast<f64>(a.surfaceHeightMeters) +
            t * static_cast<f64>(b.surfaceHeightMeters);
        const f64 channelDepth = std::max(
            (1.0 - t) * static_cast<f64>(a.channelDepthMeters) +
            t * static_cast<f64>(b.channelDepthMeters),
            0.0);
        waterSurface = std::max(waterSurface, bankElevation - channelDepth * 0.18);
    }

    return std::isfinite(waterSurface)
        ? static_cast<f32>(std::max(waterSurface - bedElevationMeters, 0.0))
        : 0.0F;
}
} // namespace

[[nodiscard]] bool SameClipmapConfig(
    const terrain_view::ClipmapConfig& a,
    const terrain_view::ClipmapConfig& b) noexcept
{
    return
        a.levelCount == b.levelCount &&
        a.gridResolution == b.gridResolution &&
        a.baseSpacingMeters == b.baseSpacingMeters &&
        a.levelScale == b.levelScale &&
        a.overlapCells == b.overlapCells &&
        a.coarseGridResolution == b.coarseGridResolution &&
        a.coarseMinSpacingMeters == b.coarseMinSpacingMeters &&
        a.bandCount == b.bandCount &&
        a.bandEdgesMeters == b.bandEdgesMeters &&
        a.bandExtentMargin == b.bandExtentMargin &&
        a.bandZoneFraction == b.bandZoneFraction &&
        a.bandPartialUpdates == b.bandPartialUpdates;
}

[[nodiscard]] terrain_view::ClipmapConfig EffectiveClipmapConfig(
    const terrain_view::ClipmapConfig& base,
    const StudioTerrainLayerOptions& layers)
{
    if (!layers.fullClipmap || !layers.experimentalDistanceBands)
    {
        return base;
    }
    terrain_view::ClipmapConfig banded = base;
    banded.bandCount = 0U;
    banded.bandEdgesMeters = {};
    // Quantised so dragging the scale does not rebuild the renderer every frame.
    const f64 scale = std::exp2(
        std::round(std::log2(static_cast<f64>(layers.clipmapBandScale)) * 8.0) / 8.0);
    f64 previous = 0.0;
    for (const f32 rawEdge : layers.clipmapBandEdgesMeters)
    {
        const f64 edge = static_cast<f64>(rawEdge) * scale;
        if (rawEdge <= 0.0F || edge <= previous)
        {
            break;
        }
        banded.bandEdgesMeters[banded.bandCount++] = edge;
        previous = edge;
    }
    if (banded.bandCount < 2U)
    {
        return base;
    }
    banded.bandPartialUpdates = layers.clipmapPartialUpdates;
    banded.gridResolution = 513U;
    banded.coarseGridResolution = 0U;
    banded.levelCount = banded.bandCount;
    return banded;
}

[[nodiscard]] terrain_render::TerrainPreviewCamera
TerrainCameraFromBodyCamera(
    const render_view::CameraState& camera,
    const world::SurfaceFrame& frame)
{
    const math::Double3 forward{
        static_cast<f64>(camera.forward.x),
        static_cast<f64>(camera.forward.y),
        static_cast<f64>(camera.forward.z)
    };

    const math::Double3 up{
        static_cast<f64>(camera.up.x),
        static_cast<f64>(camera.up.y),
        static_cast<f64>(camera.up.z)
    };

    auto localForward =
        math::Float3{
            static_cast<f32>(
                math::Dot(
                    forward,
                    frame.east)),
            static_cast<f32>(
                math::Dot(
                    forward,
                    frame.up)),
            static_cast<f32>(
                math::Dot(
                    forward,
                    frame.north))
        };

    auto localUp =
        math::Float3{
            static_cast<f32>(
                math::Dot(
                    up,
                    frame.east)),
            static_cast<f32>(
                math::Dot(
                    up,
                    frame.up)),
            static_cast<f32>(
                math::Dot(
                    up,
                    frame.north))
        };

    if (math::LengthSquared(localForward) <=
        1.0e-8F)
    {
        localForward = {
            0.0F,
            -0.28F,
            1.0F
        };
    }

    if (math::LengthSquared(localUp) <=
        1.0e-8F)
    {
        localUp = {
            0.0F,
            1.0F,
            0.0F
        };
    }

    return {
        .forward =
            math::Normalize(
                localForward),
        .up =
            math::Normalize(
                localUp),
        .verticalFovRadians = camera.verticalFovRadians,
        .nearPlaneMeters = camera.nearPlaneMeters,
        .farPlaneMeters = camera.farPlaneMeters
    };
}

[[nodiscard]] StudioPhysicalRenderPages
BuildPhysicalRenderPages(
    studio_session::StudioSession& session,
    const studio_session::StudioTerrainViewportRuntimeSnapshot& runtime,
    const terrain::AnalyticTerrainSource& analytic,
    rhi::Device& device)
{
    StudioPhysicalRenderPages result{};

    auto* services =
        session.World().
            Surfaces().
            ServicesForBody(
                runtime.body);

    if (services == nullptr)
    {
        return result;
    }

    std::array<
        terrain::PhysicalTerrainPageAddress,
        5U>
        addresses{};

    addresses[0] =
        runtime.observerPhysicalPage;

    constexpr std::array<
        world::TileEdge,
        4U>
        edges{
            world::TileEdge::North,
            world::TileEdge::East,
            world::TileEdge::South,
            world::TileEdge::West
        };

    for (u32 index = 0U;
         index < edges.size();
         ++index)
    {
        const auto neighbor =
            world::NeighborAcrossTileEdge(
                runtime.
                    observerPhysicalPage.
                    tile,
                edges[index]);

        addresses[index + 1U] = {
            .planet =
                runtime.
                    observerPhysicalPage.
                    planet,
            .tile =
                neighbor.tile
        };
    }

    auto& cache =
        services->Cache();

    const f64 seaLevelMeters =
        analytic.
            Description().
            global.
            seaLevelMeters;

    for (const auto& address :
         addresses)
    {
        const auto snapshot =
            session.
                TerrainPhysicalPages().
                Find(
                    address);

        if (snapshot == nullptr ||
            snapshot->material == nullptr)
        {
            continue;
        }

        const auto status =
            session.
                TerrainPhysicalPages().
                PageStatus(
                    address);

        if (!status.has_value() ||
            status->revisionFingerprint !=
                snapshot->
                    revisionFingerprint)
        {
            continue;
        }

        const terrain_gpu::
            PersistentGpuTerrainCacheKey
            key{
                .address =
                    address,
                .physicalLod =
                    snapshot->
                        physicalLod,
                .revisions =
                    snapshot->
                        revisions
            };

        auto cached =
            cache.Find(
                key);

        const auto physicalProduct =
            terrain_gpu::
                ProductBit(
                    terrain_gpu::
                        CachedTerrainProduct::
                            PhysicalSurface);

        if (cached == nullptr ||
            (cached->products &
             physicalProduct) == 0U ||
            cached->physicalSurface ==
                nullptr)
        {
            const auto uploadRevision =
                session.
                    TerrainPhysicalPages().
                    BeginUpload(
                        address);

            if (!uploadRevision.has_value() ||
                *uploadRevision !=
                    snapshot->
                        revisionFingerprint)
            {
                if (uploadRevision.has_value())
                {
                    static_cast<void>(
                        session.
                            TerrainPhysicalPages().
                            CompleteUpload(
                                address,
                                *uploadRevision,
                                false,
                                "M12 physical snapshot changed before GPU upload."));
                }

                continue;
            }

            try
            {
                const u32 resolution =
                    snapshot->material->
                        Resolution();

                std::vector<
                    terrain_gpu::
                        GpuPhysicalSurfaceTexel>
                    texels(
                        static_cast<
                            std::size_t>(
                                resolution) *
                        resolution);

                for (u32 y = 0U;
                     y < resolution;
                     ++y)
                {
                    for (u32 x = 0U;
                         x < resolution;
                         ++x)
                    {
                        const auto& cell =
                            snapshot->
                                material->
                                At(
                                    x,
                                    y);

                        const f32 elevation =
                            cell.
                                SurfaceHeightMeters();

                        texels[
                            static_cast<
                                std::size_t>(
                                    y) *
                                resolution +
                            x] = {
                                .elevationMeters =
                                    elevation,
                                .standingWaterDepthMeters =
                                    std::max({
                                        static_cast<f32>(std::max(
                                            seaLevelMeters - static_cast<f64>(elevation),
                                            0.0)),
                                        RiverWaterDepthAt(
                                            snapshot->rivers.get(), x, y, resolution,
                                            snapshot->material->SpacingMeters(), elevation),
                                        snapshot->lakes != nullptr
                                            ? static_cast<f32>(terrain_water::SampleLakeWater(
                                                *snapshot->lakes,
                                                {(static_cast<f64>(x) -
                                                  static_cast<f64>(resolution - 1U) * 0.5) *
                                                     snapshot->material->SpacingMeters(),
                                                 (static_cast<f64>(y) -
                                                  static_cast<f64>(resolution - 1U) * 0.5) *
                                                     snapshot->material->SpacingMeters()})
                                                   .depthMeters)
                                            : 0.0F})
                            };
                    }
                }

                auto uniqueBuffer =
                    device.CreateBuffer({
                        .sizeBytes =
                            static_cast<u64>(
                                texels.size()) *
                            sizeof(
                                terrain_gpu::
                                    GpuPhysicalSurfaceTexel),
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

                std::shared_ptr<
                    rhi::Buffer>
                    buffer{
                        std::move(
                            uniqueBuffer)};

                std::byte* mapped =
                    buffer->Map();

                std::memcpy(
                    mapped,
                    texels.data(),
                    texels.size() *
                        sizeof(
                            terrain_gpu::
                                GpuPhysicalSurfaceTexel));

                buffer->Unmap();

                auto augmented =
                    cached != nullptr
                        ? std::make_shared<
                              terrain_gpu::
                                  CachedGpuTerrainPage>(
                                      *cached)
                        : std::make_shared<
                              terrain_gpu::
                                  CachedGpuTerrainPage>();

                augmented->products |=
                    physicalProduct;

                augmented->physicalSurface =
                    std::move(
                        buffer);

                cache.Insert(
                    key,
                    augmented);

                cached =
                    std::move(
                        augmented);

                if (!session.
                        TerrainPhysicalPages().
                        CompleteUpload(
                            address,
                            *uploadRevision,
                            true))
                {
                    static_cast<void>(
                        cache.Erase(
                            key));
                    cached.reset();
                    continue;
                }
            }
            catch (const std::exception& error)
            {
                static_cast<void>(
                    session.
                        TerrainPhysicalPages().
                        CompleteUpload(
                            address,
                            *uploadRevision,
                            false,
                            error.what()));
                continue;
            }
        }

        if ((cached->products &
             physicalProduct) == 0U ||
            cached->physicalSurface ==
                nullptr)
        {
            continue;
        }

        result.pages.push_back({
            .address =
                address,
            .resolution =
                snapshot->
                    material->
                    Resolution(),
            .samples =
                cached->
                    physicalSurface
        });

        result.generation =
            terrain::StableCombine64(
                result.generation,
                terrain_gpu::
                    PersistentGpuTerrainCacheFingerprint(
                        key));

        result.generation =
            terrain::StableCombine64(
                result.generation,
                snapshot->
                    revisionFingerprint);
    }

    return result;
}
} // namespace orbit::studio_ui::viewport_detail
