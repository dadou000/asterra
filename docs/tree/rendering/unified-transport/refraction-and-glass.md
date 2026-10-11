+++
path = "/rendering/unified-transport/refraction-and-glass"
title = "Atmospheric refraction, hybrid visibility, glass and caustics (partly implemented)"
kind = "concept"
status = "experimental"
summary = """
Proposed design for refractive index as an optical medium property (horizon bending, mirages, heat shimmer), one hybrid ray-query service shared by \
mirrors, glass and mirages with confirmed/unresolved semantics, and a restructured glass and caustic pipeline that separates optics from visibility."""
keywords = ["refraction", "mirage", "ciddor", "hybrid visibility", "ray query", "glass", "caustics", "photon", "mirror", "screen space"]
related = ["/rendering/lighting", "/rendering/unified-transport"]
depends_on = ["/rendering/unified-transport/medium-and-clouds"]
invariants = [
  "Refractive index derives from temperature, pressure, humidity and wavelength (for example Ciddor); rays bend by dd/ds = [grad n - d(d.grad n)]/n.",
  "Fidelity tiers: planetary profiles, regional vertical profiles with sparse integration, local thermal-gradient shimmer; rays are curved only when projected distortion exceeds the image-error tolerance.",
  "Where bending changes solar visibility (near horizons, strong inversions) lighting uses the refracted source path, so a refracted Sun is not lit with an unrefracted visibility result.",
  "Rainbows and halos come from particle scattering/phase functions, not from treating a cloud as a lens.",
  "HybridVisibility distinguishes confirmed hit, confirmed miss and unresolved; a screen-depth match is not proof of the first physical surface; fallbacks run screen, then BVH/hardware ray query, SDF/proxy, terrain heightfield, analytic ocean, atmosphere and cloud representations.",
  "Fog and in-scattering are never applied twice to fallback radiance; temporal history follows the virtual reflected or refracted image.",
  "Glass computes a compact optical-path description (entry, exit, transmitted and reflected directions, throughput, absorption) before issuing shared visibility queries; thin panes use a cheap displacement/Fresnel/absorption path.",
  "Caustic photons are traced once in compute and stored with receiver hit, weight and differentials; footprints are drawn separately; photon budget follows projected size, irradiance, visibility and variance; multiple stars contribute independently.",
  "The default path works without ray-tracing hardware; hardware ray queries accelerate the same visibility requests rather than add a second lighting implementation.",
]
+++

## Implementation status

Done: planetary `RefractionTable` applied to the sun direction behind `ORBIT_REFRACT_SUN`; the caustic compute pre-pass behind `ORBIT_GLASS_CAUSTIC_COMPUTE`; the view-dependent photon
budget; the glass BVH trace bounded by the screen hit. `VisibilityRegistry` already has hit/miss/unresolved semantics but glass does not call it. Not done: local
inversions, mirages and heat shimmer (`BendRay` has no consumer), masked offscreen refracted visibility, separate glass optics, a thin-pane path, tile classification,
removing the backdrop copy, hardware ray queries (needs an RHI change). The flags are unverified on a GPU.

## Known hotspots (from source inspection, not GPU timings)

- The glass shader searches the environment separately for transmitted and reflected rays; its exact-triangle path does not use the hardware traversal option mirrors have.
- The caustic generator uses 96x96 to 224x224 photon cells per caster with six vertices per photon, each vertex recomputing the central and two neighbouring trajectories (over 900,000 path evaluations per object at maximum, before early exits).
- Glass keeps a full-resolution backdrop copy even when no glass pixel is visible.

Profile before prioritizing: only measured pass timings and counters establish the actual order.

## Plan

Separate optics from visibility, add bounded early rejection of negligible contributions, move photon generation to compute with cached adaptive
footprints (NVIDIA adaptive anisotropic photon scattering is the precedent), classify active tiles, compact fallback queries and drop unneeded backdrop
copies. Glass fixes can land before the rest of the architecture.
