+++
path = "/rendering/unified-transport"
title = "Unified weather, optics and light transport (proposed architecture)"
kind = "section"
status = "planned"
summary = """
Proposed architecture (not implemented) in which weather, atmospheric composition, optical/refractive fields, light sources, shared radiative transport, \
camera-independent lighting caches and camera-dependent rendering all consume one physical state. Covers clouds, haze, stars, cities, lightning, \
mirages, glass and caustics. Children give the per-area design and the implementation order."""
keywords = ["architecture", "weather", "optical medium", "radiance cache", "light transport", "lightning", "refraction", "glass", "caustics", "cloud lod", "many lights", "proposal"]
related = ["/rendering/clouds", "/rendering/atmosphere", "/rendering/lighting", "/rendering/volumes"]
verify = [
  "python tools/run_cpu_tests.py builds and runs every CPU-only test of this work on Linux (10 groups, ~75 s; storm electrification is ~30 s of it); python tools/check_cloud_shaders.py and python tools/check_glass_shaders.py compile-check the embedded HLSL. None of them runs a GPU: the opt-in flags ORBIT_CLOUD_FILTERED_FAR, ORBIT_GLASS_CAUSTIC_COMPUTE, ORBIT_REFRACT_SUN and ORBIT_CLOUD_POINT_LIGHTS are unverified on screen.",
]
invariants = [
  "The camera cannot change the physical atmosphere: cloud density, humidity, charge and light transport are independent of view distance.",
  "Hidden objects can affect lighting: camera culling never removes cloud shadowing or indirect illumination.",
  "LOD cannot create or destroy clouds; every representation derives from the same persistent weather state.",
  "Approximation quality is measured with geometric, optical and radiance error estimates, not arbitrary distance thresholds.",
  "Weather and lighting are queried at absolute simulation time; timewarp never accumulates a backlog of work.",
  "Transient bright events (lightning, caustics, transient lights) run within bounded GPU budgets.",
  "All light is accumulated as linear spectral radiance in consistent physical units before exposure and display conversion.",
  "An exhausted ray query is reported as unresolved, never as an authoritative miss.",
  "Fallbacks preserve energy: merge or probabilistically sample weak sources instead of deleting them.",
  "Every cached product carries planet, region, simulation time, source version, medium version and quality; stale and current generations are never silently mixed.",
]
+++

## Principle

Weather determines atmospheric conditions; those determine particles, moisture, temperature and charge; particle populations determine optical
properties; optical properties determine transport; sources introduce radiation; surfaces and cameras receive it. The shared part is physical
state, units, optical properties, visibility semantics, source descriptions and the cache dependency system. Each renderer keeps its own cheapest
numerical method, and there is no single monolithic shader.

Pipeline: weather and physical world -> atmospheric composition -> optical and refractive fields -> light sources and visibility -> shared radiative
transport -> camera-independent lighting caches -> camera-dependent rendering -> HDR exposure and eye adaptation.

Four representations stay separate, each with its own error budget: physical (temperature, pressure, moisture, particles, charge), optical (extinction,
scattering, absorption, phase, refractive gradients), lighting (visibility, transmission, incident and scattered radiance) and visual (how the camera
displays them). World state uses planet-relative high-precision coordinates with local floating-origin transforms for the GPU; no cache may change
meaning when the camera origin moves.

Weather tiers (climate, mesoscale systems, local storm simulation) switch between playback and active simulation using compatible state variables,
so a storm never reconstructs itself or resets its charge on promotion. Extreme timewarp evaluates absolute time with deterministic playback.

## Service ownership

| Service | Owns |
| --- | --- |
| WeatherAuthority | climate, local simulation, playback, deterministic time |
| AtmosphericComposition | vapor, aerosols, hydrometeors, temperature, pressure |
| OpticalMedium | extinction, absorption, scattering, refractive properties |
| OpticalHierarchy | sparse transport summaries and occupancy |
| EmitterRegistry | stars, cities, artificial sources, emissive surfaces, transients |
| HybridVisibility | screen queries, BVH, SDF, terrain, analytical fallbacks |
| RadianceTransport | direct and indirect transport, caches, shadowing |
| CloudPresentation | 2D / 2.5D / 3D cloud representations |
| StormElectrification, DischargeGraph, TransientRadiance | charge, leader/channel graph, fast event illumination |
| RefractivePresentation, TransparentMaterials | bent-ray reconstruction; glass optics and caustics |
| RenderScheduler | GPU budgets, invalidation, residency |
| CameraPhotometry | exposure, adaptation, display conversion |

These are ownership boundaries, not separate DLLs or dispatches; closely related functions may share kernels. Any new subsystem must satisfy the
hot-iteration rules (`/rules/hot-iteration`) and the MCP parity rule (`/rules/architecture`).

## Frame contract

A frame consumes an immutable physical snapshot for its simulation time: resolve changed data, update or stream optical structures, resolve source
importance, publish verified cache generations, give direct and transient light priority, choose cloud/glass/refraction quality by image error and
budget, then composite in linear HDR before adaptation, bloom and tone mapping. Progressive cache refinement continues for later frames.

Strategy: first make existing systems agree physically, then make them share expensive work, then pursue elaborate approximations. Every optimization
reports both its saving and the error it introduces; no speedup is assumed before measurement.

## Children

- `medium-and-clouds`: composition, haze, optical hierarchy, stable cloud LOD.
- `light-and-lightning`: stars, many lights, radiance caches, electrification, transient lighting.
- `refraction-and-glass`: mirages, hybrid visibility, glass and caustics.
- `plan-and-validation`: current-code corrections, phased order, scenarios.
- `design-review-lighting`: photon budget, transient and city lights, refraction findings.
- `design-review-systems`: glass, caustics, eclipses, visibility and lightning findings.
- `design-review`: measured findings that adjust the proposal (cloud far-field bias, linear transmittance filtering, light overflow fix).
- `storm-electrification`: CPU reference for charging, the electric field, leader flashes and a timewarp-safe flash schedule.
- `core`: the CPU reference library (`engine/optical_transport`) implementing the numerical core; renderers do not use it yet.
