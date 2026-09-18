#include <orbit/terrain_render/PhysicalSurface.hpp>

namespace orbit::terrain_render
{
bool PhysicalSurfaceRenderInput::BedrockExposed() const noexcept
{
    return
        material ==
            terrain_material_column::
                ExposedSurfaceKind::
                    Bedrock;
}

PhysicalSurfaceRenderInput MakePhysicalSurfaceRenderInput(
    const terrain_material_column::ExposedSurfaceState& surface) noexcept
{
    return {
        .material =
            surface.material,
        .substrateRock =
            surface.substrateRock,
        .exposedRock =
            surface.exposedRock,
        .exposedLayerDepthMeters =
            surface.
                exposedLayerDepthMeters,
        .moisture =
            surface.moisture,
        .standingWaterDepthMeters =
            surface.
                standingWaterDepthMeters,
        .snowDepthMeters =
            surface.snowDepthMeters
    };
}
} // namespace orbit::terrain_render
