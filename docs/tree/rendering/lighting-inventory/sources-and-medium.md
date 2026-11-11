+++
path = "/rendering/lighting-inventory/sources-and-medium"
title = "Inventory: star, atmosphere, weather and clouds"
kind = "concept"
status = "draft"
summary = """
The systems that decide how much light exists and what it passes through: the star and orbits, the atmosphere and its \
LUTs, the weather model and cloud field, the cloud renderer with its light volume and shadow pass, optical transport \
and the planetary appearance product. For each: purpose, residency, size and update rate."""
keywords = ["star", "atmosphere", "lut", "sky view", "weather", "cloud field", "cloud shadow", "light volume", "storm", "optical transport"]
sources = [
  "engine/celestial_atmosphere/include/orbit/celestial_atmosphere/Atmosphere.hpp",
  "engine/celestial_clouds/include/orbit/celestial_clouds/CloudField.hpp",
  "engine/celestial_clouds/src/CloudRenderer.cpp",
  "engine/optical_transport/include/orbit/optical_transport/CloudRepresentation.hpp",
]
related = ["/rendering/lighting-inventory", "/rendering/atmosphere", "/rendering/clouds"]
depends_on = ["/rendering/lighting-inventory"]
verified = "d639bc09"
+++

## Star, time and bodies

| System | Role | Residency | Rate |
|---|---|---|---|
| Stellar appearance and radiometry | Luminosity, temperature, limb darkening, angular size, scene scale | cpu | static |
| Orbits and rotation | State(time); the sun direction in the body-fixed frame | cpu | per frame |
| Finite disc and occultation | Apparent disc, eclipses, partial visibility; HLSL mirror for shaders | cpu + shader code | per query |

## Atmosphere

| System | Role | Residency | Size / rate |
|---|---|---|---|
| Authoring solver (world_model) | Pressure, composition and aerosols to coefficients, with provenance | cpu | on edit |
| AtmosphereParameters | The single persistent authority per body; nothing Earth-specific | cpu | on edit |
| Static LUTs | Transmittance 128x32, multiple scattering 32x16; `SunTransmittanceAt` at any altitude | cpu->gpu | ~70 KB, on edit |
| Sky-view LUT | 128x64 view/azimuth radiance per observer; sun direction snapped to 1/512 | cpu->gpu | ~64 KB, on altitude or sun change |
| Sky SH summary | Order-2 SH from the CPU sky texels; every ambient consumer reads it; cloud-blind | cpu | 27 floats, per view |
| AtmosphereRenderer | Composites scene*T + L from ground to orbit | gpu | per frame |

## Weather

| System | Role | Residency | Size / rate |
|---|---|---|---|
| WeatherModel, storm schedule, sparse zones | Climate coverage, cloud type, precipitation; analytic zone survey | cpu | per 1 s quantum |
| CloudField | Per cube-sphere texel (193^2 x 6, per layer): coverage, optical depth, single-scattering albedo, anisotropy, type, precipitation; layer base/top and peak optical depth | cpu, with GPU HostVisible copy | ~3.6 MB per layer, per product rebuild |
| GpuStormSolver | 500 km storm domain, 14 compute kernels, nested fields for the cloud renderer | gpu | per sim step; readback only for checkpoints |
| Storm electrification | Charge, field, leader flashes, DischargeGraph | cpu | per flash |
| Cloud lab | One isolated cloud via RPC (type, radius, density, maturity, sun override) | cpu->gpu constants | on edit |

## Clouds

| System | Role | Residency | Size / rate |
|---|---|---|---|
| CloudRenderer (system A) | Half-res march of the first layer into RGBA16F; its own hand-tuned ambient `(0.02 + 0.16 h^2)` and four-octave multiple scattering | gpu | per frame |
| Unified cloud system B | Separately owned backend: layered extinction, nested storm sampling, sun cache; experimental | gpu | per frame |
| Far-field LUT | Mean exp(-K s) at four scales over (hardness, coverage) | cpu->gpu | ~1 KB, static |
| Light volume | Three toroidal cascades (250 m, 2 km, 8 km cells; ~11, 92, 368 km), generation-tagged, a few voxels per frame | gpu | MB-scale |
| CloudShadow pass | Half-res per-pixel 16-step march toward the sun; multiplies the stellar term only | gpu | per frame |

## Cross-cutting

| System | Role | Residency |
|---|---|---|
| Optical transport core | Std-only: CacheStamp invalidation, aerosols, Jensen-aware TransmittanceStats, hierarchy, emitter tree, discharge graph, refraction table | cpu |
| Planetary appearance product | Revisioned far-view product (albedo, climate, water) for orbit | cpu->gpu |
| Macro globe, far renderer, ocean optics | Orbital rungs of the representation ladder | gpu |
| Celestial scheduler | Frame-budgeted, prioritised derived work (atmosphere LUT and sky, cloud field, impostors) with CPU or GPU backend | cpu |

## Facts worth remembering

- `CloudTexel` already carries optical depth, single-scattering albedo and anisotropy per column: the layer
  description a two-sided cloud model needs.
- The shadow pass and the light volume march the same density as the camera march, so every density source stays in
  parity.
- Cloud shadow multiplies only the stellar term. Ambient is untouched: under overcast the ground gets full clear-sky
  ambient and no sun.
