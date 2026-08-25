# GPU-first terrain synthesis

## Contract

The CPU is responsible only for generating/loading the persistent coarse planet maps. Once the world has been adopted, terrain below the coarse-map scale is a GPU problem.

No camera-centred CPU terrain generation, CPU detail height synthesis, CPU material placement, or CPU scatter classification belongs in the final runtime path.

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
   - material-specific micro-normal detail
   - dense near-field geometric microrelief

5. **GPU scatter**
   - vegetation, rocks, talus, river stones, debris and later trees/shrubs
   - deterministic candidate lattices from world seed + absolute snapped tangent cells
   - candidate acceptance and variation entirely on GPU
   - consume the same context / geomorph / material logic as terrain

6. **Optional local GPU compute refinement**
   - iterative hydraulic/thermal erosion only where analytic synthesis is insufficient
   - immutable coarse context + deterministic GPU base remain the only inputs

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

The same tab can disable scanned PBR and geometric microrelief independently, making it possible to isolate terrain synthesis, topology and surface shading costs.

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
- 1 m or 2 m physical scan periods that divide the 4096 m wrap exactly
- explicit `textureGrad()` sampling through material-dependent branches
- classifier-colour-preserving scan modulation rather than letting one scan repaint whole biomes
- distance-gated scanned albedo/roughness detail
- tighter near-field gating for normal-map cost
- one generic loose-ground scan plus at most one specialised grass/mud/forest scan
- periodic stochastic 3D phase warp for anti-tiling without extra PBR texture fetches
- geology-specific procedural PBR for all 13 bedrock families, including family roughness, crystalline/bedded/foliated structure, faults and close normal response
- live debug toggles for direct performance/quality comparison

Still required for the mature M5 implementation:

- dedicated scanned/procedural sand, scree, gravel and snow families
- optional scanned rock sets replacing procedural response where worthwhile
- quality-tier texture budgets for lower GPUs

### M6 — dense material microrelief — IMPLEMENTED, FIRST PASS

`spherical_geometry_clipmap_micro.gd` adds a dedicated circular near-field mesh at one quarter of L0 spacing. With the current ~0.75 m L0, the dense patch is ~0.1875 m spacing and ~24 m radius.

The ordinary L0 mesh has an ~18 m-radius central hole. L0 and the dense patch overlap for ~6 m; material microrelief fades to zero through that overlap and the underlying L0 is slightly sunk so both surfaces converge to the same base terrain without a hard stitched edge or persistent coplanar z-fighting.

`gpu_material_microrelief.gdshaderinc` supplies actual vertex displacement for features that are large enough for the dense topology to represent:

- family-specific rock blocks, joints, bedding, foliation, fractures and clast relief
- scree / talus irregularity
- river gravel structure
- broad sand relief
- soil aggregates
- wet/muddy ground relief
- snow undulation

The dense shader path uses the immutable coarse context plus GPU landform fields as its vertex-stage physical proxy. Final visible material identity is still classified in `fragment()` from the actual displaced slope, so the authoritative material system remains downstream of geometry synthesis.

Micro displacement is distance-gated and fades before the dense patch hands off to L0. Fine centimetre-scale texture remains normal/PBR/scatter detail rather than being aliased into ~19 cm vertices.

The Terrain debug tab can disable geometric microrelief while leaving the dense patch and normal terrain active, allowing direct topology/performance comparison.

### M7 — GPU scatter — IMPLEMENTED, FIRST PASS

`gpu_terrain_scatter.gd` creates static candidate MultiMeshes once, then only keeps snapped tangent-space windows centred near the camera and binds planet/origin/context uniforms. It does **not** classify individual candidates on the CPU.

`gpu_scatter_common.gdshaderinc` reconstructs each candidate from `INSTANCE_ID`, an absolute tangent-grid cell and the deterministic `gpu_scatter` seed. The GPU then:

- adds deterministic within-cell jitter
- reconstructs the spherical direction
- samples the immutable macro/context textures
- evaluates the same analytic geomorph height used by terrain
- derives landform and material suitability proxies
- accepts/rejects the candidate stochastically but deterministically
- chooses scale/orientation/colour variation
- collapses rejected candidates before rasterisation

Current candidate layers:

- **Grass clumps** — 96×96 candidates, 1.25 m cells, ~60 m nominal radius. Suitability follows vegetation potential, soil depth/stability, wetness, snow/ice/bare-biome suppression and landform context. Procedural three-blade clumps are generated without external assets and have deterministic size/colour variation plus low-cost wind bend.
- **Geological / scree stones** — 64×64 candidates, 2.5 m cells, ~80 m nominal radius. Suitability follows exposed/thin soil, mountain/scree context, erodibility and real bedrock family; colour is derived from the same 13-family rock palette.
- **River / depositional stones** — 64×64 candidates, 2.0 m cells, ~64 m nominal radius. Suitability follows discharge, deposition, sediment and the classifier's gravel signal; shape is flatter/smaller to represent water-worn stones.

The candidate windows are snapped, so moving the camera inside a cell does not slide scatter across the ground. The tangent anchor is rebuilt only after ~8 km of travel; the first pass can therefore repopulate at that rare re-anchor boundary.

Current limitations / next scatter work:

- scatter uses a context/landform **slope proxy** in the vertex stage rather than the exact fragment-stage displaced slope; this keeps the first pass cheap but should eventually be replaced by a local GPU suitability field or compute-generated candidate buffer
- no tree/shrub assets or procedural tree geometry yet
- no indirect-draw compaction yet; rejected candidates still execute vertex work and are collapsed in the shader
- no scatter shadows in this first performance-oriented pass
- no collision/interaction proxies for large rocks or trees yet
- quality tiers and density/radius controls still need tuning on target GPUs

### M8 — iterative near-field GPU erosion — OPTIONAL REFINEMENT

Add compute-generated local height/sediment fields only if the analytic erosion system is insufficient at walking distance. Compute input remains immutable coarse context + deterministic procedural base; the CPU does not synthesize terrain.

### M9 — mature GPU scatter / ecology

Promote the first M7 candidate-lattice implementation to a compacted GPU-driven ecology system:

- compute suitability from the exact final local terrain field
- append/compact accepted candidates into GPU buffers
- indirect draws per scatter family and LOD
- trees, shrubs, deadwood, biome debris and larger talus blocks
- stable planet-wide cell identity across tangent-anchor rebuilds
- quality-tier density and shadow budgets
- interaction/collision proxies only for nearby large objects

## Runtime invariant

For a fixed world seed and coarse map set, all untouched sub-grid terrain and visual scatter must be reproducible from planet position and GPU algorithms alone.
