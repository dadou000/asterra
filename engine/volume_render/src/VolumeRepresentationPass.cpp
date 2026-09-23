#include <orbit/volume_render/UniversalVolumeRenderer.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <map>
#include <numbers>
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

[[nodiscard]] f32 Saturate(const f64 value) noexcept
{
    return static_cast<f32>(
        std::clamp(value, 0.0, 1.0));
}

[[nodiscard]] f32 ProceduralNoise(
    const math::Double3 position,
    const bool passive) noexcept
{
    if (passive)
    {
        return 0.78F;
    }

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
    const ResolvedRepresentation representation) noexcept
{
    const math::Double3 half{
        std::max(std::abs(domain.halfExtentsMeters.x), 1.0e-6),
        std::max(std::abs(domain.halfExtentsMeters.y), 1.0e-6),
        std::max(std::abs(domain.halfExtentsMeters.z), 1.0e-6)
    };

    const math::Double3 local{
        (worldPosition.x - domain.centerMeters.x) / half.x,
        (worldPosition.y - domain.centerMeters.y) / half.y,
        (worldPosition.z - domain.centerMeters.z) / half.z
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
        Saturate(1.0 - maximumAxis);
    const f32 softEdge =
        edge * edge * (3.0F - 2.0F * edge);
    const bool passive =
        representation == ResolvedRepresentation::Passive ||
        representation == ResolvedRepresentation::Baked;
    const f32 noise =
        ProceduralNoise(
            worldPosition,
            passive);
    const f32 height =
        Saturate(local.y * 0.5 + 0.5);

    f32 density = 0.0F;
    f32 emission = 0.0F;

    if (domain.preset == "Fog")
    {
        density =
            0.48F * softEdge *
            (passive ? 1.0F : 0.82F + 0.18F * noise);
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
        .density = std::max(density, 0.0F),
        .emission = std::max(emission, 0.0F)
    };
}

void AddProceduralFieldUpload(
    render_graph::RenderGraph& graph,
    const std::string_view prefix,
    const world_model::ResolvedVolumeDomain& domain,
    volume_fields::VolumeFieldStorage& storage,
    const volume_fields::ImportedVolumeFields& fields,
    const ResolvedRepresentation representation)
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
        std::max(diagnostics.tileEdge, 1U);
    const u32 tileCellCount =
        tileEdge * tileEdge * tileEdge;

    const math::Double3 cellSize{
        domain.halfExtentsMeters.x * 2.0 /
            static_cast<f64>(
                std::max(diagnostics.resolutionX, 1U)),
        domain.halfExtentsMeters.y * 2.0 /
            static_cast<f64>(
                std::max(diagnostics.resolutionY, 1U)),
        domain.halfExtentsMeters.z * 2.0 /
            static_cast<f64>(
                std::max(diagnostics.resolutionZ, 1U))
    };

    std::vector<f32> densityValues(
        static_cast<std::size_t>(
            densityInfo->sizeBytes /
            sizeof(f32)),
        0.0F);

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

        for (u32 z = 0U; z < tileEdge; ++z)
        {
            for (u32 y = 0U; y < tileEdge; ++y)
            {
                for (u32 x = 0U; x < tileEdge; ++x)
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

                    const math::Double3 worldPosition{
                        (static_cast<f64>(tile.coord.x) * tileEdge +
                         static_cast<f64>(x) + 0.5) *
                            cellSize.x,
                        (static_cast<f64>(tile.coord.y) * tileEdge +
                         static_cast<f64>(y) + 0.5) *
                            cellSize.y,
                        (static_cast<f64>(tile.coord.z) * tileEdge +
                         static_cast<f64>(z) + 0.5) *
                            cellSize.z
                    };

                    const auto sample =
                        SampleProceduralRepresentation(
                            domain,
                            worldPosition,
                            representation);

                    densityValues[
                        static_cast<std::size_t>(
                            physicalIndex)] =
                                sample.density;

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

    storage.MarkAllResidentTilesValid();
}

[[nodiscard]] u64 StableIdFingerprint(
    const core::StrongId<frames::FrameIdTag> id) noexcept
{
    return id.high ^ std::rotl(id.low, 17);
}

[[nodiscard]] u64 StableIdFingerprint(
    const core::StrongId<universe::BodyIdTag> id) noexcept
{
    return id.high ^ std::rotl(id.low, 29);
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
    const bool resetHistory)
{
    const auto& runtime =
        Settings(domain.object);

    volume_representation::RepresentationInput input{
        .volume = domain.object,
        .volumeCenterInFrameMeters =
            runtime.followTarget ==
                    volume_representation::FollowTarget::Camera
                ? lightingView.cameraPositionInFrameMeters
                : domain.centerMeters,
        .halfExtentsMeters =
            domain.halfExtentsMeters,
        .observerInFrameMeters =
            lightingView.cameraPositionInFrameMeters,
        .stableFrame =
            lightingView.frame.high ^
            std::rotl(lightingView.frame.low, 17),
        .stableBody =
            lightingView.body.high ^
            std::rotl(lightingView.body.low, 29),
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
        .bakedAvailable = false
    };

    auto decision =
        RepresentationService().Resolve(
            viewportId,
            input);

    LatestDecisions().insert_or_assign(
        domain.object,
        decision);

    volume_representation::VolumeRepresentationSettings policy{
        .followTarget = runtime.followTarget,
        .liveDistanceMeters = runtime.liveDistanceMeters,
        .passiveDistanceMeters = runtime.passiveDistanceMeters,
        .liveProjectedPixels = runtime.liveProjectedPixels,
        .passiveProjectedPixels = runtime.passiveProjectedPixels,
        .hysteresisFraction = runtime.hysteresisFraction,
        .coarseResolution = runtime.coarseResolution,
        .passiveResolution = runtime.passiveResolution,
        .coarseRaymarchSteps = runtime.coarseRaymarchSteps,
        .passiveRaymarchSteps = runtime.passiveRaymarchSteps
    };

    volume_representation::PublishAllocationPolicy(
        domain.object,
        decision,
        policy);

    auto renderDomain = domain;

    if (decision.representation ==
            ResolvedRepresentation::Coarse ||
        decision.representation ==
            ResolvedRepresentation::Passive ||
        decision.representation ==
            ResolvedRepresentation::Baked)
    {
        AddProceduralFieldUpload(
            graph,
            prefix,
            domain,
            storage,
            fields,
            decision.representation);

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
        resetHistory ||
            decision.representation !=
                decision.previousRepresentation);
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
    result.stableAddressFingerprint =
        decision.stableAddressFingerprint;

    return result;
}
} // namespace orbit::volume_render
