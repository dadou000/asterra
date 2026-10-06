+++
path = "/rendering/terrain/clipmaps/distance-bands"
title = "Distance-banded levels (experiment)"
kind = "concept"
status = "experimental"
owner_module = "OrbitTerrainView"
summary = """
EXPERIMENT, off by default. With experimental_distance_bands on, clipmap level k is drawn only where a pixel's \
distance from the camera lies in its band (edges default 100, 500, 2000, 10000, ... m); neighbours cross-fade per \
pixel across +-15% of the edge, so rings resize continuously with the camera and no holes are cut. The 2:1 ladder, \
planner and cross-fade remain the production path."""
keywords = ["distance bands", "banded", "experiment", "band edges", "band scale", "clipmap_band_edges_meters", "clipmap_band_scale"]
sources = [
  "engine/terrain_view/include/orbit/terrain_view/ClipmapLayout.hpp",
  "engine/terrain_view/src/ClipmapLayout.cpp",
  "engine/terrain_render/src/TerrainPreviewRenderer.cpp",
]
symbols = ["ClipmapBand", "ClipmapLevelBand", "kMaxClipmapBands", "bandZoneFraction", "bandExtentMargin"]
invariants = [
  "Banded mode ignores levelCount, baseSpacingMeters and levelScale; level k's band is [edge[k-1] (0 for level 0), edge[k]] and the last edge is the farthest distance drawn.",
  "A pixel is owned by exactly one level (same screen-door noise); neighbours overlap by bandZoneFraction (0.15) of the edge distance on each side.",
  "Each level is a 513x513 window sized to outer edge x bandExtentMargin (1.3); spacing = window / 512; nothing is morphed to a parent.",
  "Edges must increase; at most kMaxClipmapBands (16) bands. Editing edges or the scale rebuilds the terrain renderer (the scale is applied in 1/8-octave steps).",
  "LOD bias and pixels-per-vertex do not apply in banded mode; the 'Active clipmap rings' overlay is not drawn (use level tint + wireframe).",
]
related = ["/rendering/terrain/clipmaps/level-planning"]
depends_on = ["/rendering/terrain/clipmaps"]
verify = [
  "view.text_diagnostics: clipmap_plan.banded and levels[].band_inner_meters / band_outer_meters.",
  "Altitude sweep 800 m to 12,000 km: no hitches (measured 4.6 ms near ground, ~9 ms from orbit).",
]
verified = "b0a0de7f"
+++

## Cost and caveats

- Sample spacing at a band's inner edge is `(outer / inner) x 1.3 / 512` of the distance: about 14 screen pixels per
  vertex for a x5 band at 1392 px / 70 degrees (the 2:1 ladder is about 9). Add edges to tighten.
- Levels re-centre on their own lattice, so each regenerates in full whenever the camera crosses one of its cells
  (partial strip updates are on by default: `bandPartialUpdates`).
- Enable with `view.terrain_layers_set { experimental_distance_bands, clipmap_band_edges_meters, clipmap_band_scale }`
  (MCP `orbit_view_terrain_layers_set`); Diagnostics shows one "Level N reaches (m)" input per level.
