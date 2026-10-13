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
  "engine/lighting/include/orbit/lighting/TransientLights.hpp",
  "engine/lighting/include/orbit/lighting/TransientFlashRegistry.hpp",
  "engine/lighting/include/orbit/lighting/CityLightTree.hpp",
]
symbols = ["CityLightTree", "TransientLightsFromFlash", "DirectLightingRenderer", "HybridReflectionRenderer", "ScreenSpaceFinalGatherRenderer", "RadianceClipmapConfig", "GpuRadianceCell"]
invariants = [
  "Occlusion for lighting is answered through the shared visibility registry (terrain, authored proxies, analytic bodies); do not add a parallel occlusion path for one effect.",
  "The near-field indirect stack is off when the planet is seen from orbit and whenever bypass_indirect_lighting is set; fills that depend on it vanish with it.",
  "Without sunlight (night side, space) the atmosphere's sky summary is zero, so sky-derived fills are zero too.",
  "Terrain and sky are not proxies: only authored structures cast the proxy sun shadow.",
  "Each frame stage has a per-view bypass flag so artefacts can be bisected one stage at a time; bypass flags are diagnostics and must be restored afterwards.",
  "Lighting library sources (estimator, cell layout, shaders) hot-reload through the automatic Studio-generation handoff; embedded HLSL compiles at startup so a failed shader leaves the running generation alive.",
  "The screen-space final gather traces ONE ray per pixel (the four pixels of a 2x2 quad take four different directions) and reconstructs each pixel from its 3x3 neighbourhood in groupshared memory with depth/normal bilateral weights; per-pixel cost is therefore independent of the old four-ray budget. While the camera is still (history reusable) the sample pattern rotates every frame and the temporal weight is at least 0.94; in motion the pattern is fixed so noise does not shimmer.",
  "The gather's world-space fallback sphere-traces the corner-packed f16 distance mirror (MeshSdfScene::distanceCorners, SDF_DIST_CORNERS in SdfTraceShader.hpp), one uint4 load per step; SdfGatherInput::distance must be that mirror, not the plain f32 volume.",
  "TransientLightsFromFlash (TransientLights.hpp) converts a DischargeGraph into at most maxLights ordinary LocalLights for one exposure window: clusters are energy-preserving (sum of flux / efficacy x exposure = graph energy in the window), flux is energy / exposure x luminousEfficacyOfRadiation (assumed 150 lm/W, then divided by the scene's 683 lm/W like any local light), range is where illuminance falls to minimumIlluminanceLux, and a 30 microsecond stroke inside a 16 ms window deposits all its energy. A 300 kJ stroke gives about 220 lux at 1 km. TransientFlashRegistry (TransientFlashRegistry.hpp) owns the live flashes (RPC lightning.fire); StudioViewportRenderer::TransientFlashLights feeds them to the tiled-light pass and, with ORBIT_CLOUD_POINT_LIGHTS=1, to the cloud march. A storm-solver flash handed to Add keeps its energy (a 2.2e5 J flash gives 3.316e7 lumen-s, matching energy x 150 lm/W) and stays inside its channel extent (ctest -R Orbit.StormToLights).",
  "CityLightTree (CityLightTree.hpp) is a hierarchy over a very large LocalLight set. Cut(receiver, maxAngle, maxLights) returns a bounded list in which every light lies in exactly one selected node (total flux preserved), nodes subtending more than maxAngle at the receiver are refined first, a quarter of the budget is kept to expand the nearest leaves into original lights, merged groups carry the flux-weighted centroid and colour and, if directional, a spot whose cone reproduces the group's directionality. The cut is only valid for the receiver it was made for: a cut made 3.8 km away merges a surface's own lamps and is 99.8 % off there. Use per receiver region (cloud base, distant town, tile), not as the camera's global light list for far ground. Measured (CPU): 1,000,000 lamps, tree build 1.3 s, cut 0.35 ms, tiled grid from the cut 1.9 ms versus 4.2 s for the flat list.",
  "BuildTiledLightGrid uses flat count-then-fill arrays (no per-tile heap vectors): tiles keep the strongest lights (flux x luminance / squared camera distance, ties in submission order); when a tile overflows its last slot holds one shared proxy point light carrying the dropped lights' summed flux at their flux-weighted centroid (TiledLightGrid::proxyLights, stableId has kProxyLightIdBit), so droppedAssignments counts assignments represented by a proxy, not lost energy. TiledLightGridConfig::aggregateOverflow = false restores first-N truncation. Spot cone shaping is not preserved inside a proxy (isotropic point).",
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
  "ctest -R Orbit.LightingCityLightTree (energy, budget, level of detail, accuracy at and away from the receiver).",
  "ctest -R Orbit.LightingTransientLights (energy preservation, bounded budget, tile-grid flux).",
]
verified = "288b42a"

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

## Solar-terminator leak council: temporary A/B experiments (2026-10-13)

**Observations:** the orbital screenshot shows nonuniform yellow glow and substantial night-side illumination. A single screenshot is not a causal measurement. A static code audit identifies at least four independent authorities: macro-globe preview shading contains a fixed `0.045 * lightScale` floor; DirectLighting uses a camera-relative depth/sphere fallback; `AtmosphereSkyIrradianceSummary` populates radiance-cell sky lighting (from a sky view, not a cell-local atmosphere integration); and the cloud renderer computes solar transmittance independently of planetary visibility. Exposure/bloom can amplify any of these sources. The globe surface writes both HDR preview and material metadata before shared direct lighting; sky cache and gather may add energy afterwards, and the atmosphere then clouds are composited over it. Cloud shadow map generation is not proof that visible clouds obey planetary occlusion.

**Council 1, celestial geometry:** compare existing horizon smoothstep against an analytic finite-disc sphere, evaluated at each reconstructed body-space surface point (A). Do not use camera horizon for all pixels. A missing position is a diagnostic unknown, not visibility=1 certainty.

**Council 2, terrain authority:** reuse `EnsureTerrainSunShadow` for a coarse body-fixed relief field when enabled (B); do not ray-march the terrain source on the render thread. The existing 1024x512 world elevation field misses narrow mountain geometry; it is not authoritative at fine scales. B has a potentially substantial per-pixel traversal cost and is not suitable as the shipping implementation without hierarchy/caching.

**Council 3, lighting energy provenance:** temporarily remove the radiance-cache sky fill, final gather and proxy-sky fill to isolate the stellar direct signal (C). An actually physical night side still can be sky/GI lit by scattering and artificial sources; method C is an **isolation test**, not a proposed permanent darkness mask. Globe HDR preview and non-surface content remain independently shaded and must be compared in screenshots.

**Safety:** all 3 use one per-viewport method selector (0 preserves baseline), with separate debug booleans. The active shader uses existing shared HLSL visibility functions, with a fallback when no heightfield exists. Debug masks bypass late atmosphere/GI to preserve legibility and are not interpreted as physically meaningful HDR outputs. Do not compare auto-exposed screenshots: lock exposure, time, camera and cloud state; inspect sky/cache and surface values in linear radiance. Compare GPU direct-pass timing and mask seam stability, at orbit vs ground, under planet rotation and timewarp. Check body-space/lighting-frame transforms before accepting any change as correct. Selectively bypass cloud shadow, proxy shadow, sky fill and indirect stack to localize leakage.

**Acceptance still pending:** no Windows/Vulkan compilation or on-device comparisons were possible here. Verify DXC compilation of embedded `kSunVisibilityShader` combined with DirectLighting before enabling mode B; compare direct, globe-preview, sky-cache, atmosphere and cloud pass contributions. Restore method 0 after tests. The experiments must not silently replace the original compositor or ship as a default.
