+++
path = "/rendering/terrain"
title = "Terrain"
kind = "subsystem"
status = "stable"
summary = """
Planet terrain from ground level to orbit. Authority chain: geology -> terrain processes -> exposed \
surface -> biome dressing. Generated terrain is derived state; persistent GPU pages cache already \
generated terrain and regenerate only on invalidation. The near-field and far views are drawn by a \
20-level GPU clipmap (see clipmaps)."""
keywords = ["terrain", "planet", "geology", "erosion", "hydrology", "biome", "gpu pages", "cache", "surface", "water", "streaming"]
sources = ["docs/V0.0.4_SPEC.md", "engine/terrain/include/orbit/terrain/TerrainContracts.hpp"]
invariants = [
  "Surface authority chain: geology -> terrain processes -> exposed surface -> biome dressing; no stage writes around the chain.",
  "Procedural fields are sampled in canonical body coordinates, never in per-face 2D noise domains; tangent derivatives use one consistent frame.",
  "Identical physical sample at a face/ring boundary returns equivalent geology/material/process inputs; clipmap phase does not affect physical terrain identity.",
  "Generated terrain is derived state, not project authority; authored constraints are authority (M04).",
  "GPU terrain pages are persistent working sets: regenerated only when missing or invalid, never recomputed every frame.",
  "No terrain system writes around the command/document path.",
]
related = ["/rendering/terrain/clipmaps"]

[routes]
"clipmap levels, rings, LOD, camera-relative terrain" = "clipmaps"
"terrain debugging overlays" = "clipmaps/debugging"
"geology, stratigraphy, materials" = "/legacy/v0-0-4-m02-geological-materials"
"erosion (hydraulic, thermal, aeolian, glacial, stream power)" = "/legacy/v0-0-4-m11-hydraulic-erosion"
"drainage, rivers, lakes, coast" = "/legacy/v0-0-4-m09-drainage"
"persistent GPU terrain cache, dependency invalidation" = "/legacy/v0-0-4-m26-persistent-gpu-terrain-cache"
"biomes, scatter, surface material" = "/legacy/v0-0-4-m20-biome-placement"
"water service, standing water" = "/legacy/v0-0-4-water-service"
"whole V0.0.4 terrain specification" = "/legacy/v0-0-4-spec"
+++

## Module map

| Module (`engine/`) | Role (see public headers) |
| --- | --- |
| `terrain` | contracts, analytic terrain source, global fields, tectonic descriptors |
| `terrain_geology`, `terrain_macro_geology`, `terrain_relief`, `terrain_impacts` | materials/stratigraphy, macro geology field, base relief, impact craters |
| `terrain_hydrology`, `terrain_erosion`, `terrain_water` | drainage pages and river graph, erosion operators, lake/river/coastal water and `WaterService` |
| `terrain_material_column`, `terrain_biome`, `terrain_scatter` | physical material column and surface resolver, biome service, deterministic scatter |
| `terrain_region`, `terrain_cache`, `terrain_dependency`, `terrain_stream` | derived regions and boundary exchange, page cache, dependency invalidation graph, toroidal residency/streaming |
| `terrain_gpu` | GPU generators and compute passes (field generation, drainage, erosion, persistent cache, composite) |
| `terrain_view` | clipmap layout, planner and tracker (what to draw, where) |
| `terrain_render` | `TerrainPreviewRenderer`, surface shaders and effects (how to draw it) |
| `terrain_debug` | debug fields/rasters/live pages |

Rule of thumb: **what** the terrain is lives in the generator/process modules; **which levels exist and
where they sit** lives in `terrain_view`; **how they are drawn** lives in `terrain_render`.

The V0.0.4 milestone documents (M00-M31) are the design and acceptance records of each stage; they are
historical specifications, reachable through the routes above.
