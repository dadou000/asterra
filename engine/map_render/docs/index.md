+++
path = "/rendering/planet-map"
title = "Planet map renderer"
kind = "subsystem"
status = "stable"
summary = "PlanetMapRenderer draws the full-screen planet map: one flat-coloured equirectangular texture per MapLayer, generated once on a background thread and uploaded once; a free function inverts the projection for click-to-teleport."
owner_module = "OrbitMapRender"
keywords = ["map", "planet map", "equirectangular", "map layer", "teleport", "overview"]
sources = [
  "engine/map_render/include/orbit/map_render/PlanetMapRenderer.hpp",
  "engine/map_render/CMakeLists.txt",
]
symbols = ["PlanetMapRenderer"]
invariants = [
  "The planet is static, so the layer textures are generated once on a background thread and uploaded to the GPU once.",
  "The UV-to-direction inverse of the equirectangular projection is a free function (a fixed mapping with no renderer state): normalized UV with origin top-left, matching the texture's row/column order.",
]
related = ["/rendering"]
depends_on = ["/foundation/core", "/foundation/math", "/rendering/rhi", "/rendering/shader-compiler", "/rendering/terrain/contracts"]
used_by = ["/apps/sandbox"]
verify = [
  "No test is registered for this module under its own name; changes are exercised through the tests of the modules that use it (see used_by).",
]
verified = "b0a0de7f"
+++


