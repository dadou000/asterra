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
  "engine/celestial_clouds/include/orbit/celestial_clouds/CloudField.hpp",
  "engine/celestial_clouds/include/orbit/celestial_clouds/CloudRenderer.hpp",
  "engine/celestial_clouds/include/orbit/celestial_clouds/WeatherModel.hpp",
  "engine/celestial_clouds/src/CloudRenderer.cpp",
  "engine/studio_ui/include/orbit/studio_ui/StudioTerrainLayerOptions.hpp",
  "engine/celestial_clouds/tests/CloudFieldTests.cpp",
]
symbols = ["CloudFieldProduct", "GpuCloudFieldProduct", "CloudLayerField", "CloudTexel", "WeatherClimate", "CloudRenderer"]
invariants = [
  "The cloud field is built once per body on the GPU and shared: the orbital globe composite and the ray-marched shell read the same field, so both views agree on where the weather is.",
  "CloudRenderer draws only when the view uses the clipmap (full_clipmap) or has no macro globe; clouds are never composited twice.",
  "Turning the view's `clouds` flag off stops the ray march and the cloud shadow; the cloud field itself is still built and used for shadows and orbital globes.",
  "Passes are added in this order per view: Atmosphere, then Clouds -> CloudsResolve (temporal) -> CloudsComposite; CloudShadow runs earlier, before SharedDirectLighting. Clouds are composited AFTER the atmosphere pass, so the cloud shader applies its own aerial perspective over the camera-to-cloud distance (the atmosphere pass only knows the terrain/sky behind a cloud).",
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
verified = "b0a0de7f"

[routes]
"how the near-field cloud march works, why it is blurry/striped/slow" = "raymarch"
"cloud shadows on the ground, god rays, cloud-on-cloud shadow" = "shadows-and-light-volume"
"inspect one isolated cloud of a chosen type" = "cloud-lab"
"weather model, orbital weather representation" = "/legacy/research-v006-cloud-weather"
"universal volumetrics (clouds, fog, generic media)" = "/legacy/research-v007-universal-volumetrics"
+++

## Module map (`engine/celestial_clouds`)

| File | Role |
| --- | --- |
| `WeatherModel.hpp` | weather parameters and climate that bias the cloud type and coverage |
| `CloudField.hpp` | CPU `CloudFieldProduct` and GPU `GpuCloudFieldProduct`: per-texel `cloudType`, coverage, optical depth |
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
