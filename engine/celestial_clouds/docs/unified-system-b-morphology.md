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

# What this DOES NOT claim

No Windows Vulkan runtime, HLSL compiler or matched A/B screenshots were available for this change. The new morphology has not been visually validated. It may still have banding or brightness discontinuities on some orbital grazing angles. No GPU performance improvement is asserted from adding 3D noise. The shader is more expensive than B's former sine variant; correctness/performance checks are mandatory.

This does not yet equal A's capabilities. A currently has: higher raymarch budget and adaptive density steps, richer billows/tower details and glaciation, cloud-base wall features, precipitation shafts, mammatus, richer scattering and atmospheric LUT coupling, plus temporal accumulation. B continues with its independent B3 Sun cache, simplified ambient, fixed 64-step primary raymarch and no B temporal reprojection. All of those must be measured and addressed **before claiming B >= A**. Do not copy A's shader, use its mutable light cache or merely turn on A internally when B is selected.

# Required comparisons and gates

1. Build and validate B shaders: `python tools/check_unified_cloud_shaders.py`.
2. Build `OrbitStudio` and `OrbitUnifiedCloudFoundationTests`, run `ctest -C Release -R Orbit.UnifiedCloudFoundation --output-on-failure`.
3. Capture A-only and B-only separately at identical camera/time/body/exposure with source fingerprint and shader/build commit in provenance. Disable B's Cloud Lab unsupported input and avoid confusing the old A-vs-A shader split with System B.
4. At orbit, compare: individual cloud cell shapes, coverage gaps, detached anvils/cirrus, silhouette coherence, visual repetition, transmittance against reference and terminator-edge stability.
5. At 6 km / 2 km / 300 m, compare billows, fine erosion, volumetric depth and speed/density transition. Move the camera slowly to detect static screen-aligned repeat patterns.
6. Compare midday, low Sun and planet night with Sun cache enabled/disabled and receiver shadows toggled. Cache changes should not alter the 3D density itself.
7. In Profiler, capture `.UnifiedB.March`, `.UnifiedB.SunCache`, `.UnifiedB.ReceiverShadow`, total GPU frame mean/p95 and GPU memory. The previous 20.56 ms A Clouds-pass benchmark was one specific scene, not a target guaranteed under every weather condition.
8. Enforce a measurable quality gate before parity: no obvious missing cloud layers, no repeated geometric ridge texture, comparable perceptual detail at matched scale, no unaccounted cloud-lighting regression, and a documented acceptable GPU cost. If the current fixed 64-step B march limits quality, increase/adapt it *with GPU profiling*, not by arbitrarily copying A's 320-step cap.

# Next steps

B's next quality stages should focus on conservative adaptive view marching and ray-budget exhaustion diagnostics, B-owned temporal reprojection at reduced internal resolution, directional ambient sky radiance derived from the shared atmospheric LUTs, and fine storm/rain/anvil features with temporal filtering. Keep caches, scene provenance, and compiler ownership independent; share only immutable world, weather and physically authoritative illumination inputs.
