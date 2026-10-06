+++
path = "/celestial/compact-render"
title = "Compact object renderer"
kind = "reference"
status = "stable"
summary = "CompactObjectRenderer draws compact objects (shadow and accretion flow) from CompactObjectDraw requests."
owner_module = "OrbitCelestialCompactRender"
keywords = ["compact render", "black hole render", "accretion disk", "lensing", "shadow render"]
sources = [
  "engine/celestial_compact_render/include/orbit/celestial_compact_render/CompactObjectRenderer.hpp",
  "engine/celestial_compact_render/CMakeLists.txt",
]
symbols = ["CompactObjectDraw"]
related = ["/celestial/compact-objects", "/rendering/render-view"]
depends_on = ["/celestial/compact-objects", "/rendering/render-view", "/rendering/rhi", "/rendering/shader-compiler"]
verify = [
  "No test is registered for this module under its own name; changes are exercised through the tests of the modules that use it (see used_by).",
]
verified = "b0a0de7f"
+++


