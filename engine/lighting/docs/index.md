+++
path = "/rendering/lighting"
title = "Lighting"
kind = "subsystem"
status = "stable"
owner_module = "OrbitLighting"
summary = """
Near-field lighting stack: DirectLightingRenderer combines the stellar term (with cloud shadow and proxy sun shadow) \
with sky fill from the radiance cache; a screen-space final gather and a radiance clipmap supply indirect light; hybrid \
and exact (hardware ray) reflections cover specular. Occlusion queries go through one visibility registry that knows \
terrain, authored proxies and analytic bodies. Every stage can be bypassed per view to bisect an artefact."""
keywords = ["lighting", "direct lighting", "indirect", "radiance cache", "final gather", "reflections", "visibility", "ray query", "bypass", "gi"]
sources = [
  "engine/lighting/include/orbit/lighting/DirectLighting.hpp",
  "engine/lighting/include/orbit/lighting/RadianceClipmap.hpp",
  "engine/lighting/include/orbit/lighting/RadianceEstimator.hpp",
  "engine/lighting/include/orbit/lighting/ScreenSpaceFinalGather.hpp",
  "engine/lighting/include/orbit/lighting/HybridReflectionRenderer.hpp",
  "engine/lighting/include/orbit/lighting/ExactReflectionQueryRenderer.hpp",
  "engine/lighting/include/orbit/lighting/Visibility.hpp",
]
symbols = ["DirectLightingRenderer", "HybridReflectionRenderer", "ExactReflectionQueryRenderer", "ScreenSpaceFinalGatherRenderer", "RadianceClipmapConfig", "GpuRadianceCell"]
invariants = [
  "Occlusion for lighting is answered through the shared visibility registry (terrain, authored proxies, analytic bodies); do not add a parallel occlusion path for one effect.",
  "The near-field indirect stack is off when the planet is seen from orbit and whenever bypass_indirect_lighting is set; fills that depend on it vanish with it.",
  "Without sunlight (night side, space) the atmosphere's sky summary is zero, so sky-derived fills are zero too.",
  "Terrain and sky are not proxies: only authored structures cast the proxy sun shadow.",
  "Each frame stage has a per-view bypass flag so artefacts can be bisected one stage at a time; bypass flags are diagnostics and must be restored afterwards.",
  "Lighting library sources (estimator, cell layout, shaders) hot-reload through the automatic Studio-generation handoff; embedded HLSL compiles at startup so a failed shader leaves the running generation alive.",
]
related = ["/rendering/terrain", "/rendering/terrain/clipmaps/debugging"]
depends_on = ["/rendering"]
used_by = ["/editor/viewport"]
verify = [
  "ctest -R Orbit.Lighting (the lighting tests registered in engine/lighting/CMakeLists.txt).",
  "orbit_view_terrain_layers_get lists every bypass_* flag; all must be false in normal use.",
]
verified = "b0a0de7f"

[routes]
"cache cells, residency, dark slabs, relighting, 96-byte cell" = "radiance-cache"
"visibility queries, providers, terminal miss, reflections, final gather" = "visibility-and-reflections"
"lighting budget, quality policy, overload/recovery, stable view" = "scheduler"
"no or wrong sun shadow from authored boxes/spheres, proxy walls look wrong" = "proxy-sun-shadow"
"shadowed or enclosed areas are black instead of sky-lit" = "sky-cache-fill"
"exposure, blown highlights, dark interiors lifted to grey" = "eye-adaptation"
"emissive materials and planetary emission, local lights" = "/legacy/v0-0-7-spec"
"design baseline for dynamic lighting" = "/legacy/research-v007-dynamic-lighting-baseline"
+++

## Where each concern lives (`engine/lighting`)

| Concern | Headers |
| --- | --- |
| direct light (stellar term, cloud shadow, sky fill) | `DirectLighting.hpp`, `ProxySunShadow.hpp`, `SkyVisibility.hpp` |
| indirect light | `RadianceClipmap.hpp`, `RadianceClipmapResidency.hpp`, `RadianceEstimator.hpp`, `ScreenSpaceFinalGather.hpp` |
| reflections | `HybridReflectionRenderer.hpp`, `ExactReflectionQueryRenderer.hpp`, `ReflectionPolicy.hpp` |
| visibility sources | `Visibility.hpp`, `AnalyticBodyVisibility.hpp`, `TerrainHeightfieldVisibility.hpp`, `SoftwareProxyVisibility.hpp`, `HardwareRayQueryVisibility.hpp` |
| authored occluders drawn as geometry | `ProxySurface.hpp`, `SurfaceBuffer.hpp`, `SurfaceData.hpp` |
| emission | `MaterialEmission.hpp`, `Emissive*.hpp`, `PlanetaryEmission*.hpp`, `LocalLightRegistry.hpp` |

## Per-view bypasses (bisecting a frame)

`view.terrain_layers_set` (MCP `orbit_view_terrain_layers_set`) skips one stage at a time:
`bypass_cloud_shadow`, `bypass_proxy_sun_shadow`, `bypass_proxy_surfaces`, `bypass_sky_cache`,
`bypass_indirect_lighting` (final gather + hybrid reflections), `bypass_hybrid_reflections`,
`bypass_radiance_cache`, `bypass_near_field_water`, `bypass_atmosphere`. `indirect_coverage_view` replaces the
final gather's contribution with its coverage (red confidence, green gathered brightness, magenta = nothing returned).
Method: toggle one flag, compare, restore it (`/rendering/terrain/clipmaps/debugging`).
