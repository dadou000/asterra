+++
path = "/rendering/unified-transport/design-review-lighting"
title = "Design review part 3: photon budget, city lights and refraction (measured)"
kind = "concept"
status = "experimental"
summary = """
Third part of the proposal review: the view-dependent caustic photon budget and transient lightning lights, the receiver-based cut over very large light sets with CPU timings at a million lamps, \
and planetary refraction derived from radius, surface refractivity and scale height with its validation and ducting limit."""
keywords = ["design review", "photon budget", "city lights", "light cut", "refraction", "ducting", "horizon", "measurements"]
sources = [
  "engine/lighting/include/orbit/lighting/CityLightTree.hpp",
  "engine/lighting/include/orbit/lighting/TransientLights.hpp",
  "engine/optical_transport/include/orbit/optical_transport/Refraction.hpp",
]
symbols = ["CityLightTree", "TransientLightsFromFlash", "RefractionTable"]
invariants = [
  "All numbers here are CPU measurements or calculations; nothing was run on a GPU or seen on screen.",
]
related = ["/rendering/unified-transport/design-review", "/rendering/unified-transport/design-review-systems", "/celestial/lighting"]
depends_on = ["/rendering/unified-transport/design-review"]
verified = "6614c30"
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

## Finding 11: refraction needs a table, not ray marching, for the sky; the engine has none at all

- The atmosphere docs list "no atmospheric refraction" as an intentional limit, and nothing in the engine bends the Sun or the horizon. The spec's curved-ray machinery is only needed for local layers.
  Planetary refraction is a smooth function of elevation and observer altitude, so `RefractionTable` precomputes it (50 ms to build) from radius, surface refractivity and scale height,
  which the atmosphere solver already has for every authored planet; surface refractivity follows from the same number density and composition factor it uses for Rayleigh scattering.
- Validation against the standard Earth formula: within 2.1 % below 2 degrees and 5.9 % above (the standard formulas themselves embed a refractivity about 6 % above the physical 2.78e-4 at 550 nm).
  Derived facts reproduced: horizon refraction ~35 arcminutes, a setting Sun ~15 percent flattened, horizon dip reduced by ~7 percent. My first guess in the header, "0.4-0.6 flattening", was wrong; the numbers corrected it.
- Bug found by running other planets: a Venus-class atmosphere (gradient steeper than the planet's curvature) traps horizon rays; the table now reports ducting instead of producing nonsense. An earlier version also used `acos`
  instead of `asin` for the grazing-ray angle and silently truncated every row at the horizon.
- Also built: `celestial_atmosphere::RefractionAtmosphereFor / RefractionTableFor / RefractDirection` derive the table from `AtmosphereParameters` (no new authored property) and rotate a true direction to the
  apparent one (a sun 0.3 degrees below the horizon is still seen; 1 degree below is not; a Venus-class atmosphere returns the true direction). Wiring hazard found while preparing it: `directionBody` is body-level and
  observer independent, while refraction depends on the local vertical, so it must be applied once after the observer is known and read by every consumer, or shadows and the disc disagree with the light.
- Wired (opt-in, unverified on screen): `ORBIT_REFRACT_SUN=1` refracts `studioDirectLight.directionBody` once in `StudioViewportRenderer`; every consumer I could find reads that struct, so the sky, light, shadow, glass and clouds agree.
- Not done: the sun disc drawn by the far-body pass (from orbit) is not refracted, and nothing verifies the look. Wiring the apparent Sun direction into the sun disc, shadows and the atmosphere LUT, and exposing surface refractivity as an authored/derived atmosphere property
  (which needs a schema property and RPC/MCP parity), are the next steps.

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
the C++ passes -fsyntax-only. Not verified: any run. The flash photometry is consistent (a 300 kJ stroke is ~200 lux at 1 km, ~231 scene-sr of intensity) but the cloud look, the 150 m diffusion length and the gain are untuned. Lightning has a producer: `lighting::TransientFlashRegistry` (canned channel with return strokes, or any simulated `DischargeGraph` via `Add`) feeds the tiled-light pass and the cloud march through `StudioViewportRenderer::TransientFlashLights`; fire one with RPC `lightning.fire` / MCP `orbit_lightning_fire`. CPU-measured: energy conserved within 0.1 %, deterministic per seed, 16-light budget. Not verified: any rendered frame. Limits: 'up' is camera up (a rolled camera tilts the channel); the channel is canned, not the storm solver's output.
