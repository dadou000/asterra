+++
path = "/rendering/terrain/clipmaps/ambient"
title = "Terrain sky visibility on the clipmap"
kind = "concept"
status = "experimental"
owner_module = "OrbitTerrainRender"
summary = """
Every terrain clipmap level carries a second per-sample channel next to its elevation: how open the sky is above the \
sample and which way the open part leans. It lives in one buffer with the level's own toroidal addressing, is recomputed \
on the GPU whenever the level's samples change (coarse to fine, so a fine level takes far horizons from the coarser \
one), and is read by direct lighting at the resolution of the level that draws the point. Because it stores geometry \
only, lighting multiplies it by the current sky and it never goes stale when the sun moves."""
keywords = ["ambient", "sky visibility", "horizon", "bent normal", "toroidal", "clipmap", "sky fill", "bypass_terrain_ambient", "TerrainAmbientLayout", "GpuTerrainAmbient"]
sources = [
  "engine/terrain/include/orbit/terrain/TerrainAmbientLayout.hpp",
  "engine/terrain_gpu/include/orbit/terrain_gpu/GpuTerrainAmbient.hpp",
  "engine/terrain_gpu/src/GpuTerrainAmbient.cpp",
  "engine/terrain_render/src/TerrainPreviewRenderer.cpp",
  "engine/lighting/src/DirectLighting.cpp",
  "tests/TerrainAmbientGpuTests.cpp",
]
symbols = ["kTerrainAmbientCommonShader", "kTerrainAmbientLookupShader", "GpuTerrainAmbient", "UpdateTerrainAmbient", "AmbientVisibility", "TerrainAmbientLookup"]
invariants = [
  "One 4-byte texel per clipmap sample: bits 0..7 visibility, 8..15 lean east, 16..23 lean north (offset-128 snorm, units of 1/3), 24..31 valid. It stores geometry only, never time of day or sky colour.",
  "All levels share one ambient buffer; level k owns resolution^2 texels from ambientFirstSample_[k] and is addressed exactly like g_samples: physical = (logical + origin) % resolution. Never index it with the raw logical coordinate.",
  "The per-level table (80 bytes: up+radius, east+spacing, north+halfCells, centre offset+origin, resolution+first sample+flags) is written every frame into a ring of host-visible buffers indexed frameIndex % framesInFlight; lighting must read the slot the terrain pass wrote.",
  "A level is recomputed in full whenever PrepareLevelFrame touched its samples (any dirty region), once per frame, coarse to fine, with a UAV barrier between levels. The finer level takes the horizon beyond its own grid from the coarser level's finished texels (the more occluded of the two wins).",
  "The basis and offsets are body-fixed, the frame lighting works in. The lighting lookup inverts TaSurfaceDirection (azimuthal equidistant about the level's up) and ignores the outer 2 samples of a level.",
  "Direct lighting prefers the 3D radiance cache's three finest levels (they also see authored proxies), then this channel, then a flat 70% open analytic ambient. The channel is off exactly when the sky cache fill is (orbit, bypass_sky_cache, bypass_indirect_lighting) or bypass_terrain_ambient is set.",
  "Reach is bounded by the level grid (13 log-spaced steps, up to 96 cells) plus the coarser level; bed elevation only (standing water is not opaque relief).",
]
related = ["/rendering/lighting/sky-cache-fill", "/rendering/terrain/clipmaps/shaders"]
depends_on = ["/rendering/terrain/clipmaps", "/rendering/lighting"]
verify = [
  "ctest -R Orbit.TerrainAmbientGpu (real Vulkan: open plateau, wall foot, far coarse wall merge, toroidal origin, lighting lookup agreeing with the compute output).",
  "orbit_view_terrain_layers_set(bypass_terrain_ambient=true) falls back to the flat ambient; to see what the lookup returns, temporarily paint terrainSky in the sky block of DirectLighting.cpp (red = level covers the point but no valid texel, green = hit).",
]
verified = "b9301364"

[[diagnose]]
symptom = "shadowed terrain has flat ambient or the channel seems to do nothing"
steps = [
  "orbit_view_terrain_layers_get: bypass_terrain_ambient, bypass_sky_cache and bypass_indirect_lighting must be false and the view near-field (not orbit); the sky summary is zero at night.",
  "At noon the whole sky fill is only a few percent of the frame; compare bypass_sky_cache=true to see its size before judging the openness variation.",
  "If the level table were never written the flags word of every level is 0 and the lookup finds nothing, so shading silently uses the flat 70% ambient: check UpdateTerrainAmbient ran (levels active, levelValid_, ambientValid_).",
]
docs = ["/rendering/lighting/sky-cache-fill"]
+++

## How it is produced and consumed

`TerrainPreviewRenderer::Draw` runs `PrepareLevelFrame` per active level; when a level's buffer was touched it is marked
ambient-dirty. After the draw loop `UpdateTerrainAmbient` clears the buffer once, recomputes every dirty level from the
coarsest down with `GpuTerrainAmbient::Dispatch` (sixteen azimuths marched over the level's own elevation grid, the
horizon slope corrected for planet curvature, visibility `1 / (1 + slope^2)` averaged over azimuths, lean pointing away
from blockers), then writes the level table. `AmbientVisibility(frameIndex)` hands the buffers to the lighting pass,
which binds them to `DirectLightingRenderer` (buffers 7 and 8) and looks the point up with `TerrainAmbientLookup`.

Resolution follows the clipmap: roughly distance / 64 per sample, so about 16 m at 1 km and about 150 m at 10 km, out to
the whole ladder, instead of the 3D cache's cubes that stop at about 65 km.
