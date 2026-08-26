# GPU-first terrain synthesis — latest 0.0.5 clipmap port

## Runtime contract

The CPU generates/loads persistent coarse planet maps. Sub-grid terrain detail, material classification and visual scatter belong on the GPU. Camera-centred CPU terrain/detail/material synthesis is not part of the target runtime architecture.

## Authoritative clipmap base

This branch is based directly on `terraintesting/0.0.5` and preserves the current 0.0.5 terrain renderer.

The active class chain is:

```text
spherical_geometry_clipmap_global_gpu.gd
  -> spherical_geometry_clipmap_global.gd
  -> spherical_geometry_clipmap_procedural_safe.gd
  -> current 0.0.5 clipmap parents
```

The GPU extension must not replace or regress these current clipmap invariants:

- compact UV-addressed centre/ring sector meshes
- screen-space promotion of the centre disc through L0..L14
- dynamic active minimum/maximum LOD window
- stationary per-level lattice snapping
- double-precision stable anchor reconstruction
- resident `Planet.global_height_texture` macro terrain
- resident `Planet.global_material_texture` material fallback
- cubic B-spline/mip-aware macro reconstruction
- centre/ring morph and ring-only sink behavior
- restoration of dynamic ring `visible_instance_count` after sector/debug visibility changes

## Current corrected-port status

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

`PlanetContext` is an autoload. The old camera-centred `MaterialClipmap` autoload is removed on this branch. The latest resident global material texture remains available through `spherical_geometry_clipmap_global.gd`.

### M2 — analytic GPU geomorph library — ACTIVE

`gpu_geomorph.gdshaderinc` provides the context-driven analytic terrain stack:

- multi-octave fBm and ridged structure
- domain warping
- reduced-cost cellular ridge structure
- mountain/arid/glacial/depositional landform weights
- multi-scale 16 km / 6 km / 1.4 km / 420 m / 120 m / 24 m detail
- drainage-oriented incision
- broad deposition/fans
- arid dunes
- glacial smoothing/flow
- LOD band limiting from actual geometry spacing

### M3 — latest-clipmap geomorph integration — ACTIVE, FIRST PASS

`shaders/spherical_geometry_clipmap_global_gpu.gdshader` is derived from the current 0.0.5 clipmap shader, not from the older GPU graphics branch.

Only the old generic `procedural_detail()` source has been replaced with `gm_geomorph_height()` in this first integration pass. The current 0.0.5 behavior for topology addressing, promoted centre LODs, stable anchoring, macro filtering, morphing and ring sinking is retained.

The active controller swaps to this shader after the latest global clipmap has initialized, then rebinds the resident global height/material textures and adds `PlanetContext`.

### M4 — physical material classifier — MODULE PRESENT, NOT YET ACTIVE ON CORRECTED PORT

`gpu_surface_classifier.gdshaderinc` has been transplanted as a module, but the current corrected live shader still uses the latest 0.0.5 resident global material map for fragment shading.

Next port step:

- derive slope from final displaced geometry
- classify bedrock/soil/vegetation/sand/mud/snow/scree/gravel from context + final geometry
- keep resident global material data as fallback/debug input

### M5 — PBR / anti-tiling / geology-specific rock — MODULES PRESENT, NOT YET ACTIVE

Available modules:

- `gpu_surface_pbr.gdshaderinc`
- `gpu_surface_antitile.gdshaderinc`
- `gpu_rock_pbr.gdshaderinc`

They will be re-integrated only after M3 is visually validated against the latest clipmaps.

### M6 — dense geometric microrelief — MODULE PRESENT, TO BE REDESIGNED FOR PROMOTED CENTRE LOD

`gpu_material_microrelief.gdshaderinc` is present, but the old dense microclipmap implementation is intentionally **not** copied.

The old implementation assumed a permanently active L0 centre disc. The current 0.0.5 renderer can promote the centre itself to L1..L14, so the dense near-field layer must be adapted to that behavior rather than replacing the latest topology.

### M7 — GPU scatter — NOT YET PORTED/ACTIVE ON CORRECTED BRANCH

The old scatter implementation is deliberately not active here yet. When ported it must sample the same current resident global height field (`Planet.global_height_texture`) and use the corrected terrain/context stack.

### M8 — iterative near-field GPU erosion — FUTURE/OPTIONAL

Add compute-generated local refinement only if analytic erosion remains insufficient at walking distance.

## Validation

CI validates both:

- the untouched current `spherical_geometry_clipmap_procedural_uv.gdshader`
- the new `spherical_geometry_clipmap_global_gpu.gdshader`
- the active project/autoload inheritance chain under Godot 4.7.1

This keeps the current clipmap shader available as an A/B baseline while the GPU terrain features are ported incrementally.

## Runtime invariant

For a fixed world seed and coarse map set, untouched sub-grid terrain should be reproducible from planet position and GPU algorithms alone.
