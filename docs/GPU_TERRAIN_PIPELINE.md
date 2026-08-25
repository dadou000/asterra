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
   - material-specific micro-normal detail
   - dense near-field geometric microrelief

5. **GPU scatter**
   - vegetation, trees, rocks, talus, debris, river stones, etc.
   - consume the exact same context and final material classification

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

### M7 — scatter — NEXT

Generate scatter suitability from the same context, geomorph and final material fields, then feed GPU-driven vegetation and object placement. Initial scatter families should include:

- grass and low vegetation
- trees / shrubs by biome, moisture, soil depth and slope
- loose stones and exposed-rock fragments
- scree / talus blocks
- river stones / gravel clusters
- deadwood / litter where ecological context allows

Scatter must reuse the same deterministic process fields that shape terrain and materials so objects do not appear independently painted onto the ground.

### M8 — iterative near-field GPU erosion — OPTIONAL REFINEMENT

Add compute-generated local height/sediment fields only if the analytic erosion system is insufficient at walking distance. Compute input remains immutable coarse context + deterministic procedural base; the CPU does not synthesize terrain.

## Runtime invariant

For a fixed world seed and coarse map set, all untouched sub-grid terrain must be reproducible from planet position and GPU algorithms alone.
