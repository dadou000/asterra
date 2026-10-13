+++
path = "/rendering/terrain/clipmaps/close-range-review"
title = "Harmony close-range terrain council: defects, fixes and remaining work"
kind = "playbook"
status = "experimental"
owner_module = "OrbitTerrainRender"
summary = "The 0.1.0 council found planetary stamp cancellation, CPU parent-normal mismatch, removed shading diagnostics and a missing erosion dispatch barrier. These are repaired; final composed-height normals, physical-page fine detail and shared water visibility still require runtime validation and further implementation."
keywords = ["council", "Harmony", "0.1.0", "close range", "wetness", "artifacts", "normals", "vector displacement"]
sources = [
  "engine/terrain_render/src/SurfaceEffects.cpp",
  "engine/terrain_render/src/SurfaceEffectShader.cpp",
  "engine/terrain_render/src/WaterVolumeShader.cpp",
  "engine/terrain_render/src/TerrainPreviewRenderer.cpp",
  "engine/terrain_stream/src/TerrainSampleStreamer.cpp",
  "engine/terrain_gpu/src/PhysicalPageCompositeCompute.hpp",
  "engine/terrain_gpu/src/RegionDeltaCompute.hpp",
  "engine/studio_ui/src/StudioViewportTerrainPass.cpp",
  "engine/studio_ui/src/StudioViewportLightingPass.cpp",
  "engine/terrain_render/tests/SurfaceEffectsTests.cpp",
  "engine/terrain_render/tests/TerrainSurfaceShaderTests.cpp",
  "tests/TerrainSampleStreamerTests.cpp"]
related = ["/rendering/terrain/clipmaps/debugging", "/rendering/terrain/clipmaps/precision-and-pages", "/rendering/terrain/clipmaps/continuous-detail-plan", "/rendering/terrain/surface-material", "/rendering/terrain/water-volume-shader"]
verify = ["Run Orbit.TerrainSurfaceEffects, Orbit.TerrainSampleStreamer and Orbit.TerrainSurfaceShader; use the production Windows Vulkan viewport for visual acceptance."]
verified = "8dc17324af29e897a3eb204ad9d93e3319b832c4"
+++

## Evidence and first patch

Baseline: `0.1.0`, `1d19bb17d11493409192b36cb62c1cbb115027ce`.
Three independent source reviews covered shading, sampling and geometry.
Reported close-up artifacts have **not** been reproduced in a running Studio.

| Defect | First patch | Evidence |
| --- | --- | --- |
| `acos(dot(float3))` loses small stamp distance | CPU/GPU normalized chord angle | Before: a 25 cm stamp sampled at quarter-radius returns 0.8 instead of 0.45. Regression fails before and passes after. Float-direction storage still limits tiny oblique footprints. |
| CPU morph uses child footprint/epsilon at parent endpoint | Use coarse footprint/spacing and cached coarse center | A filtered curved-source regression compares child samples against independently generated parent samples; fails before and passes after. |
| Bed assembly deletes shading debug branch | End water replacement before diagnostic | Production bed/effect assembly retains the emissive early return; regression fails before and passes after. |
| Field generation to erosion lacks compute memory dependency | UAV barriers around actual region-delta dispatch | Vulkan storage read/write dependency reviewed; renderer syntax check passes. GPU execution remains untested here. |

Linux validation compiled the CPU tests directly with GCC 13. The repository's
shader test ran with a scratch-only DXC CLI adapter because its production
compiler uses Windows COM. All six variants compiled to Vulkan 1.3 SPIR-V.
This is neither a full Windows build nor GPU runtime acceptance.

## Corrections to the earlier assessment

The production bed removes water coverage; standing water has its own pass.
Preview lighting writes a separate color target; shared opaque direct lighting
reads unlit material channels and overwrites that target. No double-lighting
defect was established. Production slope samples already use each ring's own
footprint and spacing; the mismatch below occurs after generation.

`drySurface` is selected from ocean capability, not a wetness bypass. Use the
existing `surface_effects` layer to isolate deposited wetness.

## Remaining defects

1. Physical pages and erosion deltas overwrite elevation without updating
   slopes at sample offsets 24/28. Geometry can therefore change while normals
   still describe the analytic source. A repair must run after all height
   writes, include neighboring dirty samples, respect logical window boundaries
   and account for morphed/collapsed vertices. Updating each region independently
   or dividing morphed neighbor height by original spacing risks new seams.
2. Physical pages contain absolute coarse heights. Their interior replacement
   removes finer procedural geometry. Define a coarse authority plus physical
   changes plus continuous residual before adding near refinement; do not add
   an already baked feature twice or bypass authoritative edits.
3. Near-field water evaluates solar glint outside common terrain/cloud sun
   visibility and receives raw stellar irradiance instead of atmospheric tint.
   Bring those shared inputs into water optical shading; do not create another
   visibility solver.

## Runtime isolation

At one fixed affected camera/light pose, capture the existing version overlay
and sidecar. Toggle `surface_effects`, `physical_pages` and
`bypass_near_field_water` one at a time with `view.terrain_layers_set`, restoring
each afterward. Then inspect `clipmap_shading_view`, sample health and frozen
clipmaps through the existing overlay RPC. A healthy shading diagnostic only
checks finite inputs; it cannot prove that normals match final geometry.

Saving these native and embedded shader files follows the central automatic
Studio generation handoff. No new state, ABI, watcher or GPU resources were
introduced. Live handoff and visual acceptance still need Windows validation.
