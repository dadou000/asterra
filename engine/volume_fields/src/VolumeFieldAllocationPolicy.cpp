#include <orbit/volume_fields/VolumeFieldStorage.hpp>
#include <orbit/volume_representation/VolumeRepresentation.hpp>

#include <algorithm>

namespace orbit::volume_fields
{
namespace
{
[[nodiscard]] volume_representation::PublishedAllocationPolicy
FallbackPolicy(
    const world_model::ResolvedVolumeDomain& domain) noexcept
{
    using volume_representation::ResolvedRepresentation;

    volume_representation::PublishedAllocationPolicy result{
        .representation = ResolvedRepresentation::Live,
        .denseFieldRequired = true,
        .coarseResolution = 24U,
        .passiveResolution = 8U,
        .runtimeCenterInFrameMeters = domain.centerMeters,
        .hasRuntimeCenter = false
    };

    switch (domain.representationMode)
    {
    case world_model::VolumeRepresentationMode::Live:
    case world_model::VolumeRepresentationMode::Auto:
        break;

    case world_model::VolumeRepresentationMode::Coarse:
        result.representation =
            ResolvedRepresentation::Coarse;
        result.denseFieldRequired = false;
        break;

    case world_model::VolumeRepresentationMode::Passive:
    case world_model::VolumeRepresentationMode::Baked:
        result.representation =
            ResolvedRepresentation::Passive;
        result.denseFieldRequired = false;
        break;
    }

    return result;
}
} // namespace

VolumeFieldStorage& VolumeFieldStorageService::Ensure(
    const world_model::ResolvedVolumeDomain& domain)
{
    auto adjusted = domain;

    const auto published =
        volume_representation::AllocationPolicy(
            domain.object);

    const auto policy =
        published.has_value()
            ? *published
            : FallbackPolicy(domain);

    if (policy.hasRuntimeCenter)
    {
        // M36 follows a runtime target without changing authored semantic
        // coordinates. EnsureBase/Reconfigure then reuses overlapping M31 tile
        // slots at the new frame-space center.
        adjusted.centerMeters =
            policy.runtimeCenterInFrameMeters;
    }

    if (!policy.denseFieldRequired)
    {
        const bool coarse =
            policy.representation ==
                volume_representation::
                    ResolvedRepresentation::Coarse;

        const u32 requested =
            coarse
                ? policy.coarseResolution
                : policy.passiveResolution;

        adjusted.resolution =
            std::min(
                domain.resolution,
                std::max(requested, 8U));

        if (adjusted.solverPolicy ==
            world_model::VolumeSolverPolicy::Surface2D5D)
        {
            adjusted.surfaceLayers =
                coarse
                    ? std::min(
                          domain.surfaceLayers,
                          2U)
                    : 1U;
        }

        // Coarse/passive representations are rendered from deterministic
        // procedural aggregate fields. Simulation-only channels do not survive
        // into the far allocation, so a huge authored effect remains bounded.
        constexpr u64 densityBit =
            static_cast<u64>(
                world_model::VolumeField::Density);
        constexpr u64 emissionBit =
            static_cast<u64>(
                world_model::VolumeField::Emission);

        adjusted.fieldMask =
            densityBit |
            (domain.fieldMask & emissionBit);
    }

    return EnsureBase(adjusted);
}
} // namespace orbit::volume_fields
