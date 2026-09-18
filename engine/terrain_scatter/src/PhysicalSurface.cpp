#include <orbit/terrain_scatter/PhysicalSurface.hpp>

namespace orbit::terrain_scatter
{
bool PhysicalSurfaceScatterInput::BedrockExposed() const noexcept
{
    return
        material ==
            terrain_material_column::
                ExposedSurfaceKind::
                    Bedrock;
}

PhysicalSurfaceScatterInput MakePhysicalSurfaceScatterInput(
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
} // namespace orbit::terrain_scatter
