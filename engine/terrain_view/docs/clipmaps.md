+++
path = "/rendering/terrain/clipmaps"
title = "Clipmaps (ground to orbit)"
kind = "subsystem"
status = "stable"
owner_module = "OrbitTerrainView"
summary = """
The production terrain representation: a fixed ladder of 20 nested 2:1 clipmap levels (spacing 2^j m, \
1 m up to a half extent of ~16,800 km) on an azimuthal-equidistant lattice centred under the camera. \
Each frame the planner picks a contiguous active range [first,last]; the tracker moves each level on a \
shared lattice; the renderer generates and draws only active levels and cross-fades range changes."""
keywords = ["clipmap", "lod", "level", "ring", "ladder", "terrain view", "full clipmap", "orbital globe", "20 levels"]
sources = [
  "engine/terrain_view/include/orbit/terrain_view/ClipmapLayout.hpp",
  "engine/terrain_view/include/orbit/terrain_view/ClipmapPlanner.hpp",
  "engine/terrain_view/include/orbit/terrain_view/ClipmapTracker.hpp",
  "engine/terrain_render/src/TerrainPreviewRenderer.cpp",
  "docs/ORBIT_PERFORMANCE.md",
]
symbols = ["ClipmapConfig", "ClipmapLayout", "ClipmapPlanner", "ClipmapTracker", "TerrainPreviewRenderer", "ApplyClipmapActiveRange"]
invariants = [
  "Level j has exactly one sample spacing (2^j * base); a level that stays active keeps its resident samples whatever the camera does. Level samples never depend on the active set.",
  "Production path is the fixed 2:1 ladder + planner + cross-fade; distance bands are an EXPERIMENT and off by default.",
  "full_clipmap (default on) draws the clipmap from ground to orbit and suppresses every orbital-globe draw site; both drawing at once z-fights.",
  "Fine levels (spacing < 256 m) use a 257x257 grid and coarse levels 513x513 (ClipmapConfig::coarseGridResolution / coarseMinSpacingMeters); grids must be 4k+1.",
  "The finest active level has no inner hole; every coarser active level has a hole covered by the next finer one.",
  "Clipmap phase never changes physical terrain identity: the same world position evaluates to the same terrain in every level.",
  "Inactive levels are skipped entirely (no draw, no generation, no pending dirty work); a level that becomes active is regenerated in full once, in the frame it is first drawn.",
]
related = ["/rendering/terrain"]
depends_on = ["/rendering/terrain"]
used_by = ["/editor/viewport"]
verify = [
  "ctest -R Orbit.ClipmapPlanner (engine/terrain_view/tests/ClipmapPlannerTests.cpp).",
  "orbit_view_text_diagnostics: clipmap_plan.levels lists the active levels; fully_drawn must be true for each.",
  "Dynamic and fixed (dynamic_clipmaps=false) renders at the same pose are pixel-identical.",
]
verified = "00d8c5b6"

[routes]
"which levels are drawn, wrong detail level, too many/few levels" = "level-planning"
"levels pop or flicker when the plan changes" = "level-cross-fade"
"terrain shifts, rings do not line up, jitter far from origin" = "lattice-and-tracking"
"spiky concentric rings, height step along page edges" = "precision-and-pages"
"GPU terrain differs from CPU terrain height" = "generator-parity"
"camera ends up under the drawn ground" = "camera-floor"
"multi-second freeze when altitude crosses a threshold" = "rebuild-hitches"
"experimental distance-banded levels" = "distance-bands"
"something looks wrong and I need to see the levels" = "debugging"
+++

## Pipeline (per frame)

```text
camera pose
  -> ClipmapPlanner::Plan        which contiguous level range [first,last] the screen needs
  -> ClipmapTracker::Update      per-level snapped centre on the shared lattice (toroidal strip updates)
  -> ToroidalResidency::Apply    which sample regions to refresh (terrain_stream)
  -> GPU field generation        samples for new/refreshed regions (terrain_gpu)
  -> TerrainPreviewRenderer::Draw   draw active + fading levels with the clipmap vertex/pixel shaders
```

## Numbers worth knowing

- 20 levels; spacing `2^j` m; the ladder reaches ~16,800 km half extent, so it covers the visible hemisphere
  from orbit (the lattice is azimuthal-equidistant, valid to the antipode).
- From orbit about 5 levels are drawn (for example levels 11-15 at 1,000 km); straight down from 800 m, 6 levels
  reaching 1 km; a horizon view from 1 km, 13 levels reaching 131 km.
- Terrain GPU cost measured at 0.1-0.2 ms (RTX 4090, 4K-class viewport), 5-8 ms frames from the ground to 12,000 km.

## Known differences

From orbit, small crater speckle and the sharpest coast detail are softer than the old globe patches because rings
are coarse away from the nadir. A denser coarse grid (1025) or per-ring tuning would narrow this (not done).

The full measured history, with all caveats, is in `docs/ORBIT_PERFORMANCE.md`
(`/legacy/orbit-performance/dynamic-clipmap-levels`, `/legacy/orbit-performance/full-clipmap-renderer-ground-to-orbit`).
