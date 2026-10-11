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
  "Three unit-integral vertical profiles (thin low stratus, flat-based rounded cumulus, full-height tower with a denser anvil top) are blended by cloudType so the column optical depth is preserved.",
  "Billows come from one baked 32^3 tileable cellular-noise volume (R base shape, G/B/A erosion octaves; a 16-byte voxel = one fetch), sampled at two non-aligned scales (one rotated) so the lattice never reads as a grid.",
  "The step schedule grows geometrically from kBaseStep (60 m) with the growth rate solved per pixel so a long grazing ray keeps fine steps near the camera; a uniform budget step made side-on clouds blurry, striped and see-through - do not replace it with one.",
  "Inside cloud the step is at least kInCloudScheduleFraction of the schedule step so a dense deck does not spend the whole budget; steps are not finer than kFootprintStep of the pixel footprint (a camera far from the shell must not march empty space in 60 m steps).",
  "The march ends at kMaxSteps steps or when transmittance falls below kOpaqueCutoff; empty-space skipping (kEmptySkip x) applies only where the cloud field has no cloud in that column.",
  "The march target is half resolution by default (cloud_resolution_scale = 0.5), bounded by the nearest full-resolution depth, and is composited with an alpha blend (L/(1-T), 1-T -> L + T*scene) and bilinear upsampling, without a scene copy.",
  "Erosion noise is used only at moderate range (25-60 km in the original design) and never in the sun march; sun colour comes from the atmosphere transmittance LUT sampled once per pixel.",
  "Lighting is three scattering octaves (energy, extinction and anisotropy halved each time) plus an isotropic multiple-scatter floor and a height-dependent ambient.",
  "The filtered far field is the only far field (the old shape-to-coverage blend, the gDensity->gDensityFar fade and the ORBIT_CLOUD_FILTERED_FAR switch were removed after it was verified on screen): all three cloud shaders include it and Extinction() blends, by baseBlend, from the point-sampled step transmittance exp(-sigma*gStep) to FarFieldTransmittance(skinCoverage, hardness, unitSigma*gStep), a table generated from the baked noise (CloudFarField.cpp). Every Extinction() caller must set gStep to the length the sample stands for; gStep = 0 keeps point sampling. CPU reference: EvaluateFarFieldTransmittance. Status: compiled with glslangValidator and unit-tested on CPU. Seen on screen 2026-10-11 (same camera and sim time, flag off vs on, a second flag-off launch as control): from 1590 and 400 km the old near-complete grey overcast becomes sparse cloud over clear sea (26/255 and 20/255 mean difference), the two converge at 100 km (4.7) and match at 25 km and below (0.3, 0.01). The look from orbit is sparser and clearer than the old translucent haze by design, which still needs an art decision; frame time and the god-ray gStep are unmeasured.",
  "ORBIT_CLOUD_POINT_LIGHTS=1 (read once at construction, default off) lets up to 16 point lights scatter into the clouds (CloudRenderer::PointLight, passed to Draw). The march pipeline gains a fourth shader buffer (g_cloudLights at binding 3: [0].x = count, then camera-relative position and scene-linear radiant intensity per light) and its texture bindings shift from 3/4/5 to 4/5/6 through the CLOUD_*_BINDING macros; with the flag off the three cloud shaders are byte-identical in SPIR-V to the previous commit (verified), so the default path cannot have changed. Each step adds irradiance (intensity / d^2, d floored at 5 m) x albedo x the sun's own single+multiple scattering shape x 4.6 x shading, with the light-path optical depth taken as the sample's extinction over at most 150 m (no marching); the output compressor ceiling also tracks the brightest light, because the sun-based ceiling would flatten any light at night. Intensity is W/sr as a fraction of 1361 W/m2 (lumens / 683 / 4 pi / 1361), so a light whose irradiance equals the sun's lights a cloud as bright as the sun does. Producer today: authored point/spot lights (15 strongest by flux over squared distance from the camera plus one aggregate of the rest; spots are treated as points). Per-frame buffers are retired through CloudRenderer::BeginFrame, called next to the glass renderer's. Seen on screen 2026-10-11: an authored 1e11 lm light placed inside a triangulated cumulus brightens a halo on exactly that cloud (2892 pixels brighter, 20 darker; noise floor 18/255), and 13-14 transient flash lights per frame reach this pass while a flash is live (producers: authored lights and TransientFlashRegistry flashes). The pass ignores Range; the 150 m diffusion length and the gain are modelling choices, not tuned against a render, and the night look and a flash through a cloud at dusk are not verified.",
  "The shader is an embedded string in CloudRenderer.cpp: saving it rebuilds the target and takes the automatic generation handoff.",
  "ORBIT_CLOUD_DEFINES=\"NAME=VALUE;...\" (read once at construction) injects #define lines into every cloud shader for A/B work without a rebuild (see /tools/ab-capture). Tunables, defaults = shipped behaviour: CLOUD_JITTER_FIX=0 (1 clamps the sample jitter to the thin-layer step cap, evaluated by ThinLayerStepCap at the cursor) and CLOUD_INCLOUD_STEP_SCALE=1.0. Council review R-20261011-175457-44e64759: neither is visible in four scenes, the jitter fix costs one extra ColumnData per step, so both stay at their defaults.",
]
related = ["/rendering/clouds/shadows-and-light-volume"]
depends_on = ["/rendering/clouds"]
verify = [
  "python tools/check_cloud_shaders.py now also compiles the point-light march variant (bindings 0-3 buffers, 4-6 textures); the flag-off identity is checked by diffing SPIR-V against the previous commit.",
  "python tools/check_cloud_shaders.py compile-checks the embedded cloud shaders (flag off and on) with glslangValidator on Linux; syntax and types only, not DXC and not rendered.",
  "Frame-time average with clouds on vs off at 8 km (Release, Earth): original numbers were +0.7 ms looking at the horizon and +1.65 ms looking down; a regression beyond that needs an explanation.",
  "Side-on grazing clouds at the horizon are sharp, not striped or see-through.",
]
verified = "f39887e"

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
