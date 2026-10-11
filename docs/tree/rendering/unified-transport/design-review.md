+++
path = "/rendering/unified-transport/design-review"
title = "Design review of the unified-transport proposal (measured)"
kind = "concept"
status = "experimental"
summary = """
What measurement and code reading showed about the proposal: the shipping cloud far-field model is wrong by tens of percent of opacity, a four-channel \
Laplace representation makes transmittance filtering linear and accurate, several proposed items already exist or need less machinery than specified, and \
the tiled local-light overflow is fixed with energy-preserving proxies. Records numbers, method and limits."""
keywords = ["design review", "cloud far field", "laplace transmittance", "filtering", "coverage", "light overflow", "proxy light", "refraction", "validation", "measurements"]
sources = [
  "tools/prototypes/cloud_far_field_probe.cpp",
  "engine/optical_transport/include/orbit/optical_transport/TransmittanceStats.hpp",
  "engine/lighting/src/LocalLightRegistry.cpp",
  "engine/celestial_clouds/src/CloudRenderer.cpp",
]
symbols = ["TransmittanceStats", "BuildTiledLightGrid", "gDensityFar", "kFarFadeStart"]
invariants = [
  "Measurements here were made on the CPU by porting the shader's noise bake and base-shape threshold; no GPU was available, so nothing below is a rendered-image or frame-time result.",
  "Coverage is not occupied fraction: for the baked cumulus shape, coverage 0.2 fills 2 % of the area and coverage 0.8 fills 95 %, so any model that treats coverage as the fraction of sky covered is biased in both directions.",
]
related = ["/rendering/unified-transport", "/rendering/unified-transport/medium-and-clouds", "/rendering/clouds/raymarch"]
depends_on = ["/rendering/unified-transport/core"]
verify = [
  "g++ -O2 -std=c++20 tools/prototypes/cloud_far_field_probe.cpp && ./a.out reproduces the tables below.",
  "The lighting and optical_transport test executables (see their cards) pass.",
]
verified = "4d2b8fa"
+++

## Finding 1: the far-field cloud model is biased, not just approximate

`CloudRenderer.cpp` fades cloud shape to the weather coverage (`shape = lerp(shape, coverage, baseBlend)`) and fades extinction from `gDensity` (8) to
`gDensityFar` (0.4) between footprints of 100 and 400 m. The probe ports the noise bake and compares the far model with the area average of the
near model, for a cumulus-hardness cloud (P0 = column optical depth at shape 1 and density 1; alpha = 1 - transmittance):

| coverage | area actually occupied | true alpha, P0 = 5 | shipping far alpha | true alpha, P0 = 13.6 | shipping far alpha |
| --- | --- | --- | --- | --- | --- |
| 0.2 | 0.02 | 0.02 | 0.33 | 0.02 | 0.66 |
| 0.5 | 0.42 | 0.41 | 0.63 | 0.42 | 0.93 |
| 0.8 | 0.95 | 0.95 | 0.80 | 0.95 | 0.99 |

Low coverage is filled with haze that is not there at close range; high coverage is thinned for moderate P0 and slightly over-dense for large P0. Approaching a distant region therefore changes how much
sky it covers, which matches the observed "broad formations that shrink into isolated bodies". Two compensating authored hacks (the density fade and the
shape blend) hide each other only for particular P0.

## Finding 2: filter linearly, in Laplace space

A pixel shows `mean(exp(-K s))` where the scale K is set by thickness, slant and density. That is linear in the samples, so storing it at a few K values
and filtering the channels in an ordinary mip is exact (`TransmittanceStats`). With K = 0.5, 2, 8, 32 the probe's maximum transmittance error was 0.027 (2-moment
occupancy model: 0.070; mean optical depth only: 0.59). One mechanism then covers vertical view, slanted view, sun rays and density multipliers, and it
removes the need for the density fade. Exact for a column view; slanted paths through non-uniform media remain an approximation.

For the procedural cloud the footprint statistics are separable (weather supplies coverage, the noise statistics are universal), so they are a small
2-D lookup over (coverage, footprint) per cloud type, generated once from the noise, and no per-brick hierarchy is needed. A hierarchy is only needed for
simulated or authored densities (the Weather Lab storm).

## Finding 3: light overflow was silent energy loss (fixed)

`BuildTiledLightGrid` kept the first N lights per tile in submission order. A strong light submitted after a crowd of weak ones disappeared. Now strong
lights are kept and the overflow becomes one shared proxy light with the summed flux (test cases 3-10 in `LocalLightRegistryTests.cpp`). The shader and
buffer layout are unchanged because a proxy is an ordinary entry; spot cone shaping is lost inside a proxy.

## Finding 4: items that are smaller or already present

- The cloud light volume (cached optical depth toward the sun, with a settle-skip) already exists; the proposed "directional cumulative optical depth" is a refinement,
  not a gap. It is camera-centred, which conflicts with the camera-independence goal and is the real follow-up.
- Horizon refraction from `AirRefractivity` plus Snell's law in spherical shells reproduces standard values (23.5' at 1 degree above the horizon, 17.8' at 2,
  9.7' at 5, 5.2' at 10, 1.65' at 30), about 7 px at 1 degree for a 60 degree, 1080p view. It is a smooth function of elevation and observer height, so a lookup table
  serves the sun and sky. Ducting needs a lapse rate steeper than about -0.11 K/m (gradient equal to Earth's curvature), so curved-ray work is only needed in thin
  near-surface inversion or superadiabatic layers.
- Lighting already has an emissive hierarchy (`EmissiveHierarchy`); a city-light tree should extend it, not duplicate it.

## Finding 5: the caustic photon redundancy is real and exactly as large as the spec said

`kCausticVertex` ran the full photon trace in every one of the six vertices of a photon quad: 3 `TraceGlass` calls and one `MarchScene` per vertex, 903,168
glass traces per caster at the maximum 224 x 224 grid (3 x 6 x 224^2). A compute pre-pass (`ORBIT_GLASS_CAUSTIC_COMPUTE=1`, default off) traces each photon once
(150,528) and the splat vertex shader reads the stored record. Counts come from the code, not from a GPU timing. Verified: all five shader variants compile with
`glslangValidator`, the legacy pixel shader's SPIR-V is byte-identical to before the refactor, the legacy vertex shader keeps the same bindings, and the C++ passes a
syntax check. Not verified: any run on a GPU (descriptor layout, barriers, image equivalence, speedup). See `/authoring/primitives`.

## Finding 6: glass optics are cheap; the environment queries are the cost

The spec proposes a separate optics pass and a thin-pane fast path. In the code, `TraceGlass` is already only two analytic spans and a refraction for a slab, so a thin-pane
path has little to remove. The two `EnvironmentTrace` calls per pixel are the real cost. What can be removed exactly is small: bounding the BVH walk by the screen hit
(done, 8 % fewer node visits on a synthetic room, identical results). Cutting more means lowering fidelity for low-Fresnel reflections, which is a visible trade-off and
needs a GPU and an art decision. Hardware ray queries need an RHI change first: only compute pipelines can bind an acceleration structure today.

## Finding 7: more of the spec already exists than it assumed

- **Hybrid visibility service:** `VisibilityRegistry` (lighting) already returns Hit / Miss / Unresolved with per-query confidence and a `terminal` flag, selects providers by
  stated guarantees, and has screen-space, analytic, terrain-heightfield, software-proxy and hardware-ray-query backends. The gap is consumers: glass runs its own tracing in
  the shader instead of asking it.
- **Finite stellar disc and eclipses:** `FiniteDiscOccultation` / `CombinedOccultation` already integrate the union of occluder discs over the source disc, and mesh shadows
  use PCSS sized by the Sun's angular radius. The real gap was limb darkening: eclipse fractions were shares of area. Fixed (`ApparentDisc::limbDarkening`), tested against an
  independent planar quadrature, wired from the emitter's stellar appearance. A stale header comment that claimed multiplicative composition was corrected.
- **Cloud light volume, emissive hierarchy:** exist (see Finding 4).

## Finding 8: lightning had nothing to stand on, and two defects only simulation exposed

- The storm solver (`FastStormSolver`) is warm-rain Kessler: no ice, no graupel. The spec's "the weather solver supplies ice and graupel" is not true today, so
  `/rendering/unified-transport/storm-electrification` takes ice, graupel and supercooled liquid as caller-supplied fields.
- Building the flash simulation exposed two defects that unit-level reasoning would have missed: with the channel held at a fixed potential every flash filled the entire channel cap and
  neutralised +-390 C (unphysical, fixed with a charge budget), and a strongly charged storm held more than the budget around a single cell so flashes never started (fixed with a minimum channel
  extent and by capping the charge actually removed). Flash counts then rose monotonically with updraft (4 / 27 / 78 in 15 minutes at 8 / 18 / 30 m/s).
- My first updraft sweep decreased with updraft because the synthetic storm held condensate fixed while only transport changed: the model was behaving, the experiment was wrong.

## Integration status of Finding 2

Implemented behind `ORBIT_CLOUD_FILTERED_FAR=1` (default off, see `/rendering/clouds/raymarch`): `CloudFarField` bakes the table from the
cloud noise using `TransmittanceStats`, the shader blends to it by `baseBlend`, and all four `Extinction()` callers pass their step length. Verified: the three
shader variants compile with `glslangValidator` with the flag off and on; the table builder and the CPU twin of the shader lookup pass
`CloudFarFieldTests` (off-lattice error 0.012 against an independent estimate). Not verified: the HLSL lookup numerically (only
the CPU twin), DXC compilation, the image from orbit and during descent, frame time, and whether `gStep` is the right K for the god-ray march.
Turning it on removes the old thin-haze look from orbit by design, which needs an art decision.

## Not yet done

A rendered approach sequence comparing flag off and on; frame-time measurement; a camera-independent replacement for the camera-centred light volume;
removing the old blend and fade once the flag is the default.
