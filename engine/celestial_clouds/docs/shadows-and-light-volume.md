+++
path = "/rendering/clouds/shadows-and-light-volume"
title = "Cloud shadows, light volume and god rays"
kind = "concept"
status = "stable"
owner_module = "OrbitCelestialClouds"
summary = """
Cloud shadows: a half-resolution CloudShadow pass reconstructs each pixel's surface position from depth, marches 16 steps \
along the sun ray through the cloud shell (up to 60 km) and writes the sun transmittance, which direct lighting multiplies \
into the stellar term. A camera-centred light volume (three toroidal cascades) caches optical depth toward the sun so clouds \
shadow other clouds and god rays cost one lookup per step."""
keywords = ["cloud shadow", "light volume", "god rays", "crepuscular", "cascade", "optical depth", "sun transmittance", "bypass_cloud_shadow", "cloud_light_volume", "cloud_godray_strength", "shadow on clouds"]
sources = [
  "engine/celestial_clouds/src/CloudRenderer.cpp",
  "engine/lighting/include/orbit/lighting/DirectLighting.hpp",
  "engine/studio_ui/include/orbit/studio_ui/StudioTerrainLayerOptions.hpp",
  "engine/studio_ui/src/StudioViewportRendererBase.cpp",
]
symbols = ["kShadowSteps", "kVolCascades", "DirectLightingRenderer", "cloudShadow", "cloudLightVolume", "cloudGodrayStrength", "cloudVolumeDebugAltitude"]
invariants = [
  "The shadow texture is produced by <view>.CloudShadow (half resolution) before <view>.SharedDirectLighting and is multiplied into the stellar term only; the sky ambient is untouched, so ground under overcast switches from sun-lit to sky-lit.",
  "DirectLightingRenderer::Draw receives it through the `cloudShadow` argument (binding 8, flag in extra.x); the shadow march uses the same density as the camera march (base shape plus half-strength erosion) so shadows follow the visible clouds and move with the wind.",
  "The shadow march takes kShadowSteps (16) steps through the shell up to 60 km along the sun ray.",
  "The shadow shares the per-view `clouds` toggle; bypass_cloud_shadow skips just the shadow read in direct lighting.",
  "The light volume has three toroidal cascades (kVolCascades = 3) of 250 m, 2 km and 8 km cells, usable out to about 11, 92 and 368 km from the camera, refreshed a few voxels per frame; voxels are tagged with their lattice index and a generation so stale voxels read as 'not ready' rather than wrong.",
  "cloud_light_volume (default on) lets cloud lighting see shadows from other clouds beyond the short in-march sun steps; off marches everything per sample (for comparison only).",
  "God rays (cloud_godray_strength, default 1, range 0-2) remove the direct in-scatter of air that sits in cloud shadow from what the atmosphere pass added; 0 turns them off and skips the extra march.",
  "cloud_volume_debug_altitude (metres, 0-40000, 0 = off, -1 = scene depth as log view-space distance) draws a horizontal slice of the volume as a heatmap of optical depth toward the sun; magenta = voxel not ready; nothing outside the three cascades.",
]
related = ["/rendering/clouds/raymarch", "/rendering/lighting"]
depends_on = ["/rendering/clouds", "/rendering/lighting"]
verify = [
  "Straight down at 0.8 km, clouds on vs off differ in about 3.5 % of pixels by more than 24/255; on vs on differs by 0.0 % (deterministic).",
  "cloud_volume_debug_altitude at a known cloud altitude shows heat inside the cascades and nothing outside.",
]
verified = "b0a0de7f"

[[diagnose]]
symptom = "no cloud shadows on the ground, or shadows that do not match the visible clouds"
steps = [
  "orbit_view_terrain_layers_get: clouds must be true and bypass_cloud_shadow false (the shadow shares the clouds toggle).",
  "The shadow is built from the cloud field, not from what was drawn: confirm view.text_diagnostics has a clouds object, and compare shadow positions with the march at the same time bucket.",
  "Toggle bypass_cloud_shadow: the stellar term must change only where the shadow falls; the sky ambient must not change.",
]
docs = ["/rendering/clouds"]

[[diagnose]]
symptom = "clouds do not shadow other clouds, or god rays are missing"
steps = [
  "orbit_view_terrain_layers_get: cloud_light_volume must be true and cloud_godray_strength above 0.",
  "Set cloud_volume_debug_altitude to the cloud altitude: if the heatmap is empty or magenta there, the volume has not filled (it refreshes a few voxels per frame) or the camera is outside every cascade.",
  "Beyond the short in-march sun steps (about 6.7 km) only the light volume supplies cloud-on-cloud shadow, so a missing volume looks like unshadowed distant clouds.",
]
docs = ["/rendering/clouds/raymarch"]
+++

## Where it is wired

`StudioViewportRendererBase.cpp` builds the passes (`prefix + ".CloudShadow"` then `".SharedDirectLighting"`); the shadow
texture is handed to `DirectLightingRenderer::Draw` (`engine/lighting`). The light volume and the god-ray option live in
`CloudRenderer.cpp` and are controlled through `StudioTerrainLayerOptions` (`cloudLightVolume`, `cloudGodrayStrength`,
`cloudVolumeDebugAltitude`).
