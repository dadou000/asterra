#pragma once

#include <orbit/core/Types.hpp>

#include <array>
#include <vector>

namespace orbit::studio_ui
{
// Which terrain layers a view draws and how much LOD detail it asks for.
// Transient presentation state (like the diagnostic overlays): none of it
// enters terrain identity, persistence or generation.
struct StudioTerrainLayerOptions
{
    // Near-field production clipmap terrain.
    bool productionSurface{true};
    // Full clipmap renderer (the default): the production clipmap draws from the
    // ground out to orbit and the orbital globe patches are not used at any
    // altitude. Off brings back the hand-off to the orbital globe.
    bool fullClipmap{true};
    // Orbital displaced-globe patches.
    bool macroGlobe{true};
    // Ocean surface (sea-level water) on the terrain and on the orbital globe.
    bool ocean{true};
    // Volume surface-effect stamps applied to the terrain.
    bool surfaceEffects{true};
    // LOD bias in stops, clamped to [-4, 4]. +1 keeps richer representations
    // and asks the orbital patches for twice the resolution; -1 the opposite.
    // With the full clipmap renderer it scales the clipmap planner's target
    // spacing the same way (+1 = twice as many samples per pixel).
    f32 lodBiasStops{0.0F};
    // Dynamic clipmap planning: draw and generate only the clipmap levels the
    // camera can use (terrain_view::ClipmapPlanner). Off activates the whole
    // ladder at every altitude, as before.
    bool dynamicClipmaps{true};
    // Target clipmap sample spacing in screen pixels at the nearest ground.
    // Lower keeps finer levels active longer (clamped to [0.25, 32]).
    f32 clipmapPixelsPerVertex{3.0F};
    // How long a clipmap level takes to dissolve in or out when the plan adds or
    // drops it, in seconds ([0, 5]). 0 swaps levels instantly (they pop).
    f32 clipmapFadeSeconds{0.4F};
    // EXPERIMENT (off by default): distance-banded clipmap levels. With the full
    // clipmap renderer, level k is drawn only where the camera's distance to the
    // terrain lies in its band, bandEdgesMeters[k - 1]..bandEdgesMeters[k], and
    // neighbouring bands cross-fade per pixel across a zone around each edge, so
    // the rings resize continuously as the camera moves. Replaces the fixed 2:1
    // ladder and its planner; the last edge is the farthest distance drawn.
    // Needs at least two positive, increasing edges (0 ends the list).
    bool experimentalDistanceBands{false};
    // Multiplies every band edge ([0.1, 10], default 1): 2 pushes every level's
    // band twice as far from the camera (finer detail reaches farther, at the
    // cost of larger, coarser windows per level). Applied in 1/8-octave steps
    // because a change rebuilds the terrain renderer.
    f32 clipmapBandScale{1.0F};
    // Banded levels refresh only the strip that scrolled into view (default).
    // False regenerates a whole level on every scroll, for comparison.
    bool clipmapPartialUpdates{true};
    // Composite the derived physical pages (the "cache status" bounds) into the
    // clipmap's elevation and water depth. Off draws the plain generated
    // terrain, to tell page-related height steps from the generator.
    bool physicalPages{true};
    // Ray-march the body's cloud layer in this view (ground to orbit). The
    // cloud field itself is still built and used for shadows/orbital globes.
    bool clouds{true};
    std::array<f32, 16> clipmapBandEdgesMeters{
        100.0F, 500.0F, 2000.0F, 10000.0F, 40000.0F, 160000.0F, 640000.0F,
        2560000.0F, 10000000.0F, 40000000.0F};

    [[nodiscard]] constexpr bool operator==(
        const StudioTerrainLayerOptions&) const noexcept = default;
};

// The built cloud field of the view's target body (read-only diagnostics).
struct StudioCloudReport
{
    u32 layerCount{0U};
    f64 meanCoverage{0.0};
    f64 meanOpticalDepth{0.0};
    i64 timeBucket{0};
    u64 fingerprint{0U};
    bool gpuResident{false};
};

// One clipmap level that is drawn this frame (read-only diagnostics).
struct StudioClipmapLevelStats
{
    u32 level{0U};
    f64 spacingMeters{0.0};
    f64 halfExtentMeters{0.0};
    // Distance-band edges (experimental banded clipmap); 0 for the ladder.
    f64 bandInnerMeters{0.0};
    f64 bandOuterMeters{0.0};
};

// What the clipmap planner chose for a view this frame (read-only diagnostics).
struct StudioClipmapPlanStats
{
    bool valid{false};
    bool dynamic{false};
    // The experimental distance-banded clipmap is drawing.
    bool banded{false};
    // The drawn levels, finest first.
    std::vector<StudioClipmapLevelStats> levels;
    // The clipmap is frozen (the camera no longer drives it) / drawn as wireframe.
    bool frozen{false};
    bool wireframe{false};
    u32 ladderLevels{0U};
    u32 firstLevel{0U};
    u32 lastLevel{0U};
    f64 nearestGroundMeters{0.0};
    f64 visibleArcMeters{0.0};
    f64 requiredSpacingMeters{0.0};
    f64 finestSpacingMeters{0.0};
    f64 coarsestHalfExtentMeters{0.0};
    u64 planChanges{0U};
    // Terrain samples the clipmap generated since the renderer was created.
    u64 generatedSamples{0U};
    f64 groundElevationMeters{0.0};
    // Elevation of the terrain the clipmap draws under the camera (GPU readback,
    // a few frames late); navigation floors the camera on it.
    bool renderedGroundValid{false};
    f64 renderedGroundElevationMeters{0.0};
    // The CPU terrain source evaluated at the same vertices and footprint.
    f64 renderedGroundCpuElevationMeters{0.0};
    f64 renderedGroundFootprintMeters{0.0};
};
} // namespace orbit::studio_ui
