+++
path = "/rendering/lighting-inventory/surface-and-lighting"
title = "Inventory: terrain, direct lighting, caches, reflections and exposure"
kind = "concept"
status = "draft"
summary = """
The systems that turn light into pixels: terrain clipmaps and their visibility data, the direct lighting pass and its \
sky-fill sources, the 3D radiance cache, the final gather and reflections, water, exposure, and the CPU orchestrator \
that wires them per view. For each: purpose, residency, size and update rate."""
keywords = ["clipmap", "terrain ambient", "sun shadow field", "direct lighting", "radiance cache", "final gather", "reflections", "exposure", "orchestrator"]
sources = [
  "engine/lighting/src/DirectLighting.cpp",
  "engine/lighting/src/RadianceClipmapResidency.cpp",
  "engine/terrain_gpu/src/GpuTerrainAmbient.cpp",
  "engine/studio_ui/src/StudioViewportLightingPass.cpp",
]
related = ["/rendering/lighting-inventory", "/rendering/lighting", "/rendering/terrain/clipmaps/ambient"]
depends_on = ["/rendering/lighting-inventory"]
verified = "d639bc09"
+++

## Terrain data and visibility

| System | Role | Residency | Size / rate |
|---|---|---|---|
| Clipmaps | 20 nested 2:1 levels (1 m to ~16,800 km half extent), 129^2 samples per level, 32 B per sample, toroidal | gpu | ~17k samples per level; on scroll or edit |
| GPU terrain generation | Field generator, region deltas, physical pages, hydrology and erosion chains; no CPU `Sample` for clipmaps | gpu | on scroll or edit |
| TerrainSource | CPU authority (analytic plus baked); Phacelle erosion lives here; the GPU generator mirrors it | cpu | per revision |
| Terrain ambient channel | Per-sample sky openness and lean (16 horizon azimuths, coarse-to-fine merge); level table per frame | gpu | 4 B per sample, 80 B per level; when a level's samples change |
| Sun-shadow field | Body-fixed 1024x512 heightfield from `TerrainSource.Sample` (background job); per-pixel march in shaders | cpu->gpu | 2 MB, 256 KiB per frame upload |
| Surface classifier / pixel shader | Biome and material to albedo, roughness, normal; four G-buffer targets | gpu | per frame |

## Lighting

| System | Role | Residency | Size / rate |
|---|---|---|---|
| Visibility registry | One occlusion API over screen-space, analytic, terrain, software proxy and hardware ray-query providers | cpu + gpu | per query |
| Proxy sun shadow | One hardware ray per pixel toward the star for authored proxies | gpu | per frame |
| DirectLightingRenderer | Stellar term x cloud shadow x proxy shadow x terrain visibility, plus sky fill; 40 push dwords, 9 buffers, 6 textures | gpu | per frame |
| Radiance cache (3D) | 6 levels x 32^3 cells from 2 m (x4 per level), 96 B cells: one-bounce L1 and a sky channel | cpu control and estimator, gpu cells | ~19 MB; ~64 cells per frame; 1.2 ms/frame budget |
| Final gather and cache sampler | One ray per 2x2 quad, bilateral reconstruction, cache fallback | gpu | per frame |
| Hybrid and exact reflections | Smooth reflections over one cached opaque scene; read the cache sky channel | gpu | per frame |
| Local lights, particle grid, emissive | Flux-ranked tile lists, city light tree, transient flashes | cpu->gpu | per frame |
| Water (near-field and orbital) | Bed, water vertex and pixel shaders; sky and sun as constants | gpu | per frame |
| Exposure and tone mapping | Histogram auto-exposure in cd/m^2, highlight protection, TAA | gpu | per frame |

## Control

| System | Role | Residency |
|---|---|---|
| Lighting scheduler | GPU budget and quality policy to a work plan | cpu |
| StudioViewportLightingPass | Builds the render graph per view; gathers the sun, sky summary, cache snapshots and terrain buffers; owns bypass flags (RPC and MCP) | cpu |
| FieldRegistry | Describes fields with CPU/GPU/hybrid residency and a resolution policy; the existing residency vocabulary, not yet used by these systems | cpu |

## Sky-fill sources in direct lighting (current order)

1. Clipmap terrain ambient (cosine-weighted, geometry only, scaled by the current sky).
2. 3D cache cell where the clipmap has nothing (uniform-hemisphere visibility, about half as bright on open terrain);
   cells with a zero baked sky count as missing.
3. A flat 70% open analytic ambient at the current sky.

All three use the CPU sky summary and none of them know about clouds.
