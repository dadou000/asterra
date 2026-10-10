#include <orbit/volume_render/UniversalVolumeRenderer.hpp>
#include <orbit/volume_representation/VolumeCache.hpp>

#include <algorithm>
#include <cstdio>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace orbit::volume_render
{
namespace
{
using volume_representation::RepresentationDecision;
using volume_representation::ResolvedRepresentation;

[[nodiscard]] volume_representation::VolumeRepresentationService&
RepresentationService()
{
    static volume_representation::VolumeRepresentationService service;
    return service;
}

[[nodiscard]] std::map<scene::ObjectId, RepresentationDecision>&
LatestDecisions()
{
    static std::map<scene::ObjectId, RepresentationDecision> decisions;
    return decisions;
}

[[nodiscard]] render_graph::BufferHandle FindField(
    const volume_fields::ImportedVolumeFields& fields,
    const world_model::VolumeField field) noexcept
{
    for (const auto& channel : fields.channels)
    {
        if (channel.field == field)
        {
            return channel.buffer;
        }
    }

    return {};
}

[[nodiscard]] const volume_fields::FieldChannelInfo*
FindFieldInfo(
    const volume_fields::VolumeFieldDiagnostics& diagnostics,
    const world_model::VolumeField field) noexcept
{
    for (const auto& channel : diagnostics.channels)
    {
        if (channel.field == field)
        {
            return &channel;
        }
    }

    return nullptr;
}

[[nodiscard]] f32 Saturate(
    const f64 value) noexcept
{
    return static_cast<f32>(
        std::clamp(value, 0.0, 1.0));
}

[[nodiscard]] f32 DetailedProceduralNoise(
    const math::Double3 position) noexcept
{
    const f64 a =
        std::sin(
            position.x * 0.071 +
            position.z * 0.053);
    const f64 b =
        std::sin(
            position.y * 0.119 -
            position.x * 0.037 +
            1.71);
    const f64 c =
        std::cos(
            position.z * 0.101 +
            position.y * 0.041 -
            0.63);

    return static_cast<f32>(
        std::clamp(
            0.62 +
                0.16 * a +
                0.13 * b +
                0.09 * c,
            0.12,
            1.0));
}

struct ProceduralSample
{
    f32 density{0.0F};
    f32 emission{0.0F};
};

[[nodiscard]] ProceduralSample SampleProceduralRepresentation(
    const world_model::ResolvedVolumeDomain& domain,
    const math::Double3 worldPosition,
    const f32 passiveBlend) noexcept
{
    // M37 caches are authoritative only when Baked is explicitly selected.
    // They are resampled into whatever bounded M31 allocation is active, so
    // asset resolution does not dictate runtime residency or tile dimensions.
    if (domain.representationMode ==
        world_model::VolumeRepresentationMode::Baked)
    {
        if (const auto* cache =
                volume_representation::VolumeCaches().Find(
                    domain.object);
            cache != nullptr)
        {
            const math::Double3 minimum =
                domain.centerMeters -
                domain.halfExtentsMeters;
            const math::Double3 size =
                domain.halfExtentsMeters * 2.0;

            const f64 u =
                std::abs(size.x) > 1.0e-9
                    ? (worldPosition.x - minimum.x) /
                        size.x
                    : 0.5;
            const f64 v =
                std::abs(size.y) > 1.0e-9
                    ? (worldPosition.y - minimum.y) /
                        size.y
                    : 0.5;
            const f64 w =
                std::abs(size.z) > 1.0e-9
                    ? (worldPosition.z - minimum.z) /
                        size.z
                    : 0.5;

            if (u < 0.0 || u > 1.0 ||
                v < 0.0 || v > 1.0 ||
                w < 0.0 || w > 1.0)
            {
                return {};
            }

            return {
                .density =
                    volume_representation::
                        SampleVolumeCacheDensity(
                            *cache,
                            u,
                            v,
                            w),
                .emission =
                    volume_representation::
                        SampleVolumeCacheEmission(
                            *cache,
                            u,
                            v,
                            w)
            };
        }
    }

    const math::Double3 half{
        std::max(
            std::abs(domain.halfExtentsMeters.x),
            1.0e-6),
        std::max(
            std::abs(domain.halfExtentsMeters.y),
            1.0e-6),
        std::max(
            std::abs(domain.halfExtentsMeters.z),
            1.0e-6)
    };

    const math::Double3 local{
        (worldPosition.x - domain.centerMeters.x) /
            half.x,
        (worldPosition.y - domain.centerMeters.y) /
            half.y,
        (worldPosition.z - domain.centerMeters.z) /
            half.z
    };

    const f64 maximumAxis =
        std::max({
            std::abs(local.x),
            std::abs(local.y),
            std::abs(local.z)
        });

    if (maximumAxis >= 1.0)
    {
        return {};
    }

    const f32 edge =
        Saturate(
            1.0 - maximumAxis);
    const f32 softEdge =
        edge * edge *
        (3.0F - 2.0F * edge);

    const f32 blend =
        std::clamp(
            passiveBlend,
            0.0F,
            1.0F);
    const f32 detailedNoise =
        DetailedProceduralNoise(
            worldPosition);
    const f32 noise =
        detailedNoise *
            (1.0F - blend) +
        0.78F * blend;
    const f32 height =
        Saturate(
            local.y * 0.5 + 0.5);

    f32 density = 0.0F;
    f32 emission = 0.0F;

    if (domain.preset == "Fog")
    {
        density =
            0.48F * softEdge *
            (0.82F + 0.18F * noise);
    }
    else if (domain.preset == "Smoke")
    {
        density =
            0.82F * softEdge * noise *
            (1.08F - 0.36F * height);
    }
    else if (domain.preset == "Fire")
    {
        density =
            0.66F * softEdge * noise *
            (1.15F - 0.45F * height);
        emission =
            density *
            (0.65F + 0.75F * height);
    }
    else if (domain.preset == "Dust")
    {
        density =
            0.58F * softEdge *
            (0.68F + 0.32F * noise) *
            (1.12F - 0.28F * height);
    }
    else if (domain.preset == "Snow")
    {
        density =
            0.31F * softEdge *
            (0.72F + 0.28F * noise);
    }
    else if (domain.preset == "Surface Flow")
    {
        const f32 surfaceBand =
            std::exp(
                -static_cast<f32>(
                    std::abs(local.y)) *
                8.0F);
        density =
            0.62F * softEdge *
            surfaceBand * noise;
    }
    else if (domain.preset != "Empty")
    {
        density =
            0.42F * softEdge * noise;
    }

    return {
        .density =
            std::max(
                density,
                0.0F),
        .emission =
            std::max(
                emission,
                0.0F)
    };
}

void AddProceduralFieldUpload(
    render_graph::RenderGraph& graph,
    const std::string_view prefix,
    const world_model::ResolvedVolumeDomain& domain,
    volume_fields::VolumeFieldStorage& storage,
    const volume_fields::ImportedVolumeFields& fields,
    const f32 passiveBlend)
{
    const auto diagnostics =
        storage.Diagnostics();
    const auto tiles =
        storage.Tiles();

    const auto densityHandle =
        FindField(
            fields,
            world_model::VolumeField::Density);
    const auto emissionHandle =
        FindField(
            fields,
            world_model::VolumeField::Emission);

    if (!densityHandle.IsValid())
    {
        return;
    }

    const auto* densityInfo =
        FindFieldInfo(
            diagnostics,
            world_model::VolumeField::Density);
    const auto* emissionInfo =
        FindFieldInfo(
            diagnostics,
            world_model::VolumeField::Emission);

    if (densityInfo == nullptr)
    {
        return;
    }

    const u32 tileEdge =
        std::max(
            diagnostics.tileEdge,
            1U);
    const u32 tileCellCount =
        tileEdge * tileEdge * tileEdge;

    const math::Double3 cellSize{
        domain.halfExtentsMeters.x * 2.0 /
            static_cast<f64>(
                std::max(
                    diagnostics.resolutionX,
                    1U)),
        domain.halfExtentsMeters.y * 2.0 /
            static_cast<f64>(
                std::max(
                    diagnostics.resolutionY,
                    1U)),
        domain.halfExtentsMeters.z * 2.0 /
            static_cast<f64>(
                std::max(
                    diagnostics.resolutionZ,
                    1U))
    };

    // A baked cache is static data: refilling and re-uploading the whole field
    // every frame (67 MB at 256^3) is only needed when its inputs change. The
    // field buffers persist between frames, so an unchanged key skips both.
    if (domain.representationMode ==
        world_model::VolumeRepresentationMode::Baked)
    {
        if (const auto* cache =
                volume_representation::VolumeCaches().Find(domain.object);
            cache != nullptr)
        {
            u64 key = 0xcbf29ce484222325ULL;
            const auto mix = [&key](const u64 value)
            {
                key ^= value + 0x9e3779b97f4a7c15ULL + (key << 6U) + (key >> 2U);
            };
            const auto mixDouble = [&mix](const f64 value)
            {
                mix(std::bit_cast<u64>(value));
            };
            mix(cache->payloadFingerprint);
            mix(cache->density.size());
            mixDouble(domain.centerMeters.x);
            mixDouble(domain.centerMeters.y);
            mixDouble(domain.centerMeters.z);
            mixDouble(domain.halfExtentsMeters.x);
            mixDouble(domain.halfExtentsMeters.y);
            mixDouble(domain.halfExtentsMeters.z);
            mixDouble(storage.GridAnchor().x);
            mixDouble(storage.GridAnchor().y);
            mixDouble(storage.GridAnchor().z);
            mix(diagnostics.resolutionX);
            mix(diagnostics.resolutionY);
            mix(diagnostics.resolutionZ);
            mix(tileEdge);
            mix(densityInfo->sizeBytes);
            mix(emissionHandle.IsValid() ? 1U : 0U);
            u64 residentCount = 0U;
            for (const auto& tile : tiles)
            {
                if (tile.resident)
                {
                    ++residentCount;
                    mix(static_cast<u64>(tile.slot));
                }
            }
            mix(residentCount);

            static std::map<scene::ObjectId, u64> uploaded;
            const auto found = uploaded.find(domain.object);
            if (found != uploaded.end() && found->second == key)
            {
                return;
            }
            uploaded.insert_or_assign(domain.object, key);
        }
    }

    std::vector<f32> densityValues(
        static_cast<std::size_t>(
            densityInfo->sizeBytes /
            sizeof(f32)),
        0.0F);

    constexpr f64 kOccupiedDensity = 0.004;
    math::Double3 occupiedMinimum{1.0e300, 1.0e300, 1.0e300};
    math::Double3 occupiedMaximum{-1.0e300, -1.0e300, -1.0e300};

    std::vector<f32> emissionValues;

    if (emissionHandle.IsValid() &&
        emissionInfo != nullptr)
    {
        emissionValues.assign(
            static_cast<std::size_t>(
                emissionInfo->sizeBytes /
                sizeof(f32)),
            0.0F);
    }

    for (const auto& tile : tiles)
    {
        if (!tile.resident)
        {
            continue;
        }

        for (u32 z = 0U;
             z < tileEdge;
             ++z)
        {
            for (u32 y = 0U;
                 y < tileEdge;
                 ++y)
            {
                for (u32 x = 0U;
                     x < tileEdge;
                     ++x)
                {
                    const u32 localIndex =
                        z * tileEdge * tileEdge +
                        y * tileEdge +
                        x;
                    const u64 physicalIndex =
                        static_cast<u64>(tile.slot) *
                            tileCellCount +
                        localIndex;

                    if (physicalIndex >=
                        densityValues.size())
                    {
                        continue;
                    }

                    // Tile coordinates are absolute on the world-aligned cell
                    // grid (VolumeFieldStorage tiles start at
                    // floor(minimum / tileSize), and the raymarch shader reads
                    // floor(position / tileSize)), not offsets from the
                    // domain minimum. Offsetting by the minimum only worked
                    // for a volume centred on the origin.
                    const math::Double3 gridAnchor = storage.GridAnchor();
                    const math::Double3 worldPosition{
                        gridAnchor.x +
                        (static_cast<f64>(tile.coord.x) *
                             tileEdge +
                         static_cast<f64>(x) + 0.5) *
                            cellSize.x,
                        gridAnchor.y +
                        (static_cast<f64>(tile.coord.y) *
                             tileEdge +
                         static_cast<f64>(y) + 0.5) *
                            cellSize.y,
                        gridAnchor.z +
                        (static_cast<f64>(tile.coord.z) *
                             tileEdge +
                         static_cast<f64>(z) + 0.5) *
                            cellSize.z
                    };

                    const auto sample =
                        SampleProceduralRepresentation(
                            domain,
                            worldPosition,
                            passiveBlend);

                    densityValues[
                        static_cast<std::size_t>(
                            physicalIndex)] =
                                sample.density;

                    if (sample.density > kOccupiedDensity)
                    {
                        occupiedMinimum = {
                            std::min(occupiedMinimum.x, worldPosition.x),
                            std::min(occupiedMinimum.y, worldPosition.y),
                            std::min(occupiedMinimum.z, worldPosition.z)};
                        occupiedMaximum = {
                            std::max(occupiedMaximum.x, worldPosition.x),
                            std::max(occupiedMaximum.y, worldPosition.y),
                            std::max(occupiedMaximum.z, worldPosition.z)};
                    }

                    if (!emissionValues.empty() &&
                        physicalIndex <
                            emissionValues.size())
                    {
                        emissionValues[
                            static_cast<std::size_t>(
                                physicalIndex)] =
                                    sample.emission;
                    }
                }
            }
        }
    }

    const auto upload =
        [&](const std::string& name,
            const render_graph::BufferHandle target,
            const std::vector<f32>& values)
        {
            if (!target.IsValid() ||
                values.empty())
            {
                return;
            }

            const u64 bytes =
                static_cast<u64>(
                    values.size()) *
                sizeof(f32);

            const auto source =
                graph.CreateBuffer(
                    std::string(prefix) + name,
                    {
                        .sizeBytes = bytes,
                        .usage =
                            rhi::BufferUsage::Structured,
                        .memory =
                            rhi::MemoryUsage::HostVisible,
                        .initialState =
                            rhi::ResourceState::CopySource
                    });

            auto& uploadBuffer =
                graph.Buffer(source);
            auto* mapped =
                uploadBuffer.Map();
            std::memcpy(
                mapped,
                values.data(),
                static_cast<std::size_t>(bytes));
            uploadBuffer.Unmap();

            graph.AddPass(
                std::string(prefix) +
                    name + ".Commit",
                {},
                {
                    {
                        .buffer = source,
                        .state =
                            rhi::ResourceState::CopySource,
                        .access =
                            render_graph::Access::Read
                    },
                    {
                        .buffer = target,
                        .state =
                            rhi::ResourceState::CopyDestination,
                        .access =
                            render_graph::Access::Write
                    }
                },
                [source, target, bytes](
                    rhi::CommandList& commands,
                    const render_graph::Resources& resources)
                {
                    commands.CopyBuffer(
                        resources.Buffer(source),
                        0U,
                        resources.Buffer(target),
                        0U,
                        bytes);
                });
        };

    upload(
        ".ProceduralDensity",
        densityHandle,
        densityValues);

    if (!emissionValues.empty())
    {
        upload(
            ".ProceduralEmission",
            emissionHandle,
            emissionValues);
    }

    if (occupiedMaximum.x >= occupiedMinimum.x)
    {
        // A cell centre holds density: the cloud reaches half a cell past it,
        // and the trilinear read another half, so pad by two cells.
        storage.SetOccupiedBounds(
            {occupiedMinimum.x - 2.0 * cellSize.x,
             occupiedMinimum.y - 2.0 * cellSize.y,
             occupiedMinimum.z - 2.0 * cellSize.z},
            {occupiedMaximum.x + 2.0 * cellSize.x,
             occupiedMaximum.y + 2.0 * cellSize.y,
             occupiedMaximum.z + 2.0 * cellSize.z});
    }
    else
    {
        // Nothing to draw: an empty box, so no ray enters it.
        storage.SetOccupiedBounds({0.0, 0.0, 0.0}, {0.0, 0.0, 0.0});
    }

    storage.MarkAllResidentTilesValid();
}
} // namespace

void UniversalVolumeRenderer::AddPasses(
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
    const auto& runtime =
        Settings(domain.object);

    volume_representation::RepresentationInput input{
        .volume = domain.object,
        .volumeCenterInFrameMeters =
            domain.centerMeters,
        .halfExtentsMeters =
            domain.halfExtentsMeters,
        .observerInFrameMeters =
            lightingView.cameraPositionInFrameMeters,
        .followTarget =
            runtime.followTarget,
        .followObject =
            runtime.followObject,
        .followObjectPositionInFrameMeters =
            runtime.
                followObjectPositionInFrameMeters,
        .stableFrame =
            lightingView.frame.high ^
            std::rotl(
                lightingView.frame.low,
                17),
        .stableBody =
            lightingView.body.high ^
            std::rotl(
                lightingView.body.low,
                29),
        .viewportHeightPixels = height,
        .verticalFovRadians =
            camera.verticalFovRadians,
        .authoredMode =
            domain.representationMode,
        .liveDistanceMeters =
            runtime.liveDistanceMeters,
        .passiveDistanceMeters =
            runtime.passiveDistanceMeters,
        .liveProjectedPixels =
            runtime.liveProjectedPixels,
        .passiveProjectedPixels =
            runtime.passiveProjectedPixels,
        .hysteresisFraction =
            runtime.hysteresisFraction,
        .bakedAvailable =
            volume_representation::VolumeCaches().Has(
                domain.object)
    };

    auto decision =
        RepresentationService().Resolve(
            viewportId,
            input);

    LatestDecisions().insert_or_assign(
        domain.object,
        decision);

    volume_representation::VolumeRepresentationSettings policy{
        .followTarget =
            runtime.followTarget,
        .followObject =
            runtime.followObject,
        .followObjectPositionInFrameMeters =
            runtime.
                followObjectPositionInFrameMeters,
        .liveDistanceMeters =
            runtime.liveDistanceMeters,
        .passiveDistanceMeters =
            runtime.passiveDistanceMeters,
        .liveProjectedPixels =
            runtime.liveProjectedPixels,
        .passiveProjectedPixels =
            runtime.passiveProjectedPixels,
        .hysteresisFraction =
            runtime.hysteresisFraction,
        .coarseResolution =
            runtime.coarseResolution,
        .passiveResolution =
            runtime.passiveResolution,
        .coarseRaymarchSteps =
            runtime.coarseRaymarchSteps,
        .passiveRaymarchSteps =
            runtime.passiveRaymarchSteps
    };

    // The field store and solver are shared by all Studio viewports. Use the
    // primary viewport as their single representation/allocation authority;
    // secondary views may choose cheaper shading locally but must not replace
    // the primary view's field layout or center.
    const bool ownsSharedVolumeFields =
        viewportId == "studio.primary";

    if (ownsSharedVolumeFields)
    {
        volume_representation::PublishAllocationPolicy(
            domain.object,
            decision,
            policy);
    }

    auto renderDomain = domain;
    renderDomain.centerMeters =
        decision.runtimeCenterInFrameMeters;

    const bool proceduralAggregate =
        decision.liveWeight <= 0.001F &&
        (decision.representation ==
             ResolvedRepresentation::Coarse ||
         decision.representation ==
             ResolvedRepresentation::Passive ||
         decision.representation ==
             ResolvedRepresentation::Baked);

    if (ownsSharedVolumeFields)
    {
        // A static aggregate lays its tile grid on its own minimum corner; a
        // live volume keeps the world-aligned grid its solver scrolls through.
        storage.SetGridAnchor(
            proceduralAggregate
                ? renderDomain.centerMeters - renderDomain.halfExtentsMeters
                : math::Double3{},
            proceduralAggregate);
    }

    if (proceduralAggregate && ownsSharedVolumeFields)
    {
        AddProceduralFieldUpload(
            graph,
            prefix,
            renderDomain,
            storage,
            fields,
            decision.passiveWeight);

        if (decision.representation ==
            ResolvedRepresentation::Coarse)
        {
            renderDomain.renderSteps =
                std::clamp(
                    runtime.coarseRaymarchSteps,
                    8U,
                    96U);
            renderDomain.shadowSteps =
                std::min(
                    renderDomain.shadowSteps,
                    4U);
            renderDomain.temporalWeight =
                std::max(
                    renderDomain.temporalWeight,
                    0.88F);
        }
        else
        {
            renderDomain.renderSteps =
                std::clamp(
                    runtime.passiveRaymarchSteps,
                    4U,
                    32U);
            renderDomain.shadowSteps =
                std::min(
                    renderDomain.shadowSteps,
                    2U);
            renderDomain.temporalWeight =
                std::max(
                    renderDomain.temporalWeight,
                    0.93F);
        }
    }

    // Do not flush M35 history merely because the representation tier changed.
    // Radiance/transmittance rejection already bounds stale history, while
    // preserving it smooths the live/coarse/passive/baked energy handoff.
    AddLivePasses(
        graph,
        prefix,
        viewportId,
        sceneColor,
        depth,
        width,
        height,
        camera,
        lightingView,
        renderDomain,
        storage,
        fields,
        stellar,
        localLights,
        radianceCells,
        radianceLevels,
        radianceLevelCount,
        particleLightGrid,
        resetHistory,
        stellarColorTop,
        atmosphereLuts);
}

VolumeRenderDiagnostics
UniversalVolumeRenderer::Diagnostics(
    const scene::ObjectId volume) const noexcept
{
    auto result =
        LiveDiagnostics(volume);

    const auto found =
        LatestDecisions().find(volume);

    if (found ==
        LatestDecisions().end())
    {
        return result;
    }

    const auto& decision =
        found->second;

    result.representation =
        decision.representation;
    result.previousRepresentation =
        decision.previousRepresentation;
    result.liveWeight =
        decision.liveWeight;
    result.coarseWeight =
        decision.coarseWeight;
    result.passiveWeight =
        decision.passiveWeight;
    result.bakedWeight =
        decision.bakedWeight;
    result.runtimeCenterInFrameMeters =
        decision.runtimeCenterInFrameMeters;
    result.distanceToBoundsMeters =
        decision.distanceToBoundsMeters;
    result.projectedDiameterPixels =
        decision.projectedDiameterPixels;
    result.representationForced =
        decision.forced;
    result.representationTransition =
        decision.transitionActive;
    result.denseFieldRequired =
        decision.denseFieldRequired;
    result.bakedFallback =
        decision.bakedFallback;
    result.followTargetResolved =
        decision.followTargetResolved;
    result.stableAddressFingerprint =
        decision.stableAddressFingerprint;

    return result;
}
} // namespace orbit::volume_render
