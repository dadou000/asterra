+++
path = "/rendering/clouds"
title = "Clouds"
kind = "subsystem"
status = "stable"
owner_module = "OrbitCelestialClouds"
summary = """
One GPU cloud field per body (weather model -> CloudField texels with a cloudType axis) feeds three consumers: the orbital \
macro-globe composite, a per-pixel ray-marched cloud shell for clipmap/near-field views (CloudRenderer), and the cloud shadow \
texture read by direct lighting. Near-field clouds and shadows share the per-view `clouds` toggle."""
keywords = ["clouds", "cloud field", "weather", "ray march", "cloud shadow", "cumulonimbus", "stratus", "cloud type", "volumetric", "orbital weather"]
sources = [
  "engine/celestial_clouds/tests/WeatherTimewarpTests.cpp",
  "engine/celestial_clouds/include/orbit/celestial_clouds/CloudField.hpp",
  "engine/celestial_clouds/include/orbit/celestial_clouds/CloudRenderer.hpp",
  "engine/celestial_clouds/include/orbit/celestial_clouds/WeatherModel.hpp",
  "engine/celestial_clouds/src/CloudRenderer.cpp",
  "engine/studio_ui/include/orbit/studio_ui/StudioTerrainLayerOptions.hpp",
  "engine/celestial_clouds/tests/CloudFieldTests.cpp",
]
symbols = ["CloudFieldProduct", "GpuCloudFieldProduct", "CloudLayerField", "CloudTexel", "WeatherClimate", "CloudRenderer"]
invariants = [
  "StormSchedule (ScheduleStorm, StormsNear) is a pure function of the absolute time, the seed and the weather parameters: no camera, no history, so launches and viewpoints agree on every storm and a timewarp needs no stepping.",
  "EvaluateWeather is a pure function of (direction, absolute seconds, parameters, climate): valid and finite at any epoch tested up to 1e10 s (about 317 years), identical on repeat, as smooth in time at t = 1e10 s as at t = 0 (mean coverage change 1.37e-5 per second at both) and with no drift of global cover (0.367-0.387 across epochs) or loss of circulation structure; timewarp therefore needs no stepping through skipped time (pinned by WeatherTimewarpTests).",
  "Pace of the weather map is fast compared with nature, by authoring: everything drifts through a noise dimension at evolve = 1.5e-5 per second (the pattern is 0.96 correlated after one hour and about 0.32 after a day, a floor set by the fixed latitude bands) and the baroclinic wave phase advances at 6 x evolve, i.e. waves travel ~60 m/s at 52 degrees latitude where real synoptic waves move 10-15 m/s. Fine at 1x; at high timewarp clouds morph rather than drift for days. Slowing the rates ~5-8x is a one-line change but changes the look of every session, so it is left as an art decision.",
  "The cloud field is built once per body on the GPU and shared: the orbital globe composite and the ray-marched shell read the same field, so both views agree on where the weather is.",
  "System A CloudRenderer keeps the legacy clipmap-or-no-macro-globe near-field gate; independent System B is also permitted in macro-globe orbital view for direct B volumetric comparisons. Their output targets and histories are separate.",
  "Turning the view's `clouds` flag off stops the ray march and the cloud shadow; the cloud field itself is still built and used for shadows and orbital globes.",
  "Passes are added in this order per view: Atmosphere, then Clouds -> CloudsResolve (temporal) -> CloudsComposite; CloudShadow runs earlier, before SharedDirectLighting. Clouds are composited AFTER the atmosphere pass, so the cloud shader applies its own aerial perspective over the camera-to-cloud distance (the atmosphere pass only knows the terrain/sky behind a cloud).",
  "A simulated storm domain (storm.place) replaces the procedural density inside its domain in the march, the cloud shadow and the sun light volume alike, because all three call Extinction(); the buffer is bound at slot 3 of every cloud pipeline and CLOUD_STORM=0 removes the lookup (raymarch).",
  "Cloud type is a continuous axis in each texel: 0 stratus, 0.5 cumulus, 1 cumulonimbus (lab labels: 0.05 stratus, 0.2 stratocumulus, 0.32 nimbostratus, 0.5 cumulus, 0.72 congestus, 1.0 cumulonimbus).",
]
related = ["/rendering/lighting", "/rendering/terrain/clipmaps"]
depends_on = ["/rendering"]
used_by = ["/rendering/lighting"]
verify = [
  "ctest -R Orbit.CelestialClouds.",
  "orbit_view_text_diagnostics: the `clouds` object (layer count, mean coverage and optical depth, time bucket, GPU residency) is absent when the body has no cloud layer.",
  "Clouds-on vs clouds-off captures at 0.8, 3, 8, 60 and 2,000 km differ only by clouds and their shadow.",
]
verified = "69bafa8"

[routes]
"how the near-field cloud march works, why it is blurry/striped/slow" = "raymarch"
"independent unified cloud System B architecture and A/B isolation" = "unified-system-b"
"unified B2 layered cloud extinction, nested storm integration and tests" = "unified-system-b-density"
"unified B3 independent sunlight optical depth cache" = "unified-system-b-sun-cache"
"system B multiscale 3D cloud morphology and visual parity tests" = "unified-system-b-morphology"
"frozen EVE V5 reference full system integration specification" = "reference-eve-v5"
"EVE V5 reference source and preset inventory" = "reference-eve-v5-01-package-and-presets"
"EVE V5 config parser asset paths and cloud authoring" = "reference-eve-v5-02-config-and-assets"
"EVE V5 cloud medium geometry and raymarch" = "reference-eve-v5-03-medium-and-geometry"
"EVE V5 atmosphere skylight shadow volume and godrays" = "reference-eve-v5-04-atmosphere-and-lighting"
"EVE V5 terrain ocean wetness and cloud shadow integration" = "reference-eve-v5-05-terrain-and-water"
"EVE V5 lightning rain particles droplets and sound" = "reference-eve-v5-06-events-and-fx"
"EVE V5 Vulkan render graph data ownership and temporal" = "reference-eve-v5-07-render-graph"
"EVE V5 validation golden captures and performance gates" = "reference-eve-v5-08-validation-and-migration"
"EVE V5 exact implementation task ledger" = "reference-eve-v5-09-implementation-ledger"
"blackrack EVE release 5 archive and rendering techniques" = "blackrack-eve-release5-reference"
"blackrack-inspired optimization plan and code mapping for Orbit" = "blackrack-eve-orbit-design"
"papers, open-source examples and volumetric clouds resource library" = "cloud-rendering-reference-library"
"cloud shadows on the ground, god rays, cloud-on-cloud shadow" = "shadows-and-light-volume"
"inspect one isolated cloud of a chosen type" = "cloud-lab"
"weather model, orbital weather representation" = "/legacy/tree-history-research-v006-cloud-weather"
"universal volumetrics (clouds, fog, generic media)" = "/legacy/tree-history-research-v007-universal-volumetrics"
+++

## Unified rendering System B (experimental)

- [System B foundation, independence and integration contracts](unified-system-b.md): B0 contracts, B1 Vulkan rendering, B2 multilayer/nested storms, B3 time-sliced Sun cache and a later morphology upgrade (3D noise + progressive raymarch) implemented. New morphology is not GPU/visual-validated. A is still the default; B remains an opt-in preview. Far-field sunlight cascades, sky ambient cache, temporal reconstruction and Cloud Lab parity are outstanding. The old A shader split is NOT System B.

- [B2 unified density and storm data contract](unified-system-b-density.md): GPU weather metadata append-only ABI, multi-layer extinction, nested storm interpolation, conflict register, and local validation.
- [B3 independent sunlight cache](unified-system-b-sun-cache.md): physical optical depth, 16-frame sliced GPU updates, invalidation, safe cache-miss fallback, profiler and A/B testing.
- [B morphology quality upgrade](unified-system-b-morphology.md): 3D tileable cell noise, cloud-type profiles, footprint-based erosion, independent resource lifetime and explicit A-parity gate.

## Frozen V5 reference port: complete technical specification

This is a **documentation-first** design for a separate EveV5Reference renderer, NOT a completed port. The supplied August 2026 preview ZIP is a proprietary compiled KSP release: source shaders and **103 cloud texture/sound paths (76 images, 27 audio)** are missing from it. No private binaries or assets may be copied into this public repository. A faithful line-by-line Vulkan port cannot be claimed until the original source/permissions and visual reference assets are available. Preserve existing A and B.

- [00 Reference architecture and evidence boundaries](reference-eve-v5.md)
- [01 Full package/config/preset inventory](reference-eve-v5-01-package-and-presets.md)
- [02 Typed config importer, Hermite cloud types and asset validation](reference-eve-v5-02-config-and-assets.md)
- [03 Cloud medium, 3D noise, grazing geometry, LOD, raymarch code](reference-eve-v5-03-medium-and-geometry.md)
- [04 Atmosphere, physical Sun/sky light, multiple scattering, shadows and godrays](reference-eve-v5-04-atmosphere-and-lighting.md)
- [05 Terrain, ocean, glass, rain wetness and puddle contracts](reference-eve-v5-05-terrain-and-water.md)
- [06 Storm lightning, precipitation, droplets, particle fields and sound-domain events](reference-eve-v5-06-events-and-fx.md)
- [07 Vulkan render graph, GPU ABI, barriers, snapshots, multi-view and profiler](reference-eve-v5-07-render-graph.md)
- [08 26-scene validation matrix, image/GPU error metrics and gates](reference-eve-v5-08-validation-and-migration.md)
- [09 52-item implementation ledger with exact Orbit touch points](reference-eve-v5-09-implementation-ledger.md)

Use the original self-contained private ZIP audit: `python tools/audit_eve_reference.py "path/to/release.zip" --strict-manifest`. The audit never extracts the ZIP or uploads proprietary payloads.

## Reference research (Release 5 / planetary volumetrics)

- [Inspected EVE Release 5 archive and technique inventory](blackrack-eve-release5-reference.md): evidence-ranked configuration, build limitations, cloud types, light volume, temporal upscaling, layer order, SDF and Scatterer coupling. Proprietary archive and binaries are not stored in this repo.
- [Orbit-specific code and performance design](blackrack-eve-orbit-design.md): function-level integration plan, A/B shader toggles, cache structure, correctness constraints and staged implementation.
- [Cloud rendering source library](cloud-rendering-reference-library.md): upstream blackrack documentation, Nubis/Nubis3, Unreal, atmospheric papers, source projects and caveats.

## Module map (`engine/celestial_clouds`)

| File | Role |
| --- | --- |
| `WeatherModel.hpp` | weather parameters and climate that bias the cloud type and coverage |
| `CloudField.hpp` | CPU `CloudFieldProduct` and GPU `GpuCloudFieldProduct`: per-texel `cloudType`, coverage, optical depth |
| `StormSchedule.hpp/.cpp` | where and when simulated storms exist: a pure function of the absolute time (fixed lat/lon tiles x six hour slots, hashed, weighted by the weather map's convective score); see `/world/weather-lab/global-system-status-p6` |
| `CloudRenderer.hpp/.cpp` | per-pixel ray-marched shell, light volume, temporal resolve, cloud lab; shaders are embedded strings |

## Per-view controls (`view.terrain_layers_set`)

`clouds` (default on), `cloud_resolution_scale` (default 0.5, range 0.25-1), `cloud_temporal` (default on),
`cloud_godray_strength` (default 1, range 0-2), `cloud_light_volume` (default on), `cloud_volume_debug_altitude`,
`bypass_cloud_shadow`, `cloud_lab` (object). Same controls as the viewport Diagnostics "Terrain layers" section.
Full parameter descriptions: `/legacy/orbit-mcp/panels-and-the-shading-tab`.

## Diagnosing

| Symptom | First step |
| --- | --- |
| no clouds in a near-field or clipmap view | `view.terrain_layers_get`: `clouds` true? `view.text_diagnostics`: is there a `clouds` object (a body with no cloud layer has none)? |
| clouds look doubled | the march must run only with `full_clipmap` or without a macro globe; check which of the two paths is active |
| cloud cost too high | `cloud_resolution_scale` (1 = crisp, about four times the cost of 0.5) |
| noisy, jittery clouds | `cloud_temporal` off shows the raw single-frame march; if that is the noisy one, the resolve is working |
