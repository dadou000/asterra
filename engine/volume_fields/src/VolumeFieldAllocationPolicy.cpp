#include <orbit/volume_fields/VolumeFieldStorage.hpp>
#include <orbit/volume_representation/VolumeRepresentation.hpp>

#include <algorithm>

namespace orbit::volume_fields
{
VolumeFieldStorage& VolumeFieldStorageService::Ensure(
    const world_model::ResolvedVolumeDomain& domain)
{
    auto adjusted = domain;

    if (const auto policy =
            volume_representation::AllocationPolicy(
                domain.object);
        policy.has_value() &&
        !policy->denseFieldRequired)
    {
        const bool coarse =
            policy->representation ==
                volume_representation::
                    ResolvedRepresentation::Coarse;

        const u32 requested =
            coarse
                ? policy->coarseResolution
                : policy->passiveResolution;

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
        // procedural aggregate fields. Do not carry simulation-only channels
        // into the far allocation.
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
