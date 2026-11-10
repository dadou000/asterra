+++
path = "/rendering/terrain/water-volume-shader"
title = "Near-field water shaders"
kind = "concept"
status = "stable"
owner_module = "OrbitTerrainRender"
summary = """
WaterVolumeShader.cpp builds the three shader variants of the near-field standing-water split from the shared clipmap \
sources by exact-text patching: a bed pixel shader (terrain without water, silt under the sea), a water vertex shader (flat \
surface, wet-triangle test) and a water pixel shader (depth-tested shoreline, refracted water column, Fresnel and glint). \
The fixed-LOD UniformPlanetRenderer and its UniformPlanetMesh, which only the removed sandbox app used, were deleted in 0.0.9."""
keywords = ["water pass", "near-field water", "shoreline", "bed pixel shader", "silt", "BuildClipmapWaterVertexShader", "BuildClipmapWaterPixelShader", "BuildClipmapBedPixelShader", "water optics", "terrain depth", "alpha blend"]
sources = [
  "engine/terrain_render/src/WaterVolumeShader.cpp",
  "engine/terrain_render/include/orbit/terrain_render/WaterVolumeShader.hpp",
  "engine/terrain_render/src/TerrainPreviewRenderer.cpp",
  "engine/studio_ui/src/StudioViewportRenderer.cpp",
]
symbols = [
  "BuildClipmapBedPixelShader",
  "BuildClipmapWaterVertexShader",
  "BuildClipmapWaterPixelShader",
  "kWaterPixelBody",
  "kWaterVertexElevation",
  "DrawWater",
  "BindWaterOptics",
  "CreateWaterPipeline",
]
invariants = [
  "The clipmap terrain pass draws the true bed and never water; standing water is the separate NearFieldWater pass. Its page water-depth sample may contain sea, lake or river depth; the bed variant replaces the standing-water block with waterCoverage = 0 and swaps the ocean biome colour for silt (0.30, 0.27, 0.20), because that is what shows through the water.",
  "The water vertex shader is the terrain vertex shader with one block replaced, so morph, toroidal residency and the finer-level hole are identical. Do not fork it: edit ClipmapVertexShader.hpp and keep the markers (the vertex elevation block, `output.waterDepth = 0.0;`, the VSOutput closing brace, the final `return output;`) intact, or construction throws 'Orbit water pass could not locate ...'.",
  "Water vertices sit at sea level (push-constant dword 43, g_centerOffsetMeters.w) or at bed + depth where the sample has standing water. A triangle is drawn only if at least one of its three corners has water depth > 0; the corner sample coordinates in kWaterVertexElevation must match the vertex shader's corner order.",
  "The shoreline is not geometry: the plane over dry corners lies under the terrain and the pixel stage discards fragments whose terrain view depth is not farther than the water's (behind <= 0). The pass therefore needs the terrain depth texture at graphics texture slot 0 (shader binding 2) from the same frame.",
  "The water pixel input struct is cut out of the water vertex shader text (BuildClipmapWaterPixelShader), so its interpolants always match; the optics buffer WaterOptics (six float4) is shader binding(1, 0), written by BindWaterOptics as 24 floats in the order absorption+index, deep colour+depth, roughness/opacity/planet radius, near/far, sun direction+irradiance, sky irradiance.",
  "Water is alpha-blended (alpha = max(1 - (1 - fresnel) * transmittance, 0.02), times optics opacity), writes no depth and one colour attachment. DrawWater returns without drawing when optics opacity <= 0 or the view is wireframe.",
  "In the water pass selfFade and finerFade are both 1 (ladder mode), so the dither discard never fires and the finer level's hole is a hard cut; the water is flat so overlapping levels coincide. Coverage dithering applies to terrain only unless distance bands are on.",
  "Ripples are world-fixed (no time input): two octaves, wavelengths 14 m and 5 m, each faded out as the wavelength approaches the pixel footprint. The optical path is deliberately 0.6x the geometric one and transmittance uses only green and blue absorption.",
]
related = [
  "/legacy/standing-water-rendering",
  "/rendering/terrain/water",
  "/rendering/terrain/clipmaps/shaders",
  "/rendering/terrain/clipmaps/level-cross-fade",
  "/rendering/terrain/surface-material",
]
depends_on = ["/rendering/terrain/clipmaps", "/rendering/terrain/streaming", "/rendering/shader-compiler"]
verify = [
  "ctest -R Orbit.TerrainSurfaceShader (all four clipmap variants compile; the bed variant no longer contains the depth-based water coverage and the water pixel shader reads g_terrainDepth).",
  "Toggle bypass_near_field_water (orbit_view_terrain_layers_set): the water plane disappears and the bed shows silt-coloured ground under the sea, with no other pass changing.",
]
verified = "55d48117"

[routes]
"what the standing-water pass looked like when it was introduced" = "/legacy/standing-water-rendering"
"how the clipmap vertex shader morphs and culls" = "/rendering/terrain/clipmaps/shaders"

[[diagnose]]
symptom = "no water near the camera, or a flat water plane showing through land"
steps = [
  "Toggle bypass_near_field_water in view.terrain_layers_set to see whether the pass is responsible; a wireframe view and optics opacity <= 0 also skip the pass (DrawWater early return).",
  "A plane over land means the depth test against the terrain failed: check the terrain depth texture is the one from this frame's terrain pass and that near/far in the optics buffer (floats 12-13) equal the projection actually used (camera override or config_ near/far).",
  "Missing water over a lake: the triangle's three corner depths were all <= 0 (deepestCorner), so the triangle is culled by design; check the standing-water depth stored in the sample (byte offset 20 of TerrainSampleValue; the lake source is /rendering/terrain/water).",
]
docs = ["/rendering/terrain/clipmaps/debugging", "/rendering/terrain/water"]

[[diagnose]]
symptom = "the sea looks opaque, too clear, or the glint is blown out"
steps = [
  "Read the optics: absorptionPerMeter, deepColor and deepColorDepthMeters, roughness and opacity come from the resolved ocean (TerrainWaterOptics, bound by BindWaterOptics).",
  "Transmittance uses (0, 0.5, 0.5) . absorption over a path of 0.6 * vertical depth / cos(refracted angle) with the cosine floored at 0.08; roughness is clamped to [0.02, 0.6]; the output colour is clamped to 60000 so RGBA16F cannot overflow.",
]
docs = ["/legacy/standing-water-rendering"]

+++

## The three builders (WaterVolumeShader.cpp)

All three work on strings; each locates its markers with `Require`/`ReplaceOnce` and throws `std::runtime_error` when a
marker is missing, so a refactor of the base shader fails at renderer construction instead of drawing something subtly wrong.

| Builder | Input | What it changes |
| --- | --- | --- |
| `BuildClipmapBedPixelShader` | `kTerrainSurfacePixelShader` | injects `lodFade` (TEXCOORD13) into the pixel input and the dither discard (`ditherNoise >= lodFade.x \|\| ditherNoise < lodFade.y`); silt instead of ocean colour; cuts out the standing-water block between the "Depth gives a continuous shallow shoreline" comment and `SurfaceOutputs output;` and substitutes `waterCoverage = 0` |
| `BuildClipmapWaterVertexShader` | `kClipmapVertexShader` | elevation = water surface; evaluates the three corners of the current triangle for water depth; adds `waterDepth` and `bodyFixedRay` (TEXCOORD12) outputs; sets `horizonClip = -1` when no corner is wet |
| `BuildClipmapWaterPixelShader` | the water vertex shader | takes its `VSOutput` struct text and appends `kWaterPixelBody` |

## Draw path

`CreateWaterPipeline` compiles the water vertex and pixel shaders: 56 push-constant dwords (same layout as the terrain
pass, see `/rendering/terrain/clipmaps/shaders`), two shader-resource buffers, one sampled texture, alpha blend, depth test
`GreaterEqual` without depth write, one colour attachment. `DrawWater` skips inactive levels, passes `drySurface`,
`seaLevelMeters` and fades of 1, binds the optics buffer at buffer slot 1 and the terrain depth at texture slot 0, and draws
`patchVertexCounts_[level]` vertices per level. The render graph runs it as the `NearFieldWater` pass after lighting and
before the atmosphere, only for the production-terrain presentation with a resolved ocean and `bypass_near_field_water` off
(`StudioViewportRenderer.cpp`).

For the physical story (why water became a separate object, limits, validation) read
`/legacy/standing-water-rendering`.

## Water pixel stage

Per fragment: dither test, terrain depth fetch (`g_terrainDepth.Load` at the pixel) converted to view depth using the
optics near/far, `behind = terrainViewDepth - surfaceViewDepth` (discard if `<= 0`); ripple normal; water column
(`rayMeters` from the depth difference, vertical depth, refracted path); `fresnel` from the refractive index; sky
reflection, in-scattered body colour weighted by depth against `deepColorDepthMeters`, and a GGX sun glint.

Hot iteration: all of this is native code and embedded strings, so a save takes the Studio generation handoff
(`/rendering/terrain/clipmaps/shaders`, last section).
