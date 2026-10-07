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
  "engine/celestial_clouds/include/orbit/celestial_clouds/CloudRenderer.hpp",
  "tests/CloudLightVolumeTests.cpp",
  "engine/lighting/include/orbit/lighting/DirectLighting.hpp",
  "engine/studio_ui/include/orbit/studio_ui/StudioTerrainLayerOptions.hpp",
  "engine/studio_ui/src/StudioViewportRenderer.cpp",
]
symbols = ["kShadowSteps", "kVolCascades", "DirectLightingRenderer", "cloudShadow", "cloudLightVolume", "cloudGodrayStrength", "cloudVolumeDebugAltitude"]
invariants = [
  "The shadow texture is produced by <view>.CloudShadow (half resolution) before <view>.SharedDirectLighting and is multiplied into the stellar term only; the sky ambient is untouched, so ground under overcast switches from sun-lit to sky-lit.",
  "DirectLightingRenderer::Draw receives it through the `cloudShadow` argument (binding 8, flag in extra.x); the shadow march uses the same density as the camera march (base shape plus half-strength erosion) so shadows follow the visible clouds and move with the wind.",
  "The shadow march takes kShadowSteps (16) steps through the shell up to 60 km along the sun ray.",
  "The shadow shares the per-view `clouds` toggle; bypass_cloud_shadow skips just the shadow read in direct lighting.",
  "The light volume has three toroidal cascades (kVolCascades = 3) of 250 m, 2 km and 8 km cells, usable out to about 11, 92 and 368 km from the camera, refreshed a few voxels per frame; voxels are tagged with their lattice index and a generation so stale voxels read as 'not ready' rather than wrong.",
  "The light volume update is its own graph pass (<view>.CloudLightVolume), ordered before <view>.Clouds by the shared cloud target; it was part of <view>.Clouds, which made the ray march look about four times as expensive as it is (about 0.6 ms against 1.7 ms for the refresh in the Sponza Atrium).",
  "The voxels are a pure function of: camera position, sun direction, shell and layer parameters, the cloud lab, the voxel generation and anchor, and the cloud field. When none of those has changed for kLightVolumeSettleFrames (24) consecutive frames, every voxel has been refreshed at least once with the current inputs (a valid voxel every 6th frame, 12th for the far cascade, a missing one every 3rd), so UpdateLightVolume skips the dispatch: the result is bit-identical to refreshing. Any change restarts the count and the original schedule runs until it settles again; a moving camera or a running simulation (the sun moves every frame) therefore never skips.",
  "The refresh schedule uses frameIndex % 4096, and 4096 is not a multiple of 12, so a window that straddles the wrap sees the refresh residues in two runs. Two full cycles (24) guarantee one run covers every residue; a single cycle (12) is NOT enough and fails Orbit.CloudLightVolume's wrap case.",
  "The skip fingerprint hashes exactly the Constants dwords the volume shader's main() can reach (camera xyz, sun xyz, shell, optics.w, atmosphere.z, lod.yzw, lab, labParams, labLife, temporal.z/w, anchor) plus the field fingerprint and the volume buffer. If the shader starts reading another constant, add it to LightVolumeInputFingerprint or the skip becomes wrong.",
  "cloud_light_volume (default on) lets cloud lighting see shadows from other clouds beyond the short in-march sun steps; off marches everything per sample (for comparison only).",
  "God rays (cloud_godray_strength, default 1, range 0-2) remove the direct in-scatter of air that sits in cloud shadow from what the atmosphere pass added; 0 turns them off and skips the extra march.",
  "cloud_volume_debug_altitude (metres, 0-40000, 0 = off, -1 = scene depth as log view-space distance) draws a horizontal slice of the volume as a heatmap of optical depth toward the sun; magenta = voxel not ready; nothing outside the three cascades.",
]
related = ["/rendering/clouds/raymarch", "/rendering/lighting"]
depends_on = ["/rendering/clouds", "/rendering/lighting"]
verify = [
  "ctest -R Orbit.CloudLightVolume: runs a refresh-every-frame volume and a skipping volume in lock-step and requires every byte to match after static, sun-moved, camera-moved, field-changed and frame-wrap phases. It fails with a 3-frame or 12-frame settle window, so it does catch an unsafe skip.",
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
symptom = "cloud-on-cloud shadows or god rays look stale after changing something, or only update when the camera moves"
steps = [
  "Set ORBIT_CLOUD_VOLUME_ALWAYS_REFRESH=1 before launching Studio: this turns the settled-input skip off and restores the original refresh-every-frame schedule. If the stale look disappears, an input the skip does not hash is changing, so extend LightVolumeInputFingerprint (and check which Constants members the volume shader reads).",
  "Read <view>.CloudLightVolume and <view>.Clouds separately in profiler.snapshot top_gpu_passes: about 1.7 ms for CloudLightVolume with a moving camera or sun, near zero once settled.",
  "The GPU pass timer attributes time between consecutive pass-end timestamps, so a skipped (nearly empty) pass can still show some time that really belongs to overlapped neighbouring work; judge the change by whole-frame time, not by that one number.",
]
docs = ["/rendering/clouds/raymarch"]

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

`StudioViewportRenderer.cpp` builds the passes (`prefix + ".CloudShadow"` then `".SharedDirectLighting"`); the shadow
texture is handed to `DirectLightingRenderer::Draw` (`engine/lighting`). The light volume and the god-ray option live in
`CloudRenderer.cpp` and are controlled through `StudioTerrainLayerOptions` (`cloudLightVolume`, `cloudGodrayStrength`,
`cloudVolumeDebugAltitude`).
