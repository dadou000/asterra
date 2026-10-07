+++
path = "/rendering/lighting/proxy-sun-shadow"
title = "Proxy sun shadow and visible proxies"
kind = "concept"
status = "stable"
owner_module = "OrbitLighting"
summary = """
Authored Visibility Proxies (box/sphere) are lighting occluders: ProxySunShadowRenderer traces one hardware ray per \
visible pixel toward the star and DirectLightingRenderer multiplies the visibility into the stellar term next to the cloud \
shadow. ProxySurfaceRenderer also draws every proxy as lit geometry (surface class 3) that is lit exactly - ray-traced sun \
and sky fill - and deliberately ignores the radiance cache."""
keywords = ["proxy", "visibility proxy", "sun shadow", "occluder", "ray query", "surface class 3", "sky fill", "gpu origin", "acceleration structure", "bypass_proxy_sun_shadow", "bypass_proxy_surfaces"]
sources = [
  "engine/lighting/include/orbit/lighting/ProxySunShadow.hpp",
  "engine/lighting/src/ProxySunShadow.cpp",
  "engine/lighting/include/orbit/lighting/ProxySurface.hpp",
  "engine/lighting/src/ProxySurface.cpp",
  "engine/lighting/src/DirectLighting.cpp",
  "engine/lighting/tests/ProxySunShadowTests.cpp",
  "engine/lighting/tests/ProxySurfaceTests.cpp",
]
symbols = ["ProxySunShadowRenderer", "ProxySurfaceRenderer", "DirectLightingRenderer", "ProxyGpuOriginIsStale", "skyRayCount"]
invariants = [
  "Only authored proxies cast the proxy sun shadow; terrain and sky are not proxies.",
  "The shadow pass runs only when the device supports ray queries AND the target body has at least one proxy; otherwise direct lighting is unchanged.",
  "Visible proxies are rasterised (boxes as triangles, spheres ray-traced in the pixel shader) into the deferred surface buffer right after the terrain pass as surface class 3 (RigidGeometry/LocalMesh); this needs no ray-query support.",
  "Proxy surfaces ignore the radiance cache: the final gather's cache fallback and the hybrid reflections' cache fallback skip surface class 3 pixels. A cache cell is a planet-frame-aligned cube that straddles thin walls, so its lifted sample can sit on the other side of a wall. Terrain keeps using the cache.",
  "Proxy scene data (acceleration structure and primitive buffers) is float32 relative to the GPU origin it was built at; it is rebuilt with a fresh origin when the camera is more than 1.5 km from it while within 20 km of the proxies (ProxyGpuOriginIsStale). Farther away proxies are sub-pixel and left alone.",
  "Sky fill on proxy surfaces uses skyRayCount cosine-weighted rays (default 12, capped at 64 in the shader) against the other proxies and adds albedo/pi times the sky irradiance times the open fraction, on proxy surfaces only; terrain lighting is unchanged.",
  "bypass_proxy_sun_shadow turns off the sun shadow and the proxy sky fill together; bypass_proxy_surfaces returns proxies to invisible occluders.",
  "Known limit: proxy shadows do not darken terrain's own sky or bounce light; the radiance cache traces proxies for those, but only for pixels the screen-space gather leaves unresolved.",
]
related = ["/rendering/lighting/sky-cache-fill"]
depends_on = ["/rendering/lighting"]
verify = [
  "ctest -R Orbit.LightingProxySunShadow (builds ProxySunShadowTests and ProxySurfaceTests, including ProxyGpuOriginIsStale and constant packing).",
  "orbit_view_terrain_layers_set(bypass_proxy_sun_shadow=true): the proxy shadow and proxy sky fill disappear; terrain lighting must not change.",
  "Edit a proxy property: the semantic-revision rebuild applies it without a restart.",
]
verified = "b0a0de7f"

[[diagnose]]
symptom = "authored proxies cast no sun shadow at all"
steps = [
  "orbit_view_terrain_layers_get: bypass_proxy_sun_shadow must be false.",
  "Confirm the target body has at least one proxy (object.create with the Visibility Proxy type) and that the GPU supports ray queries; without either the pass does not run and direct lighting is unchanged by design.",
  "Remember terrain and sky never cast through this pass; only authored structures do.",
]
docs = ["/rendering/lighting"]

[[diagnose]]
symptom = "bands of wrongly occluded rays or shadow stripes on proxy walls"
steps = [
  "Suspect a stale GPU origin: a scene built while the camera was far away (for example a world reopened from a planet-scale view) resolves only about a metre in float32.",
  "Move to within 20 km of the proxies and more than 1.5 km from the build origin: ProxyGpuOriginIsStale should trigger a rebuild; if it does not, check the thresholds in ProxySurface.hpp and the call site in StudioViewportRenderer.cpp.",
  "The exact reflections share the same scene, so a reflection artefact with the same pattern confirms it.",
]
docs = ["/rendering/lighting"]

[[diagnose]]
symptom = "inner walls or ceilings of a proxy room pop between light and dark in cell-shaped triangles"
steps = [
  "This is the radiance-cache cell straddling a thin wall (the lifted sample sits outside); proxy surfaces must skip the cache.",
  "Verify surface class 3 pixels skip the cache fallback in both the final gather and the hybrid reflections; toggle bypass_radiance_cache to confirm the pattern disappears.",
  "Terrain should keep using the cache; do not fix this by disabling the cache globally.",
]
docs = ["/rendering/lighting/sky-cache-fill"]
+++

## Pipeline

```text
authored proxy objects (object.create, Visibility Proxy type, shape box|sphere, body-local pose)
  -> proxy scene: acceleration structure + primitive buffers (float32 about the GPU origin)
  -> ProxySunShadowRenderer (compute): 1 ray per visible pixel toward the star -> visibility texture
        + 12 cosine-weighted sky rays per proxy-surface pixel -> open fraction x sky irradiance
  -> DirectLightingRenderer: stellar term x visibility x cloud shadow; + albedo/pi sky fill on proxy surfaces
  ProxySurfaceRenderer: rasterises the proxies into the surface buffer after the terrain pass
```

Proxy surface appearance: neutral grey for Material ID 0, a stable tint per other Material ID, roughness 0.85. They
receive the direct sun, the proxy sun shadow, the screen-space gather and can bounce light in the gather.

## Hot iteration

Proxy property edits take the existing semantic-revision rebuild (no restart; primitive buffers for both passes rebuild
with it). Saving `ProxySunShadow.cpp` or `DirectLighting.cpp` takes the automatic Studio-generation handoff; the
embedded HLSL compiles at startup, so a failed shader leaves the running generation alive.
