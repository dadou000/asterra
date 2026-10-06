+++
path = "/celestial/magnetosphere-render"
title = "Aurora renderer"
kind = "reference"
status = "stable"
summary = "AuroraRenderer draws the GPU aurora mesh produced by the magnetosphere module."
owner_module = "OrbitCelestialMagnetosphereRender"
keywords = ["aurora render", "aurora mesh", "gpu aurora"]
sources = [
  "engine/celestial_magnetosphere_render/include/orbit/celestial_magnetosphere_render/AuroraRenderer.hpp",
  "engine/celestial_magnetosphere_render/CMakeLists.txt",
]
symbols = ["GpuAuroraMeshProduct"]
related = ["/celestial/magnetosphere", "/rendering/render-view"]
depends_on = ["/celestial/magnetosphere", "/rendering/render-view", "/rendering/rhi", "/rendering/shader-compiler"]
verify = [
  "No test is registered for this module under its own name; changes are exercised through the tests of the modules that use it (see used_by).",
]
verified = "b0a0de7f"
+++


