+++
path = "/rendering/terrain/clipmaps/level-planning"
title = "Level planning (which levels are drawn)"
kind = "concept"
status = "stable"
owner_module = "OrbitTerrainView"
summary = """
ClipmapPlanner chooses the active range [first,last] each frame: the finest level from the screen-space \
sample spacing at the nearest visible ground (pixelsPerVertex x pixel angle x distance), the coarsest from \
the farthest visible ground (horizon arc, at least 1.5x height), with 0.4-octave hysteresis so a camera \
on a boundary does not flicker."""
keywords = ["planner", "level selection", "lod", "pixels per vertex", "hysteresis", "finest", "coarsest", "visible ground", "plan"]
sources = [
  "engine/terrain_view/include/orbit/terrain_view/ClipmapPlanner.hpp",
  "engine/terrain_view/src/ClipmapPlanner.cpp",
  "engine/terrain_view/tests/ClipmapPlannerTests.cpp",
  "engine/terrain_render/src/TerrainPreviewRenderer.cpp",
]
symbols = ["ClipmapPlanner", "ClipmapPlannerConfig", "ClipmapPlan", "ClipmapPlanView", "SetClipmapPlanner"]
invariants = [
  "The plan is a contiguous inclusive range inside the ladder and never leaves it; at least minimumLevels (3) stay active.",
  "The ground directly below the camera always counts as visible, so turning the camera never needs levels that were just dropped.",
  "A fine level is added the moment demand reaches it but dropped only 0.4 octaves later; the coarse end grows at once and shrinks 0.4 octaves late (hysteresisOctaves).",
  "The planner adds reliefMarginMeters (300 m) above the height hint so mountains ahead are not assumed farther than they can be.",
  "Equality of plans compares only dynamic/first/last (ClipmapPlan::operator==); metrics are informational.",
  "Planner disabled (dynamic_clipmaps=false) means every ladder level is active.",
]
related = ["/rendering/terrain/clipmaps/level-cross-fade", "/rendering/terrain/clipmaps/debugging"]
depends_on = ["/rendering/terrain/clipmaps"]
verify = [
  "ctest -R Orbit.ClipmapPlanner.",
  "Straight down from 3 km the finest active level is 4 m; from 20 km it is 32 m (pixelsPerVertex 3).",
  "view.text_diagnostics clipmap_plan: first/last level, finest spacing, coarsest reach, nearest ground, required spacing.",
]
verified = "b0a0de7f"

[[diagnose]]
symptom = "wrong detail level: terrain too coarse near the camera or fine levels wasted far away"
steps = [
  "Read view.text_diagnostics -> clipmap_plan.required_spacing vs finest_spacing (what the screen asks for vs what is drawn).",
  "Lower clipmap_pixels_per_vertex (view.terrain_layers_set) to keep finer levels longer; raise it to drop them sooner (default 3).",
  "lod_bias_stops scales the target spacing: +1 stop doubles samples per pixel.",
  "Turn on clipmap_levels (tint by level) and clipmap_rings to see the active range.",
]
docs = ["/rendering/terrain/clipmaps/debugging"]
+++

## Inputs and outputs

`ClipmapPlanner::Plan(ladder, ClipmapPlanView)` takes the planet-centred camera position and axes, vertical FOV,
viewport size, planet radius and the analytic terrain elevation under the camera (supplied by `ComposeBase` as a
hint). It returns a `ClipmapPlan`: `firstLevel`, `lastLevel`, `nearestGroundMeters`, `visibleArcMeters`,
`requiredSpacingMeters`, `finestSpacingMeters`, `coarsestHalfExtentMeters`, `groundInView`,
`farthestDistanceMeters`.

## How the range is chosen

- **Finest level (screen error).** Cast a grid of rays (`raysAcross` x `raysDown`, 9 x 7) across the viewport
  against the ground sphere, plus the ground below. The sample spacing one pixel asks for at the nearest
  visible ground is `pixelsPerVertex x pixelAngle x distance`; levels finer than that are not drawn.
- **Coarsest level (visible ground).** The ground arc to the farthest visible point (the horizon arc if the
  horizon is in view, at least 1.5x the height above ground), times `coverageMargin` (1.2), sets how far the outer
  level must reach.
- **Hysteresis** as in the invariants.

## Controls (and where they are wired)

`view.terrain_layers_set`: `dynamic_clipmaps`, `clipmap_pixels_per_vertex` (default 3), `lod_bias_stops`
(clamped to [-4, 4]), `clipmap_fade_seconds`. Same controls in the viewport Diagnostics "Terrain layers"
section. Renderer entry: `TerrainPreviewRenderer::SetClipmapPlanner`.

## Gotcha

Changing the range is cheap because the renderer keeps all 20 sample buffers. The adaptive coverage tiers
(`StudioTerrainRuntimeConfig::adaptiveCoverage`) are disabled: they shifted the whole ladder and forced every
level to resample at fixed altitudes.
