#pragma once

#include <orbit/terrain_material_column/SurfaceResolver.hpp>

namespace orbit::terrain_render
{
// Renderer-facing physical surface input. It is deliberately constructed only
// from the canonical M18 state so rendering cannot independently select rock
// identity from biome weights.
struct PhysicalSurfaceRenderInput
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

[[nodiscard]] PhysicalSurfaceRenderInput MakePhysicalSurfaceRenderInput(
    const terrain_material_column::ExposedSurfaceState& surface) noexcept;
} // namespace orbit::terrain_render
