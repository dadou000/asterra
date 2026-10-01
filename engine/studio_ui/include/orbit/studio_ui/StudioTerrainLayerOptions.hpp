#pragma once

#include <orbit/core/Types.hpp>

namespace orbit::studio_ui
{
// Which terrain layers a view draws and how much LOD detail it asks for.
// Transient presentation state (like the diagnostic overlays): none of it
// enters terrain identity, persistence or generation.
struct StudioTerrainLayerOptions
{
    // Near-field production clipmap terrain.
    bool productionSurface{true};
    // Orbital displaced-globe patches.
    bool macroGlobe{true};
    // Ocean surface (sea-level water) on the terrain and on the orbital globe.
    bool ocean{true};
    // Volume surface-effect stamps applied to the terrain.
    bool surfaceEffects{true};
    // LOD bias in stops, clamped to [-4, 4]. +1 keeps richer representations
    // and asks the orbital patches for twice the resolution; -1 the opposite.
    f32 lodBiasStops{0.0F};

    [[nodiscard]] constexpr bool operator==(
        const StudioTerrainLayerOptions&) const noexcept = default;
};
} // namespace orbit::studio_ui
