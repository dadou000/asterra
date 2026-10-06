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
sources = ["engine/terrain/include/orbit/terrain/TerrainContracts.hpp"]
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
"GPU compute passes: field generation, drainage, flow accumulation, hydraulic/thermal/aeolian/coastal on the GPU, readback" = "gpu-passes"
"frozen contracts: authority, revisions, page identity, analytic source, tectonics" = "contracts"
"sample streaming, toroidal residency, strip refresh" = "streaming"
"clipmap levels, rings, LOD, camera-relative terrain" = "clipmaps"
"terrain debugging overlays" = "clipmaps/debugging"
"geology, rock types, stratigraphy" = "geology"
"uplift, tectonics, mountain belts" = "macro-geology"
"base relief, ridges, valleys, derivatives" = "relief"
"craters, impacts, ejecta" = "impacts"
"material column, regolith, soil, exposed surface, caves/local 3D" = "material-column"
"drainage, flow routing, depression filling" = "hydrology"
"erosion (stream power, hydraulic, thermal, aeolian, glacial), sediment" = "erosion"
"river network, meanders" = "rivers"
"water service, coastal process, lakes, rivers as water" = "water"
"biomes, base biome, placement" = "biomes"
"vegetation/instance scatter" = "scatter"
"derived regions, page boundary exchange, halo" = "regions"
"CPU terrain page cache" = "page-cache"
"persistent GPU terrain cache, regenerating while stationary, resident bytes" = "gpu-cache"
"edit invalidation, what rebuilds after a terrain or biome edit" = "invalidation"
"terrain debug fields (the 21 views)" = "debug-fields"
"V0.0.4 milestone specs M00-M31" = "/history"
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

Each module card lists the V0.0.4 milestone specs it implements; the full M00-M31 index is `/history` (the milestone documents are
design and acceptance records, historical rather than normative).
