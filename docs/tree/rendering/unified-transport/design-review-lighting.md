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
- Not done: nothing consumes the table yet. Wiring the apparent Sun direction into the sun disc, shadows and the atmosphere LUT, and exposing surface refractivity as an authored/derived atmosphere property
  (which needs a schema property and RPC/MCP parity), are the next steps.
