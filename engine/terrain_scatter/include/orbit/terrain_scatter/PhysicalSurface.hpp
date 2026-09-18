#pragma once

#include <orbit/terrain_material_column/SurfaceResolver.hpp>

namespace orbit::terrain_scatter
{
// Physical substrate context supplied to future biome/scatter placement. M18
// intentionally contains no biome rules; M19+ can filter this shared physical
// state but cannot replace its geological identity.
struct PhysicalSurfaceScatterInput
{
    terrain_material_column::ExposedSurfaceKind material{
        terrain_material_column::ExposedSurfaceKind::Bedrock};

    terrain_geology::RockTypeId substrateRock{};
    terrain_geology::RockTypeId exposedRock{};

    f32 exposedLayerDepthMeters{0.0F};
    f32 moisture{0.0F};
    f32 standingWaterDepthMeters{0.0F};
    f32 snowDepthMeters{0.0F};

    [[nodiscard]] bool BedrockExposed() const noexcept;
};

[[nodiscard]] PhysicalSurfaceScatterInput MakePhysicalSurfaceScatterInput(
    const terrain_material_column::ExposedSurfaceState& surface) noexcept;
} // namespace orbit::terrain_scatter
