+++
path = "/celestial/magnetosphere-render"
title = "Aurora renderer"
kind = "subsystem"
status = "stable"
summary = "GpuAuroraMeshProduct uploads the CPU AuroraMeshProduct (vertex and index buffers, reference radius, fingerprint) and AuroraRenderer::Draw renders it into a target for a camera with an intensity scale."
owner_module = "OrbitCelestialMagnetosphereRender"
keywords = ["aurora render", "aurora mesh", "gpu aurora", "intensity scale", "fingerprint"]
sources = [
  "engine/celestial_magnetosphere_render/include/orbit/celestial_magnetosphere_render/AuroraRenderer.hpp",
  "engine/celestial_magnetosphere_render/CMakeLists.txt",
]
symbols = ["GpuAuroraMeshProduct"]
invariants = [
  "The GPU mesh carries the CPU product's fingerprint and reference radius, so a rebuilt mesh is detected by fingerprint change; the product itself is derived and disposable (/celestial/magnetosphere).",
  "Draw takes an intensityScale (default 1) so presentation brightness can be changed without rebuilding the mesh.",
]
related = ["/celestial/magnetosphere", "/rendering/render-view"]
depends_on = ["/celestial/magnetosphere", "/rendering/render-view", "/rendering/rhi", "/rendering/shader-compiler"]
used_by = ["/editor/studio-ui"]
verify = [
  "No test is registered for this module under its own name; changes are exercised through the tests of the modules that use it (see used_by).",
]
verified = "b0a0de7f"
+++


