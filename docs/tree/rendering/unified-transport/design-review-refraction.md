+++
path = "/rendering/unified-transport/design-review-refraction"
title = "Design review part 4: planetary refraction (measured)"
kind = "concept"
status = "experimental"
summary = """
Planetary refraction derived from radius, surface refractivity and scale height: validation against the standard Earth formula, the ducting limit, the sun-direction wiring behind ORBIT_REFRACT_SUN and its on-screen check."""
keywords = ["design review", "refraction", "ducting", "horizon", "refracted sun", "measurements"]
sources = [
  "engine/optical_transport/include/orbit/optical_transport/Refraction.hpp",
  "engine/celestial_atmosphere/src/AtmosphereRefraction.cpp",
  "engine/studio_ui/src/StudioViewportRenderer.cpp",
]
symbols = ["RefractionTable", "RefractDirection", "RefractionTableFor"]
invariants = [
  "The table values are CPU calculations. The on-screen check (2026-10-11) is the sun edge height at 20 m above the sea, flag on vs off.",
]
related = ["/rendering/unified-transport/design-review", "/rendering/unified-transport/design-review-lighting", "/rendering/unified-transport/refraction-and-glass"]
depends_on = ["/rendering/unified-transport/design-review"]
verified = "f39887e"
+++

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
- Wired (opt-in, verified on screen 2026-10-11): `ORBIT_REFRACT_SUN=1` refracts `studioDirectLight.directionBody` once in `StudioViewportRenderer`; every consumer I could find reads that struct, so the sky, light, shadow, glass, clouds and the sun disc drawn by the atmosphere pass agree.
- Verified on screen: 20 m above the sea, the sun's upper edge sits 1, 6, 12 and 37 px higher with the flag on at true elevations of about 1.4, 0.6, 0.2 and -0.2 degrees, matching lifts of 0.2 to 0.5 degrees; a sun 1.17 degrees below the horizon stays hidden. Not done: the sun disc drawn by the far-body pass (from orbit) is not refracted. Wiring the apparent Sun direction into the sun disc, shadows and the atmosphere LUT, and exposing surface refractivity as an authored/derived atmosphere property
  (which needs a schema property and RPC/MCP parity), are the next steps.
