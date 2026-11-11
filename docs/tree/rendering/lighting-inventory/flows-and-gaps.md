+++
path = "/rendering/lighting-inventory/flows-and-gaps"
title = "Inventory: CPU/GPU flows, duplicated mechanisms and gaps"
kind = "concept"
status = "draft"
summary = """
Where data crosses the CPU/GPU boundary today and at what size and rate, which mechanisms duplicate each other, what is \
missing for a cloud-aware ambient system, and candidate residency moves with their risk. The decision input for the \
unified lighting plan."""
keywords = ["transfer", "readback", "residency", "duplicate", "gap", "ambient", "cloud aware", "horizon map", "lighting state block"]
sources = [
  "engine/studio_ui/src/StudioViewportTerrainSunShadow.cpp",
  "engine/celestial_clouds/src/CloudField.cpp",
  "engine/lighting/src/RadianceClipmapResidency.cpp",
  "engine/weather_gpu/src/GpuStormSolver.cpp",
]
related = ["/rendering/lighting-inventory", "/rendering/unified-transport/medium-and-clouds"]
depends_on = ["/rendering/lighting-inventory"]
verified = "d639bc09"
+++

## CPU to GPU transfers at runtime

| Data | Size | Rate | Notes |
|---|---|---|---|
| Sun pose, irradiance, sky summary (push constants) | tens of bytes | per frame | Sky summary also bakes into cells and drives refresh fingerprints |
| Atmosphere LUTs | ~70 KB | on parameter edit | Built on the CPU |
| Sky-view LUT | ~64 KB | altitude or sun change | CPU texels also feed the SH |
| CloudField product | ~3.6 MB per layer | product rebuild | Whole-product HostVisible buffer; incremental policy unchecked |
| Sun-shadow field | 2 MB | per terrain revision | Staged 256 KiB per frame |
| Radiance cell snapshot | 96 B per changed cell | ~64 cells per frame, bursts on refresh | Estimated on the CPU frame thread |
| Level tables (terrain ambient) | 80 B x 20 | per frame ring | Host-visible |
| Cloud lab, surface effect stamps, local lights | KB | on edit or per frame | |

## GPU to CPU readbacks

| Data | Size | Rate | Notes |
|---|---|---|---|
| Ground height (clipmap) | 16 B | per frame, frames-in-flight delay | The pattern to copy for any diagnostic |
| Final-gather statistics | 16 B | per frame slot | Pass culling |
| GPU timestamps | small | per frame | Profiler |
| Storm solver checkpoints | large | rare | Not per frame |

## Duplicated mechanisms

- Five ambient approximations: the sky SH summary, the cell sky channel, the clipmap terrain ambient, the analytic
  fallback, and the cloud shader's own height-based ambient. None knows about clouds.
- Three sun-visibility structures: the coarse global sun-shadow field, the per-pixel cloud shadow march, the cloud
  light volume (plus the proxy hardware ray).
- Two multiple-scattering models: the atmosphere's geometric-series LUT and the cloud shader's four octaves.
- Two cloud renderers: system A and the experimental system B.
- Two copies of finite-disc visibility: CPU (`celestial_lighting`) and HLSL (`SunVisibilityShader`).

## Gaps for cloud-aware lighting

- No cloud effect on ambient: shadow only multiplies the sun.
- No two-sided cloud response (transmit when viewed from below, reflect when viewed from above); the cloud-top
  albedo for orbit is separate and ad hoc.
- No penumbra by distance below the cloud; no forward-scattering softening.
- No spatial variation of sky light under or between clouds; no in-cloud sky or ground-bounce term from real data.
- No GPU-resident single block of lighting state: the same numbers are copied to push constants, cells and CPU hashes.

## Candidate residency moves (not decided)

| Id | Move | Value | Risk |
|---|---|---|---|
| R1 | Lighting State Block: one small GPU buffer for sun, sky SH, cloud summary, albedo, stamps | One source of truth, fewer constants | Low |
| R2 | Horizon maps: store 16 horizon slopes per clipmap sample; serve sun shadow and ambient | Replaces the ~40-step per-pixel march and the coarse field; follows clipmap resolution | Medium |
| R3 | Sky-view LUT built on the GPU | Removes the upload and the CPU texels; cheap per-probe sky views | Medium (SH currently derived from CPU texels) |
| R4 | One GPU cloud column map from all sources (field, storm domains, cloud lab) | One authority for ambient, shadow penumbra and orbital albedo | Medium |
| R5 | Procedural `CloudField` generated on the GPU, upload kept for authored and imported | No large uploads, time-continuous weather | Medium (CPU virtual source interface) |
| R6 | Radiance cell estimator as a compute pass | Thousands of cells per frame instead of 64 | High (needs proxy and body occlusion on the GPU) |
| R7 | Cells store geometry only; the sky is applied at shading | CPU no longer needs the sky summary for baking or fingerprints | Low |

## Open questions

- Is directional (anisotropic) ambient a priority, or is openness plus a lean direction enough?
- Which consumers come first: ground, clouds, orbit?
- Is the G-buffer or lighting-pass layout allowed to change if it removes transfers?
