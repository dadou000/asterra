+++
path = "/rendering/unified-transport/design-review-lighting"
title = "Design review part 3: photon budget, city lights and cloud light injection (measured)"
kind = "concept"
status = "experimental"
summary = """
Third part of the proposal review: the view-dependent caustic photon budget and transient lightning lights, the receiver-based cut over very large light sets with CPU timings at a million lamps, \
and cloud light injection. Refraction moved to design-review-refraction."""
keywords = ["design review", "photon budget", "city lights", "light cut", "cloud lights", "measurements"]
sources = [
  "engine/lighting/include/orbit/lighting/CityLightTree.hpp",
  "engine/lighting/include/orbit/lighting/TransientLights.hpp",
]
symbols = ["CityLightTree", "TransientLightsFromFlash"]
invariants = [
  "The numbers in the findings are CPU measurements or calculations. Only the cloud-light status note reports an on-screen check (2026-10-11).",
]
related = ["/rendering/unified-transport/design-review", "/rendering/unified-transport/design-review-systems", "/rendering/unified-transport/design-review-refraction", "/celestial/lighting"]
depends_on = ["/rendering/unified-transport/design-review"]
verified = "f39887e"
+++

## Finding 9: caustic photon budget followed physical size only; lightning light has a bounded path

- `GlassPhotonGrid` chose 96..224 photons per side from the body's size alone, so a glass covering nine pixels still traced 9,216 photons. The new view-dependent rule keeps at least two photons
  per pixel across the body (the density the old grid already had at its worst case), floors at 32 and never exceeds the old grid; off-screen bodies keep the full budget. Unit tests pin monotonicity,
  bounds and the density rule. Not seen rendered.
- `TransientLightsFromFlash` (lighting) turns a flash graph into at most N local lights per exposure window with the energy preserved, so lightning reaches surfaces through the existing tiled-light path, including its
  overflow aggregation. A 300 kJ stroke gives about 220 lux at 1 km under the assumed 150 lm/W. Clouds cannot see these lights yet: that needs a light list bound into the cloud march, which I did not build
  without a GPU to check descriptor layouts.

## Finding 10: the existing emissive hierarchy is sound but is for surfaces; discrete lights had no scalable path

- `EmissiveHierarchy` (quadtree over emissive surface radiance) was checked for the one way it could silently lose energy, the node cap in `SelectEmissiveHierarchy`: the invariant `selected + stack <= max`
  guarantees every subtree is eventually selected, so it cannot. It does not cover discrete emitters such as street lamps and windows.
- For those the flat `LocalLight` list scales with the light count every frame. Measured on the CPU with a square grid of downward lamps: at 1,000,000 lamps `BuildTiledLightGrid` takes 4.2 s and folds 26.8 million
  assignments into proxies; a 2,048-light cut from the new `CityLightTree` takes 0.35 ms to select (tree build 1.3 s once) and the tiled grid built from it 1.9 ms.
- Accuracy is excellent for the receiver the cut was made for (0.13 % under the lamps, 0.25 % for cloud bases 1.5 to 5 km up with only 256 lights standing in for 130,000) and terrible elsewhere: a cut made for a place
  3.8 km away is 99.8 % off at a surface that has merged away its own lamps. This is why the cut must be per receiver region; a camera-centred global light list would be wrong for far ground. Two defects were found while testing: the first
  accuracy numbers were vacuous (wrong receiver normal gave zero reference light) and the nearest leaves were never expanded because the tree selection consumed the whole budget.

## Finding 12: the timewarp invariant already holds; the weather map just runs fast

Audit of the spec's "query weather at absolute simulation time, no backlog": `EvaluateWeather` is a pure analytic function of direction and absolute seconds, so it holds by construction, and
measurement confirms it numerically: valid at every epoch up to 1e10 s (about 317 years), no precision loss (mean coverage change per second 1.37e-5 at t = 0 and at t = 1e10), no drift of global cover, circulation structure intact.
What the audit did find is pace: patterns reshuffle in about half a day (correlation 0.96 after 1 hour, 0.32 after 1 day) and the storm-track waves propagate at ~60 m/s against 10-15 m/s in nature. That is an authoring
choice recorded in `/rendering/clouds`, not changed here.

## Finding 13: cloud light injection was blocked by two questions that reading the code answered

I had declined to build it because I thought I could not know (1) whether lighting-frame positions are comparable with the cloud shader's and (2) where a per-frame buffer could live safely. Both were answerable from source:
`LightingView::cameraPositionInFrameMeters` is `Camera().localPositionMeters`, the same body-centric position the cloud view uses; and the glass renderer's `BeginFrame` ring (frames-in-flight mask) is a pattern CloudRenderer can copy exactly.
Built behind `ORBIT_CLOUD_POINT_LIGHTS=1` (see `/rendering/clouds/raymarch`): a fourth shader buffer, a light term that mirrors the sun's scattering formula, authored lights as the producer (reduced to 16 with the energy of the rest aggregated).
Verified: all cloud shader variants compile; with the flag off the three cloud shaders' SPIR-V is byte-identical to the previous commit; the light-enabled variant's bindings (buffers 0-3, textures 4-6) match the pipeline the C++ creates;
the C++ passes -fsyntax-only. Verified on screen (2026-10-11): an authored 1e11 lm light inside a cloud brightens a halo on exactly that cloud (2892 pixels brighter, 20 darker; noise 18/255), and 13-14 flash lights per frame reach the cloud pass while a flash is live (a daytime flash changed the cloud only marginally). Not verified: the night look. The flash photometry is consistent (a 300 kJ stroke is ~200 lux at 1 km, ~231 scene-sr of intensity) but the cloud look, the 150 m diffusion length and the gain are untuned. Lightning has a producer: `lighting::TransientFlashRegistry` (canned channel with return strokes, or any simulated `DischargeGraph` via `Add`) feeds the tiled-light pass and the cloud march through `StudioViewportRenderer::TransientFlashLights`; fire one with RPC `lightning.fire` / MCP `orbit_lightning_fire`. Unverified on screen; 'up' is camera up. `lightning.storm_start` drives it from the Weather Lab (needs dx = dz).
