# GPU-first terrain synthesis — latest 0.0.5 clipmap port

## Runtime contract

The CPU generates/loads persistent coarse planet maps. Sub-grid terrain detail, material classification and visual scatter belong on the GPU. Camera-centred CPU terrain/detail/material synthesis is not part of the target runtime architecture.

## Authoritative clipmap base

This branch is based directly on `terraintesting/0.0.5`. The fixed 0.0.5 clipmap source files remain untouched; GPU features are layered through subclasses and new shaders.

The active class chain is:

```text
spherical_geometry_clipmap_micro.gd
  -> spherical_geometry_clipmap_global_gpu.gd
  -> spherical_geometry_clipmap_global.gd
  -> spherical_geometry_clipmap_procedural_safe.gd
  -> current 0.0.5 clipmap parents
```

Preserved clipmap invariants:

- compact UV-addressed centre/ring sector meshes
- screen-space promotion of the centre disc through L0..L14
- dynamic active minimum/maximum LOD window
- stationary per-level lattice snapping
- double-precision stable anchor reconstruction
- resident `Planet.global_height_texture` macro terrain
- resident `Planet.global_material_texture` fallback
- cubic B-spline/mip-aware macro reconstruction
- centre/ring morph and ring-only sink behavior
- dynamic sector visibility

## Current port status

### M1 — immutable GPU planet context — ACTIVE

`gpu_planet_context.gd` uploads six-face context textures once per adopted world:

- soil composition
- soil depth/moisture/vegetation/sediment
- geology/erodibility/strata/basin/boundary context
- uplift/fault/floodplain/wetland
- climate
- hydrology/flow/discharge/deposition
- nearest-filtered rock family ID
- nearest-filtered biome ID

`PlanetContext` is an autoload. The old camera-centred `MaterialClipmap` autoload is removed.

### M2 — analytic GPU geomorph — ACTIVE

`gpu_geomorph.gdshaderinc` provides:

- multi-octave fBm and ridged structure
- domain warping
- reduced-cost cellular ridge structure
- mountain/arid/glacial/depositional landform weights
- 16 km / 6 km / 1.4 km / 420 m / 120 m / 24 m bands
- drainage-oriented incision
- broad deposition/fans
- arid dunes
- glacial smoothing/flow
- LOD band limiting from actual geometry spacing

### M3 — latest-clipmap geomorph integration — ACTIVE

The live surface shader is derived from the current 0.0.5 shader. Stable anchoring, macro filtering, promoted-centre LOD, morphing, ring sinking and UV topology are preserved while sub-grid displacement comes from `gm_geomorph_height()`.

### M3.1 — horizon-exact physical clipmap window — ACTIVE

The corrected GPU controller physically packs only the active annuli into each sector MultiMesh. It does not leave an L1..L14 instance tail allocated behind `visible_instance_count`.

A coarse level is omitted when its inner annulus edge begins beyond the current horizon-safe visible cap. Therefore:

- levels which can contribute are real GPU instances;
- levels completely below the geometric horizon are absent from the draw buffer;
- debug/sector visibility changes cannot resurrect horizon-culled levels.

HUD diagnostics report `physical rings` and `horizon max L#` separately from the logical LOD selector.

### M4 — physical material classifier — ACTIVE

The final displaced surface normal is reconstructed from derivatives and used for slope/material stability. Classification combines final geometry with immutable planet context.

Current classes:

- bedrock
- mineral soil
- vegetation
- aeolian sand
- mud
- snow
- scree/talus
- river gravel

Loose materials are suppressed as slope exceeds plausible stability/repose limits, exposing actual bedrock family.

### M5 — PBR / anti-tiling / geology-specific rock — ACTIVE

The live shader integrates:

- `gpu_surface_classifier.gdshaderinc`
- `gpu_surface_pbr.gdshaderinc`
- `gpu_surface_antitile.gdshaderinc`
- `gpu_rock_pbr.gdshaderinc`

Existing scanned CC0 terrain assets are triplanar sampled for mineral ground, vegetation, mud and forest floor. Detail coordinates use a 4096 m precision-safe wrapped origin. All 13 current bedrock families receive geology-specific procedural rock response.

### M5.1 — terrain aerial perspective / atmospheric veil — ACTIVE

`terrain_aerial_gpu.gdshaderinc` evaluates optical depth between the camera and terrain. The terrain shader applies atmospheric transmittance to sky/direct ground radiance and adds Rayleigh/Mie in-scattering.

This is distance/path-length aerial perspective rather than a generic linear fog:

- close terrain remains clear;
- distant terrain progressively loses contrast/saturation;
- terrain near the geometric horizon receives the strongest atmospheric veil;
- default terrain aerial strength is `0.78` and can be changed at runtime.

### M6 — dense geometric microrelief — ACTIVE, PROMOTED-CENTRE SAFE

`spherical_geometry_clipmap_micro.gd` adds a dense 1/4-L0 near-field patch only while the promoted centre level is L0.

With the current 0.75 m L0 spacing:

- micro spacing is about 0.1875 m;
- dense radius is about 24 m;
- L0 hole radius is about 18 m;
- overlap converges through a controlled sink/fade.

When screen-space LOD promotes the centre to L1 or coarser, the untouched full centre-sector meshes are restored and the micro draw disappears completely.

The micro identity is stored in `INSTANCE_CUSTOM.w`; `.z` remains exclusively the annulus flag, so the dense centre patch can never receive ring sinking.

### M7 — GPU scatter — ACTIVE

`TerrainScatter` uses `gpu_terrain_scatter_global.gd`, which layers the compute-compacted/fallback GPU scatter implementation onto the latest resident terrain architecture.

Both paths sample:

- `Planet.global_height_texture`
- immutable `PlanetContext`
- the same deterministic GPU geomorph function used by terrain
- the physical GPU material classifier

Candidate classes currently include grass, geological/scree stones and river/depositional stones.

The preferred Forward+/Mobile path performs candidate classification and compaction in a RenderingDevice compute shader, writes accepted transforms directly into MultiMesh GPU buffers and uses indirect instance counts without GPU-to-CPU readback. The inherited vertex-stage candidate renderer remains the compatibility fallback.

### M8 — iterative near-field GPU erosion — FUTURE/OPTIONAL

Add compute-generated local refinement only if analytic erosion remains insufficient at walking distance.

## Validation

Godot 4.7.1 CI validates:

- untouched current 0.0.5 terrain shader
- corrected latest-clipmap geomorph shader
- full physical/PBR/aerial surface shader
- scatter draw shaders
- scatter compute SPIR-V
- active `GroundGeometryClipmap` inheritance/autoload chain
- active `TerrainScatter` inheritance/autoload chain

## Runtime invariant

For a fixed world seed and coarse map set, untouched sub-grid terrain and pristine visual scatter are reproducible from planet position and GPU algorithms alone.