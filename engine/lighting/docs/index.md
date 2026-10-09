+++
path = "/rendering/lighting"
title = "Lighting"
kind = "subsystem"
status = "stable"
owner_module = "OrbitLighting"
summary = """
Near-field lighting stack: DirectLightingRenderer combines the stellar term (with cloud shadow and proxy sun shadow) \
with sky fill from the radiance cache; a screen-space final gather and a radiance clipmap supply indirect light; hybrid \
reflections with software/hardware triangle queries cover smooth specular. Occlusion queries go through one visibility registry that knows \
terrain, authored proxies and analytic bodies. Every stage can be bypassed per view to bisect an artefact."""
keywords = ["lighting", "direct lighting", "indirect", "radiance cache", "final gather", "reflections", "visibility", "ray query", "bypass", "gi"]
sources = [
  "engine/lighting/include/orbit/lighting/DirectLighting.hpp",
  "engine/lighting/include/orbit/lighting/RadianceClipmap.hpp",
  "engine/lighting/include/orbit/lighting/RadianceEstimator.hpp",
  "engine/lighting/include/orbit/lighting/ScreenSpaceFinalGather.hpp",
  "engine/lighting/include/orbit/lighting/HybridReflectionRenderer.hpp",
  "engine/lighting/include/orbit/lighting/ReflectionScene.hpp",
  "engine/lighting/include/orbit/lighting/Visibility.hpp",
]
symbols = ["DirectLightingRenderer", "HybridReflectionRenderer", "ScreenSpaceFinalGatherRenderer", "RadianceClipmapConfig", "GpuRadianceCell"]
invariants = [
  "Occlusion for lighting is answered through the shared visibility registry (terrain, authored proxies, analytic bodies); do not add a parallel occlusion path for one effect.",
  "The near-field indirect stack is off when the planet is seen from orbit and whenever bypass_indirect_lighting is set; fills that depend on it vanish with it.",
  "Without sunlight (night side, space) the atmosphere's sky summary is zero, so sky-derived fills are zero too.",
  "Terrain and sky are not proxies: only authored structures cast the proxy sun shadow.",
  "Each frame stage has a per-view bypass flag so artefacts can be bisected one stage at a time; bypass flags are diagnostics and must be restored afterwards.",
  "Lighting library sources (estimator, cell layout, shaders) hot-reload through the automatic Studio-generation handoff; embedded HLSL compiles at startup so a failed shader leaves the running generation alive.",
  "The screen-space final gather traces ONE ray per pixel (the four pixels of a 2x2 quad take four different directions) and reconstructs each pixel from its 3x3 neighbourhood in groupshared memory with depth/normal bilateral weights; per-pixel cost is therefore independent of the old four-ray budget. While the camera is still (history reusable) the sample pattern rotates every frame and the temporal weight is at least 0.94; in motion the pattern is fixed so noise does not shimmer.",
  "The gather's world-space fallback sphere-traces the corner-packed f16 distance mirror (MeshSdfScene::distanceCorners, SDF_DIST_CORNERS in SdfTraceShader.hpp), one uint4 load per step; SdfGatherInput::distance must be that mirror, not the plain f32 volume.",
  "BuildTiledLightGrid uses flat count-then-fill arrays (no per-tile heap vectors): tiles keep the first maximumLightsPerTile lights in submission order and the rest count as droppedAssignments.",
  "The final gather's edit of the earlier invariant: it traces ONE ray per 2x2 QUAD (not per pixel). Each thread picks a pixel of its quad (a different one every frame while the camera is still), traces one ray, and every pixel of the quad is rebuilt from the 3x3 quad neighbourhood with bilinear-style spatial weights times normal/depth bilateral weights, so the tracing cost does not scale with pixel count.",
  "The gather's 3x3 reconstruction suppresses fireflies: a neighbourhood sample whose luminance exceeds 4x the weighted mean of the others (plus a small floor) is scaled down to that level before filtering, so one ray that hits a bright SDF voxel is not spread into a blob. It trades a little energy (about 2% mean frame brightness in the atrium) for removing the dominant speckle.",
  "Pass culling is content driven, not configured: the final gather counts surface, smooth (roughness <= 0.25 or metallic), uncovered (confidence < 0.5) and mirror-like (roughness <= 0.1) pixels into a 16-byte host-visible buffer per frame slot (FinalGatherPresentation::needStatsBuffers). The CPU reads each slot when it comes round again and keeps a pass alive for 90 frames after the last frame it was needed: hybrid reflections and their copy-back need >0.2% smooth pixels, the radiance-cache fallback >1% uncovered pixels. Until data exists (or for sky-only views) every pass stays on. The bypass_* flags still force a pass off. The former proxy-only exact pass is no longer scheduled: triangle queries and independent reflection history live inside HybridReflections. A roughness threshold above 0.25 or a reflection debug view disables its smooth-pixel culling because the counters use the fixed 0.25 threshold.",
]
related = ["/rendering/lighting/smooth-reflections", "/rendering/terrain", "/rendering/terrain/clipmaps/debugging"]
depends_on = ["/rendering"]
used_by = ["/editor/viewport"]
verify = [
  "ctest -R Orbit.Lighting (the lighting tests registered in engine/lighting/CMakeLists.txt).",
  "orbit_view_terrain_layers_get lists every bypass_* flag; all must be false in normal use.",
  "ctest -R Orbit.LightingLocalLightRegistry (capped-tile ordering and dropped counts).",
]
verified = "f842a956"

[routes]
"cache cells, residency, dark slabs, relighting, 96-byte cell" = "radiance-cache"
"visibility queries, providers, terminal miss, reflections, final gather" = "visibility-and-reflections"
"smooth mirrors, glass, triangle BVH, reflection history" = "smooth-reflections"
"lighting budget, quality policy, overload/recovery, stable view" = "scheduler"
"no or wrong sun shadow from authored boxes/spheres, proxy walls look wrong" = "proxy-sun-shadow"
"shadowed or enclosed areas are black instead of sky-lit" = "sky-cache-fill"
"exposure, blown highlights, dark interiors lifted to grey" = "eye-adaptation"
"emissive materials and planetary emission, local lights" = "/legacy/v0-0-7-spec"
"design baseline for dynamic lighting" = "/legacy/tree-history-research-v007-dynamic-lighting-baseline"
+++

## Where each concern lives (`engine/lighting`)

| Concern | Headers |
| --- | --- |
| direct light (stellar term, cloud shadow, sky fill) | `DirectLighting.hpp`, `ProxySunShadow.hpp`, `SkyVisibility.hpp` |
| indirect light | `RadianceClipmap.hpp`, `RadianceClipmapResidency.hpp`, `RadianceEstimator.hpp`, `ScreenSpaceFinalGather.hpp` |
| reflections | `HybridReflectionRenderer.hpp`, `ReflectionScene.hpp`, `ReflectionTraceShader.hpp` |
| visibility sources | `Visibility.hpp`, `AnalyticBodyVisibility.hpp`, `TerrainHeightfieldVisibility.hpp`, `SoftwareProxyVisibility.hpp`, `HardwareRayQueryVisibility.hpp` |
| authored occluders drawn as geometry | `ProxySurface.hpp`, `SurfaceBuffer.hpp`, `SurfaceData.hpp` |
| emission | `MaterialEmission.hpp`, `Emissive*.hpp`, `LocalLightRegistry.hpp` |

## Per-view bypasses (bisecting a frame)

`view.terrain_layers_set` (MCP `orbit_view_terrain_layers_set`) skips one stage at a time:
`bypass_cloud_shadow`, `bypass_proxy_sun_shadow`, `bypass_proxy_surfaces`, `bypass_sky_cache`,
`bypass_indirect_lighting` (final gather + hybrid reflections), `bypass_hybrid_reflections`,
`bypass_radiance_cache`, `bypass_near_field_water`, `bypass_atmosphere`. `indirect_coverage_view` replaces the
final gather's contribution with its coverage (red confidence, green gathered brightness, magenta = nothing returned).
Method: toggle one flag, compare, restore it (`/rendering/terrain/clipmaps/debugging`).
