+++
path = "/rendering/clouds/raymarch"
title = "Cloud ray march (shape, lighting, cost)"
kind = "concept"
status = "stable"
owner_module = "OrbitCelestialClouds"
summary = """
CloudRenderer ray-marches the body's first cloud layer at reduced resolution into an RGBA16F target (radiance + transmittance), \
bounded by the nearest full-resolution depth, then resolves temporally and composites over the scene. Shape blends three \
vertical profiles with a baked 32^3 cellular-noise volume; the step schedule grows geometrically per pixel so grazing rays keep \
fine steps near the camera."""
keywords = ["ray march", "cloud shape", "billow", "noise", "step", "empty space skipping", "half resolution", "cloud cost", "blurry clouds", "striped clouds", "see-through", "profile", "anvil"]
sources = [
  "engine/celestial_clouds/src/CloudRenderer.cpp",
  "engine/celestial_clouds/include/orbit/celestial_clouds/CloudRenderer.hpp",
  "engine/celestial_clouds/src/CloudFarField.cpp",
  "engine/studio_ui/src/StudioViewportAtmospherePass.cpp",
  "engine/studio_ui/src/StudioViewportRenderer.cpp",
  "docs/ORBIT_PERFORMANCE.md",
]
symbols = ["FarFieldTransmittance", "gStep", "PointLightSource", "UploadPointLights", "kMaxSteps", "kBaseStep", "kEmptySkip", "kOpaqueCutoff", "kInCloudScheduleFraction", "kFootprintStep"]
invariants = [
  "Each interval uses a spatial/temporal stratified sample, and its partition length is jittered too. A thin-layer cap is evaluated at the interval start; shortening an interval requires re-sampling its density.",
  "tools/orbit_report_rendering.py restores report camera/time via RPC, verifies the restored pose, pins exposure and captures default/reference/repeat images with provenance and timing. It refuses captures spanning a viewport resize and does not resolve reports automatically.",
  "Three unit-integral vertical profiles (thin low stratus, flat-based rounded cumulus, full-height tower with a denser anvil top) are blended by cloudType so the column optical depth is preserved.",
  "Billows come from one baked 32^3 tileable cellular-noise volume (R base shape, G/B/A erosion octaves; a 16-byte voxel = one fetch), sampled at two non-aligned scales (one rotated) so the lattice never reads as a grid.",
  "The step schedule grows geometrically from kBaseStep (60 m) with the growth rate solved per pixel so a long grazing ray keeps fine steps near the camera; a uniform budget step made side-on clouds blurry, striped and see-through - do not replace it with one.",
  "The geometric schedule is a search budget. A detected density sample is re-sampled in a shorter interval capped to clamp(15 m + 0.001 times distance, 15 m, 175 m); the represented interval is chosen before density evaluation, clipped to the ray end and used for Beer-Lambert integration.",
  "The march ends at kMaxSteps steps or when transmittance falls below kOpaqueCutoff; empty-space skipping (kEmptySkip x) applies only where the cloud field has no cloud in that column.",
  "The march target is half resolution by default (cloud_resolution_scale = 0.5), bounded by the nearest full-resolution depth, and is composited with an alpha blend (L/(1-T), 1-T -> L + T*scene) and bilinear upsampling, without a scene copy.",
  "Erosion noise is used only at moderate range (25-60 km in the original design) and never in the sun march; sun colour comes from the atmosphere transmittance LUT sampled once per pixel.",
  "Lighting uses four scattering octaves plus height-dependent sky ambient. Terrain visibility affects the stellar term; ambient remains independent.",
  "All cloud shaders use the same filtered far-field coefficient. FarFieldTransmittance filters one physical noise correlation length (kBaseVoxelMeters, 700 m), independent of the numerical integration step; gStep = 0 keeps point sampling. Changing a march step must not change the material extinction.",
  "ORBIT_CLOUD_POINT_LIGHTS=1 binds up to 16 point lights at buffer slot 5; the terrain visibility field is slot 4. March textures use bindings 5/6/7 without point lights and 6/7/8 with them. Per-frame light buffers retire through BeginFrame after submission-slot fences.",
  "The shader is an embedded string in CloudRenderer.cpp: saving it rebuilds the target and takes the automatic generation handoff.",
  "ORBIT_CLOUD_DEFINES and clouds.set_defines compile development variants transactionally. CLOUD_SUN_VOLUME=0 (default) evaluates a 16-slab altitude-stratified integral, interpolated over 350 m view-ray knots with temporal phase (CLOUD_SUN_KNOTS=1). CLOUD_SUN_KNOTS=0 is the per-sample reference. CLOUD_SUN_VOLUME=1 compares the coarser full-volume lighting approximation, which is unsuitable at cloud skins. Failed compilations leave the active pipelines and defines unchanged.",
]
related = ["/rendering/clouds/shadows-and-light-volume"]
depends_on = ["/rendering/clouds"]
verify = [
  "python tools/check_cloud_shaders.py now also compiles the point-light march variant (bindings 0-3 buffers, 4-6 textures); the flag-off identity is checked by diffing SPIR-V against the previous commit.",
  "python tools/check_cloud_shaders.py compile-checks the embedded cloud shaders (flag off and on) with glslangValidator on Linux; syntax and types only, not DXC and not rendered.",
  "Frame-time average with clouds on vs off at 8 km (Release, Earth): original numbers were +0.7 ms looking at the horizon and +1.65 ms looking down; a regression beyond that needs an explanation.",
  "Side-on grazing clouds at the horizon are sharp, not striped or see-through.",
]
verified = "283b7b066432087a3acd420afc2e112b30ca3482"

[[diagnose]]
symptom = "side-on or grazing clouds look blurry, striped or see-through"
steps = [
  "This is the symptom of a step schedule that does not keep fine steps near the camera: check the per-pixel solved growth rate, kBaseStep and kInCloudScheduleFraction in CloudRenderer.cpp.",
  "Raise cloud_resolution_scale to 1 to separate a resolution problem from a march problem.",
  "Compare cloud_temporal on and off: the resolve averages jitter; if only the raw march shows stripes the step budget is the cause.",
]
docs = ["/rendering/clouds"]

[[diagnose]]
symptom = "cloud rendering is too slow"
steps = [
  "Lower cloud_resolution_scale (default 0.5; 1 costs about four times as much).",
  "Capture a GPU trace (orbit_profiler_capture / orbit_renderdoc_capture) and read the <view>.Clouds, <view>.CloudsResolve and <view>.CloudsComposite passes separately.",
  "Check cloud_light_volume: off marches everything per sample, which is slower for god rays and cloud-on-cloud shadow.",
]
docs = ["/rendering/clouds/shadows-and-light-volume"]
+++

## Current values versus older notes

`docs/ORBIT_PERFORMANCE.md` (section "Clouds in the clipmap view") was written when the march was first made
half resolution. Its numbers have moved with the code; trust the constants in `CloudRenderer.cpp`, which at the
verified commit are `kMaxSteps = 224` (the note says 112), `kOpaqueCutoff = 0.004` (the note says 2 %) and
`kBaseStep = 60 m`. The note says the cloud passes run "before `<view>.Atmosphere`"; the render graph adds the atmosphere pass first and the cloud shader's own comment says the cloud is composited after it. The note also lists the cached light volume and temporal reprojection as "not done"; both now
exist (`/rendering/clouds/shadows-and-light-volume`). The measured costs in the note are for the earlier version.

## Pass graph (per view)

```text
<view>.Clouds           half-resolution march -> RGBA16F (radiance + transmittance)
<view>.CloudsResolve    temporal accumulation (reprojected through the cloud shell, clamped to the neighbourhood)
<view>.CloudsComposite  composited over the scene AFTER <view>.Atmosphere has been drawn;
                        the march itself adds aerial perspective over the camera-to-cloud distance
```

## Simulated storm domain (phase 5 of the storm domain plan)

`CloudRenderer::SetStormBuffer(buffer)` points every cloud pipeline (the march, the cloud shadow and the sun light volume) at a simulation's output: 32 header floats (active flag, domain centre and east/north axes on the planet, planet radius, cell sizes, dimensions, edge fade) then the extinction (1/m) of each cell, `[k][j][i]`, produced by `GpuStormSolver::ExportCloud`. The buffer is bound at buffer slot 3 in all three pipelines; the point lights moved to slot 4 and the texture bindings moved up by one (depth 4 or 5, transmittance 5 or 6, multiscatter 6 or 7). A placeholder with the active flag 0 is bound when no storm is placed.

`Extinction()` is now a wrapper: with the flag set, inside the domain (a smooth fade over the sponge width at its edge) it returns the trilinear-sampled simulated extinction, carved by the erosion noise when a pixel is narrower than about 140 m; outside, `ExtinctionProcedural()` as before. Because the light volume and the shadow pass use `Extinction()` too, the storm shades itself and the ground. `CLOUD_STORM=0` (shader defines) removes the lookup, which is how the split view compares simulated and procedural clouds on the same pixels (`clouds.split_set` with `CLOUD_STORM=0` on the right; the shadow and light-volume shaders are shared, so only the march differs).

Measured (2026-10-12): with no storm placed the images match the build before the change within the harness noise (mean 0.04/255, the same as run-to-run repeats) and the Clouds pass is unchanged (interleaved A/B of default against `CLOUD_STORM=0`: 5.1 vs 5.4, 5.7 vs 5.5, 0.69 vs 0.63 ms, inside the +-5% spread). With a 256 km storm placed 60 km away the pass is about 3 ms either way in that view.

The domain mask is multiplied by an influence map (header word 10 = index of a 0 to 1 map over the parent grid, written by `GpuStormSolver::ExportCloud`: the column optical depth, a maximum over about 40 km, smoothed), so the procedural clouds stay everywhere the simulation has no cloud nearby and only the storm's neighbourhood is replaced. Without it the domain was a clear patch (the simulation has no shallow cumulus); with it the cumulus field returns around the storm. Cost: unchanged within the harness spread (3.9 vs 4.0 ms in that view).

Known limits: the domain edge is a fade to the procedural clouds (and the sponge removes anything the simulation lets reach the edge); the grid is 500 m to 2 km and the silhouette detail is the erosion noise, not simulated; the sun light volume cascades are camera-windowed, so a storm far from the camera is lit by the march's own sun samples.

The storm buffer can also carry the nest's child level (phase 7): header word 22 says it is present, 23 to 27 give its cell sizes and dimensions, 28 and 29 its centre relative to the domain centre, 30 the index of its first cell and 31 the width of the blend. `StormSigma` samples the child with the same vertical grid and blends from the parent's value to the child's across that width, so the child's finer extinction replaces the parent's only where the child exists.

The storm's close-range detail (`StormSigma`, defines `CLOUD_STORM_DETAIL`, `CLOUD_STORM_WARP`, `CLOUD_STORM_CARVE`, `CLOUD_STORM_CORE`): the simulation is sampled at a position displaced by low-frequency noise (billows), interpolated with smoothstep weights (so the carved surface does not show the grid's cell planes), and carved by a continuous three-octave modulation that acts on the thin fringe and not on the dense core. It fades out as pixels get wider than the features. Measured and tuned in `/world/weather-lab/global-system-status-p9`.
