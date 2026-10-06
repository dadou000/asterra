+++
path = "/celestial/compact-render"
title = "Compact object renderer"
kind = "subsystem"
status = "stable"
summary = "CompactObjectRenderer::Draw renders one CompactObjectDraw into a target texture: the compact object's presentation (shadow and optical radii), an optional accretion flow, the camera and the projected shadow/optical radii in pixels."
owner_module = "OrbitCelestialCompactRender"
keywords = ["compact render", "black hole render", "accretion disk", "lensing", "shadow render", "point proxy weight"]
sources = [
  "engine/celestial_compact_render/include/orbit/celestial_compact_render/CompactObjectRenderer.hpp",
  "engine/celestial_compact_render/CMakeLists.txt",
]
symbols = ["CompactObjectDraw"]
invariants = [
  "The point-proxy transition is a continuous weight (pointProxyWeight) so the same analytic optical model survives into the sub-pixel regime instead of switching renderer.",
  "The draw is fully described by CompactObjectDraw (presentation, optional accretion parameters, camera, projected radii in pixels, opacity); the renderer holds only its pipeline and no body state.",
]
related = ["/celestial/compact-objects", "/celestial/representation", "/rendering/render-view"]
depends_on = ["/celestial/compact-objects", "/rendering/render-view", "/rendering/rhi", "/rendering/shader-compiler"]
used_by = ["/editor/studio-ui"]
verify = [
  "No test is registered for this module under its own name; changes are exercised through the tests of the modules that use it (see used_by).",
]
verified = "b0a0de7f"
+++


