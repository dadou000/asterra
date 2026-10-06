+++
path = "/rendering/debug-overlay"
title = "Debug overlay (version/HUD text)"
kind = "subsystem"
status = "stable"
summary = "VersionOverlayRenderer draws the version text and stacked HUD lines (for example the F3 debug HUD) over a frame."
owner_module = "OrbitDebugRender"
keywords = ["debug", "overlay", "version", "hud", "f3", "text overlay"]
sources = [
  "engine/debug_render/include/orbit/debug_render/VersionOverlayRenderer.hpp",
  "engine/debug_render/CMakeLists.txt",
]
symbols = ["VersionOverlayConfig"]
invariants = [
  "Overlay text is sanitised and truncated; replacing it is safe to call every frame.",
]
related = ["/editor/viewport"]
depends_on = ["/foundation/core", "/rendering/rhi"]
verify = [
  "No test is registered for this module under its own name; changes are exercised through the tests of the modules that use it (see used_by).",
]
verified = "b0a0de7f"
+++


