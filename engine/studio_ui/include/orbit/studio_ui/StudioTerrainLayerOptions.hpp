#pragma once

#include <orbit/core/Types.hpp>

#include <array>
#include <vector>

namespace orbit::studio_ui
{
// Cloud lab: replaces the weather with ONE isolated cloud of a chosen type, with
// exaggerated development, placed ahead of the camera, so vertical development,
// shape and self-shadowing can be judged without hunting for them in the weather.
// Transient presentation state like the other layer options.
struct StudioCloudLab
{
    bool enabled{false};
    // Cloud type axis: 0.05 stratus, 0.2 stratocumulus, 0.32 nimbostratus,
    // 0.5 cumulus, 0.72 congestus, 1.0 cumulonimbus.
    f32 type{1.0F};
    f32 coverage{0.85F};
    // High cloud (anvil / cirrus) coverage over and around the cloud, [0, 1].
    f32 cirrus{0.0F};
    f32 precipitation{0.3F};
    // Horizontal radius of the cloud in metres.
    f32 radiusMeters{6000.0F};
    // Multiplies the vertical extent of the cloud shell (the cloud grows taller).
    f32 heightScale{1.0F};
    // Distance ahead of the camera, along the ground, where it is placed.
    f32 distanceMeters{20000.0F};
    // Light the cloud from a chosen sun (elevation above the horizon at the cloud,
    // azimuth clockwise from north) instead of the scene sun, to read self-shadowing.
    bool overrideSun{false};
    f32 sunElevationDegrees{35.0F};
    f32 sunAzimuthDegrees{90.0F};
    // Life cycle (0 towering cumulus .. 0.3 growing .. 0.6 mature with anvil ..
    // 0.9 dissipating), organisation (0 single cell, 0.5 multicell, 1 organised),
    // density multiplier and the seed that arranges multicell clusters.
    f32 maturity{0.6F};
    f32 organisation{0.2F};
    f32 density{1.0F};
    // Coverage of a thin cirrus sheet on the anti-sun side of the cell (0 = none), to see the
    // storm shadow it.
    f32 cirrusSheet{0.0F};
    u32 seed{1U};
    // Incremented to (re)place the cloud ahead of the camera.
    u32 placeSerial{0U};

    [[nodiscard]] constexpr bool operator==(const StudioCloudLab&) const noexcept = default;
};

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
    // Resolution of the cloud ray-march relative to the viewport, [0.25, 1]. The
    // default 0.5 marches at half resolution (cheap, soft); 1 marches every pixel
    // (crisp edges and fine detail, about four times the cost).
    f32 cloudResolutionScale{0.5F};
    // Accumulate the cloud march over frames (reprojected, history-clamped). Averages
    // the per-frame sampling jitter away, so the half-resolution march reads as a much
    // finer image. Off shows the raw single-frame march.
    bool cloudTemporal{true};
    // Strength of the crepuscular rays in cloud-shadowed air, [0, 2]. 0 turns them off
    // (and skips their march), 1 is physically consistent with the atmosphere pass.
    f32 cloudGodrayStrength{1.0F};
    // Cache the optical depth towards the sun around the camera (a light volume, like EVE's) so
    // cloud lighting reaches past the short in-march sun steps (cloud-on-cloud shadows at a low
    // sun) and god rays are one lookup per step. Off falls back to marching everything.
    bool cloudLightVolume{true};
    // Per-pass bypass switches for bisecting a rendering artefact (all default off = normal rendering).
    // Each skips one stage of the frame for this view: the cloud shadow texture read by direct lighting,
    // the indirect lighting (final gather + hybrid reflections), the near-field water pass, and the
    // atmosphere (which also skips the clouds drawn after it).
    bool bypassCloudShadow{false};
    // Skips the authored Visibility Proxy sun shadow (hardware ray query) that direct lighting reads.
    bool bypassProxySunShadow{false};
    // Skips drawing authored Visibility Proxies as lit geometry (they stay invisible occluders).
    bool bypassProxySurfaces{false};
    // Skips drawing imported Static Meshes (glTF/GLB) into the surface buffer.
    bool bypassMeshSurfaces{false};
    // Skips the radiance cache's sky-only fill (sky irradiance occluded by terrain and proxies, added by direct lighting).
    bool bypassSkyCache{false};
    bool bypassIndirectLighting{false};
    bool bypassNearFieldWater{false};
    bool bypassAtmosphere{false};
    // Skips only the hybrid reflections stage (the final gather still runs); bypassIndirectLighting skips both.
    bool bypassHybridReflections{false};
    // Skips only the radiance-cache fallback that fills pixels the screen-space gather did not resolve.
    bool bypassRadianceCache{false};
    // Replaces the final gather's contribution with its coverage: red = confidence, green = gathered
    // brightness (log), magenta = the gather returned nothing for that pixel (it gets no indirect light).
    bool indirectCoverageView{false};
    // Shows only the global illumination: the final gather and radiance-cache cascade light, with no
    // direct sun, sky fill, emission or reflections. Needs indirect lighting on.
    bool giOnlyView{false};
    // Anti-aliasing of the HDR scene colour before tone mapping:
    // 0 = off, 1 = FXAA, 2 = TAA (jittered camera, history reprojection; falls
    // back to FXAA on frames without usable history). See post_process::AntiAliasingMode.
    u8 antiAliasing{2U};
    // Mesh distance-field debug view (sphere traces the merged field and shades it):
    // 0 = off, 1 = shaded, 2 = step count heat map, 3 = distance, 4 = split (left SDF, right scene).
    u8 sdfDebugView{0U};
    // Skips the final gather's world-space fallback (rays the screen cannot resolve are traced
    // through the mesh distance field), to compare with screen-space-only GI.
    bool bypassSdfGi{false};
    // Leave the terrain patch / Visibility Proxies out of the mesh distance
    // field (A/B checks of what each contributes to the GI fallback).
    bool bypassSdfTerrain{false};
    bool bypassSdfProxies{false};
    // Strength of the final gather's indirect light. The gather averages the radiance of what its
    // rays hit and multiplies by albedo / pi, so a physically correct diffuse bounce
    // (albedo x average radiance) needs pi here; lower values dim all gathered bounce light.
    f32 giIntensity{1.0F};
    // Scales the TAA sub-pixel jitter (1 = full +-0.5 px, 0 = no jitter, which turns TAA into a
    // plain temporal filter). Lower values trade anti-aliasing for less visible shimmer.
    f32 taaJitterScale{1.0F};
    // Multiplies the sun's angular size in the mesh sun shadow (PCSS): 1 = the real sun, 0 = hard
    // shadows, larger values exaggerate the penumbra.
    f32 meshShadowSoftness{1.0F};
    // > 0 draws a horizontal slice of the light volume at this altitude (metres) over the view as a
    // heatmap of the optical depth towards the sun (magenta = voxel not ready). 0 = off.
    f32 cloudVolumeDebugAltitude{0.0F};
    StudioCloudLab cloudLab{};
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
    // Layout grid resolution and the vertices the renderer submits for the level (see
    // TerrainClipmapLevelSummary); a level is fully drawn when drawnVertices == 6 * (resolution - 1)^2.
    u32 gridResolution{0U};
    u64 drawnVertices{0U};
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
