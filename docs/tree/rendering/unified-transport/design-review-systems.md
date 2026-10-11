+++
path = "/rendering/unified-transport/design-review-systems"
title = "Design review part 2: glass, eclipses, visibility and lightning (measured)"
kind = "concept"
status = "experimental"
summary = """
Second half of the proposal review: the caustic photon redundancy and its compute pre-pass, why glass cost is in its environment queries and not its optics, which spec items already exist \
(visibility registry, finite-disc eclipses) and the limb-darkening gap, and the lightning simulation with the defects its first runs exposed."""
keywords = ["design review", "caustics", "photon", "glass", "limb darkening", "eclipse", "visibility registry", "lightning", "flash rate", "measurements"]
sources = [
  "engine/mesh_render/src/GlassSurface.cpp",
  "engine/celestial_lighting/src/CelestialLighting.cpp",
  "engine/storm_electrification/src/LeaderSimulation.cpp",
]
symbols = ["CausticPhotonRecord", "SampledObscuration", "TryFlash"]
invariants = [
  "Counts in this block (903,168 vs 150,528 glass traces, 8 percent fewer BVH node visits) come from the code and a CPU port, not from GPU timings; no GPU was available.",
  "Nothing here has been seen on screen or run on a GPU; see each linked card for what was and was not verified.",
]
related = ["/rendering/unified-transport/design-review", "/authoring/primitives", "/celestial/lighting", "/rendering/unified-transport/storm-electrification"]
depends_on = ["/rendering/unified-transport/design-review"]
verified = "1d3603b"
+++

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

## Finding 9: caustic photon budget followed physical size only; lightning light has a bounded path

- `GlassPhotonGrid` chose 96..224 photons per side from the body's size alone, so a glass covering nine pixels still traced 9,216 photons. The new view-dependent rule keeps at least two photons
  per pixel across the body (the density the old grid already had at its worst case), floors at 32 and never exceeds the old grid; off-screen bodies keep the full budget. Unit tests pin monotonicity,
  bounds and the density rule. Not seen rendered.
- `TransientLightsFromFlash` (lighting) turns a flash graph into at most N local lights per exposure window with the energy preserved, so lightning reaches surfaces through the existing tiled-light path, including its
  overflow aggregation. A 300 kJ stroke gives about 220 lux at 1 km under the assumed 150 lm/W. Clouds cannot see these lights yet: that needs a light list bound into the cloud march, which I did not build
  without a GPU to check descriptor layouts.
