# GPU-first terrain synthesis

## Contract

The CPU is responsible only for generating/loading the persistent coarse planet maps. Once the world has been adopted, terrain below the coarse-map scale is a GPU problem.

No camera-centred CPU terrain generation, CPU detail height synthesis, or CPU material placement belongs in the final runtime path.

The current `GPUPlanetContext` conversion is a transition step: it uploads baked `PlanetFields` into immutable GPU textures once per adopted world. The long-term bake format should persist these context textures directly so runtime work is reduced to loading/uploading them.

## Pipeline

1. **Offline / load-time planet context**
   - macro elevation
   - tectonic uplift / plate context
   - bedrock family, erodibility, faults, strata, basins
   - coarse erosion sediment
   - drainage / discharge / floodplain / wetland
   - climate
   - soil depth and composition
   - biome / vegetation potential

2. **GPU geomorph synthesis**
   - reconstruct coarse elevation
   - derive continuous landform weights
   - domain-warped multi-scale noise
   - cellular/Voronoi structural ridges
   - mountain/crag synthesis
   - analytic drainage incision
   - sedimentary/depositional approximations
   - dune/glacial/depositional processes
   - output final displaced terrain height

3. **GPU surface classification**
   - derive slope from final displaced geometry
   - derive curvature/exposure/relative height where available
   - combine final terrain state with coarse geology, climate, soil and biome context
   - select/blend physically plausible materials
   - enforce material stability (angle of repose / exposed bedrock)

4. **GPU material detail**
   - PBR texture/material families
   - material-specific micro-normal/parallax/displacement
   - near-field microgeometry / higher-density geometry clipmap

5. **GPU scatter**
   - vegetation, trees, rocks, talus, debris, river stones, etc.
   - consume the exact same context and final material classification

## Current context textures

All continuous textures are linearly filtered and have direction-space cube-face gutters. Categorical maps are separate nearest-filtered textures so interpolation cannot invent nonexistent IDs.

- **Soil RGBA**: sand / silt / clay / organic
- **Surface RGBA**: soil depth / soil moisture / vegetation potential / sediment thickness
- **Geology RGBA**: erodibility / strata dip / basin / plate-boundary proximity
- **Structure RGBA**: signed uplift / fault intensity / floodplain / wetland
- **Climate RGBA**: mean temperature / precipitation / humidity / seasonal temperature range
- **Hydrology RGBA**: tangent-frame flow direction RG / logarithmic discharge / depositional influence
- **Rock R**: categorical bedrock family ID
- **Biome R**: categorical biome ID

## Milestone status

### M1 — coarse context textures — IMPLEMENTED

`GPUPlanetContext` builds immutable six-face context textures once per adopted world. The old camera-centred `MaterialClipmap` is no longer an autoload in this branch.

Remaining cleanup: persist the packed context with the world bake so runtime does not repack `PlanetFields`.

### M2 — analytic geomorph shader library — IMPLEMENTED, FIRST PASS

`gpu_geomorph.gdshaderinc` currently provides:

- deterministic value noise and five-octave fBm
- ridged multifractal structure
- domain warping
- reduced-cost cellular ridge skeletons
- context-driven mountain / arid / glacial / depositional weights
- multi-scale 16 km / 6 km / 1.4 km / 420 m / 120 m / 24 m bands
- drainage-oriented analytic incision using the baked downstream vector
- broad depositional shoulders/fans
- arid dune contribution
- glacial smoothing/flow
- LOD band limiting from actual clipmap vertex spacing

This is analytic synthesis, not iterative hydraulic simulation yet.

### M3 — active clipmap integration — IMPLEMENTED

`spherical_geometry_clipmap_procedural_uv.gdshader` now gets sub-grid displacement from `gm_geomorph_height()` rather than the old altitude-only five-band heuristic. Existing concentric LOD morphing, atmosphere and displacement-aware fragment normals are retained.

### M4 — material classifier — IMPLEMENTED, FIRST PASS

Materials are classified after displacement from final geometry slope plus context. Current classes:

- actual-family bedrock
- mineral soil
- vegetation cover
- aeolian sand
- mud
- snow
- scree / talus
- river gravel

Loose-material stability is slope-dependent. Thin soil and slopes above the plausible loose-material angle of repose reveal the actual underlying rock family.

### M4.1 — diagnostics — IMPLEMENTED

The Terrain debug tab exposes unlit views for:

- landform weights
- primary material weights
- secondary material weights
- soil context
- geology / biome IDs
- hydrology

The same tab can disable scanned PBR detail independently, which makes it possible to compare geomorph/classification cost and appearance against texture-detail cost.

### M5 — PBR material families — IMPLEMENTED, FIRST PASS

`gpu_surface_pbr.gdshaderinc` adds precision-safe scanned surface detail while preserving the classifier as the source of material identity and large-scale colour.

Current scanned families use the existing CC0 assets:

- `ground003` — generic mineral/loose ground base
- `leafy_grass` — vegetated open ground
- `brown_mud` — wet cohesive ground
- `forrest_ground_01` — forest floor

Current implementation details:

- triplanar projection, so the spherical terrain needs no authored UV unwrap
- 4096 m wrapped floating-origin detail coordinate, calculated before conversion to `Vector3`
- 1 m or 2 m physical tile periods that divide the 4096 m wrap exactly, preventing phase jumps at rebases
- classifier-colour-preserving scan modulation rather than letting one scan repaint whole biomes
- distance-gated scanned albedo/roughness detail
- tighter near-field gating for normal-map cost
- one generic loose-ground scan plus at most one specialised grass/mud/forest scan
- live debug toggle for direct performance/quality comparison

Still required for the mature M5 implementation:

- stochastic anti-tiling / macro variation for repeated scans
- dedicated PBR families for the 13 actual rock types
- dedicated sand, scree, gravel and snow scans/procedural PBR
- quality-tier texture budgets for lower GPUs

### M6 — material microrelief — NEXT

Use classified materials to control geometry and shading spectra:

- rock fractures / bedding / joints
- scree and gravel relief
- dune ripples
- soil aggregates
- snow buildup
- vegetation-ground roughness

The current L0 spacing is 0.75 m, so centimetre/decimetre silhouette relief requires a future denser near-field geometry layer rather than simply adding higher-frequency displacement to the current vertices. Until that layer exists, sub-metre relief belongs in normal/parallax shading rather than vertex displacement.

### M7 — iterative near-field GPU erosion

Add GPU compute-generated local height/sediment fields for the nearest terrain if analytic erosion is insufficient. The compute input remains immutable coarse context + deterministic procedural base; the CPU does not synthesize terrain.

### M8 — scatter

Generate scatter suitability from the same final material and geomorph fields, then feed GPU-driven/indirect vegetation, trees, rocks, talus blocks, debris and river stones.

## Runtime invariant

For a fixed world seed and coarse map set, all untouched sub-grid terrain must be reproducible from planet position and GPU algorithms alone.
