+++
path = "/rendering/clouds/unified-system-b-morphology"
title = "System B cloud morphology: multiscale 3D extinction"
kind = "concept"
status = "experimental"
owner_module = "OrbitCelestialClouds"
summary = "B-owned tileable Worley-cellular noise and footprint-filtered typed cloud density, with explicit A parity gates and performance constraints."
keywords = ["System B", "cloud morphology", "Perlin Worley", "Worley", "cellular noise", "erosion", "footprint", "orbital clouds", "multiscale", "independent A/B"]
sources = [
  "engine/celestial_clouds/include/orbit/celestial_clouds/UnifiedCloudNoise.hpp",
  "engine/celestial_clouds/src/UnifiedCloudNoise.cpp",
  "engine/celestial_clouds/src/UnifiedCloudRenderer.cpp",
  "engine/celestial_clouds/tests/UnifiedCloudFoundationTests.cpp",
  "engine/celestial_clouds/src/CloudRenderer.cpp",
  "tools/check_unified_cloud_shaders.py"
]
related = [
  "/rendering/clouds/unified-system-b",
  "/rendering/clouds/unified-system-b-density",
  "/rendering/clouds/unified-system-b-sun-cache",
  "/rendering/clouds/references/blackrack-eve-orbit-design"
]
depends_on = ["/rendering/clouds/unified-system-b-density"]
[routes]
"why B clouds looked like a uniform repeating blanket" = "/rendering/clouds/unified-system-b-morphology"
"how B cloud shape uses footprint filtering" = "/rendering/clouds/unified-system-b-morphology"
"how to benchmark cloud rendering B against A" = "/rendering/clouds/unified-system-b-morphology"
+++

# Problem and evidence

User screenshots at orbit showed almost solid pale-grey cloud coverage, with a fine repeated corrugated pattern. B2/B3's original visible density had only a single sinusoidal modifier:
`0.75 + 0.25*sin(p.x*0.0013 + 1.7*sin(p.y*0.00083) + p.z*0.0011 + layer*1.72)`.
This is deterministic and cheap but cannot produce believable separate cloud cells, eroded cauliflower silhouettes or stable multiscale material. The repeating small-scale pattern was **not** a jagged shadow artifact. A's current shader has many more shaping features and much higher maximum march-step budget. This phase targets B's density representation; it is not proof of full quality parity with A.

# What is implemented

- `BuildUnifiedCloudNoise()` bakes a deterministic, tileable 32x32x32 RGBA float field **owned by B**. The channels are broad shape (mixture of cellular F1), 4-cell, 8-cell and 16-cell inverse Worley distance. The actual cell features use per-cell deterministic hash offsets and wrap in all three axes. Channel values normalize to [0,1] individually, avoiding hard-coded assumptions about the random seed's amplitude. Memory is 32^3 * 4 * sizeof(float) = 524,288 bytes (512 KiB).
- The 512 KiB B noise buffer is allocated/uploaded once by `UnifiedCloudRenderer`, not by weather, Sun cache or viewports. The weather product and storm export remain authoritative shared *immutable input data*. A's own noise buffer, compiled shaders, shadow pass, light volumes and render targets remain unchanged.
- B's shader declares a fourth read-only ByteAddressBuffer binding. Graphics March, ReceiverShadow and the two compute Sun-cache pipelines get that independent buffer. The depth sampler moves from binding 3 to 4 to preserve Vulkan's explicit binding layout. CPU shader-resource-buffer counts change from 3 to 4 for B-only pipelines.
- The shader samples tileable trilinear 3D noise with smoothed interpolation. Two differently rotated/offset and incommensurately scaled coordinates avoid the easily visible uniform sinusoidal ridge structure. Large noise shapes are roughly 15–40 km, intermediate billows roughly 3–12 km and fine erosion roughly 0.3–1 km. These approximate features are not fixed to one voxel/grid point in world space.
- Cloud weather coverage **controls an iso-surface threshold** in 3D shape space rather than merely multiplying a continuous vertical density profile. Dense cores become opaque while gaps between individual cloud masses allow the underlying planet/atmosphere to show through.
- Vertical profiles now use separate flat-base/shallow stratus, developing cumulus, and tall/rounded convective towers, with their top fractions varying by coarse noise. Cloud-top narrowing increases for updraft towers. High cirrus/anvil contributions occupy a different, thinner upper region.
- Erosion is active only for camera-visible, sufficiently resolved small features; the B secondary Sun/cache/shadow integrators evaluate cheaper broad structures. Avoid evaluating noisy fine geometry at a footprint larger than the feature. Secondary transport will not exactly match finest silhouette features until a higher-resolution lighting correction is designed.
- Projected world-space footprint is computed using camera-ray distance, vertical half field of view and the output target height, without changing A's UI settings. Fine octaves fade as projected footprint grows; coarse structure remains visible across orbit views.
- Empty single-layer weather columns are rejected before expensive noise-buffer lookups, including in Sun-cache generation. B keeps the live nested-storm replacement semantics and all authored layer descriptors from B2.
- The view march has been upgraded from 64 uniform samples to 128 **nonuniform but complete** samples. Its camera-to-cloud partition is a monotonic quadratic schedule: t(u)=tNear+span*u*(0.35+0.65*u). This preferentially samples near the viewer without leaving a long grazing ray partially unrendered at budget exhaustion. A stable per-pixel PCG offset stratifies each sample inside its segment, with no temporal phase until B has reliable history.

# Matched-scenario A/B review from user captures (2026-10-14)

The user supplied two near-horizontal cloud-views: the first explicitly displays **Cloud renderer: A (reference)**, and the second is B. They share approximately similar horizon orientation and reported cloud-field statistics, but are not accompanied by identical physical timestamps, camera transform, exposure, or GPU trace. Therefore this is a qualitative visual comparison, **not** a controlled parity or speed benchmark.

Observed:
- A has more distinct broken cumulus silhouettes, many separate white billows, taller apparent masses and stronger sun-facing volume.
- B appears as a broader, lower-contrast grey cloud bank, with very dark bases and weaker separation into individual structures; its morphology is still insufficient even after the first 32-cubed noise upgrade.
- A has visibly clipped or nearly clipped white highlights. B should not reproduce that clipping as a target; it should reproduce the readable shape and illumination without sacrificing energy consistency.
- The screenshot does not distinguish whether missing B towers come from coverage mapping, type/vertical profile, under-sampled ray segments, perspective/framing differences, or over-dark lighting. It must not be cited as proof of a specific shader bug.

Immediate source-level lighting finding: System A has a broad multiple-scattering closure, several phase lobes and an atmosphere-aware Sun term. B previously used only a normalized HG single lobe plus weak approximate daylight sky access. It therefore made cloud faces and interiors much darker even when the same optical-depth input was valid.

An independent B-only multiple-scattering closure has now been introduced in `UnifiedCloudRenderer.cpp`: a bounded directional single term, two broad HG lobes weighted into diffuse multi-order scattering, a slower optical-depth fade for scattered light, and a stronger daylight-gated sky-access term. This does **not** copy A's shader implementation, consume A's Sun-cache buffers, or replace its physically shared weather/storm inputs. Phase lobes are normalized; the broad lobes use reciprocal square roots instead of general pow calls in the per-sample shader. B still lacks authoritative atmosphere LUT sky-radiance and true multiple-scattering transport, so this is an intermediate visual correction, not a physically calibrated sky solver.

**Required evaluation after rebuilding:** capture the same body/time/weather, camera, Sun, resolution and eye-exposure with A then B. Check B daylight (should be noticeably brighter and less charcoal-grey); cloud bases (should remain darker than tops); night side (should not acquire constant glow); Sun-cache ON/OFF (should produce broadly consistent lit shapes after cache warmup); and whether flat silhouettes remain despite improved brightness. Use B receiver-shadow OFF as a separate ground-shadow diagnostic. Obtain profiler median/p95; B's independently baked 3D noise, 128 view samples and broad-light evaluation may exceed A's GPU budget.

**Next morphology work after this correction:** profile/mask matching to authoritative cloud types, noise-scale/coverage clustering, physically coherent vertical top growth, and selective adaptive fine stepping. Do not tweak illumination to disguise missing cloud volumes. No Vulkan run has been performed by the assistant for this update.

# What this DOES NOT claim

No Windows Vulkan runtime, HLSL compiler or matched A/B screenshots were available for this change. The new morphology has not been visually validated. It may still have banding or brightness discontinuities on some orbital grazing angles. No GPU performance improvement is asserted from adding 3D noise. The shader and larger raymarch budget are more expensive than B's former sine/64-step variant; correctness/performance checks are mandatory.

This does not yet equal A's capabilities. A currently has: higher raymarch budget and adaptive density steps, richer billows/tower details and glaciation, cloud-base wall features, precipitation shafts, mammatus, richer scattering and atmospheric LUT coupling, plus temporal accumulation. B continues with its independent B3 Sun cache, simplified ambient, a fixed-budget 128-step progressive primary raymarch (not yet A's adaptive 320-step density refinement) and no B temporal reprojection. All of those must be measured and addressed **before claiming B >= A**. Do not copy A's shader, use its mutable light cache or merely turn on A internally when B is selected.

# Required comparisons and gates

1. Build and validate B shaders: `python tools/check_unified_cloud_shaders.py`.
2. Build `OrbitStudio` and `OrbitUnifiedCloudFoundationTests`, run `ctest -C Release -R Orbit.UnifiedCloudFoundation --output-on-failure`.
3. Capture A-only and B-only separately at identical camera/time/body/exposure with source fingerprint and shader/build commit in provenance. Disable B's Cloud Lab unsupported input and avoid confusing the old A-vs-A shader split with System B.
4. At orbit, compare: individual cloud cell shapes, coverage gaps, detached anvils/cirrus, silhouette coherence, visual repetition, transmittance against reference and terminator-edge stability.
5. At 6 km / 2 km / 300 m, compare billows, fine erosion, volumetric depth and speed/density transition. Move the camera slowly to detect static screen-aligned repeat patterns.
6. Compare midday, low Sun and planet night with Sun cache enabled/disabled and receiver shadows toggled. Cache changes should not alter the 3D density itself.
7. In Profiler, capture `.UnifiedB.March`, `.UnifiedB.SunCache`, `.UnifiedB.ReceiverShadow`, total GPU frame mean/p95 and GPU memory. The previous 20.56 ms A Clouds-pass benchmark was one specific scene, not a target guaranteed under every weather condition.
8. Enforce a measurable quality gate before parity: no obvious missing cloud layers, no repeated geometric ridge texture, comparable perceptual detail at matched scale, no unaccounted cloud-lighting regression, and a documented acceptable GPU cost. The progressive 128-step B march must be profiled against A's 320-step adaptive march; increase/adapt further only when a quality error and GPU budget justify it.

# Next steps

B's next quality stages should focus on conservative adaptive view marching and ray-budget exhaustion diagnostics, B-owned temporal reprojection at reduced internal resolution, directional ambient sky radiance derived from the shared atmospheric LUTs, and fine storm/rain/anvil features with temporal filtering. Keep caches, scene provenance, and compiler ownership independent; share only immutable world, weather and physically authoritative illumination inputs.
