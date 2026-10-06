+++
path = "/apps/sandbox"
title = "Orbit Sandbox (engine test application)"
kind = "subsystem"
status = "stable"
summary = "A standalone windowed application that exercises the terrain, planet map, camera, debug overlay and jobs stack without Studio, with GPU frame timing and the dev server on port 4319."
keywords = ["sandbox", "test app", "asterra sandbox", "terrain sandbox", "gpu timing", "dev server"]
sources = [
  "apps/sandbox/src/Main.cpp",
  "apps/sandbox/CMakeLists.txt",
]
symbols = []
invariants = [
  "Per-frame-in-flight GPU timestamps are only read back after the same frame-fence wait that guards reusing that slot's allocator, so no extra synchronisation is needed.",
  "It registers a second body even though it renders only Asterra so the composition root exercises a multi-body registry.",
]
related = ["/tools/dev-server", "/rendering/terrain", "/rendering/planet-map"]
depends_on = ["/foundation/core", "/foundation/jobs", "/foundation/math", "/foundation/platform", "/foundation/runtime-session", "/rendering/debug-overlay", "/rendering/free-camera", "/rendering/planet-map", "/rendering/render-graph", "/rendering/render-view", "/rendering/terrain/clipmaps", "/rendering/terrain/contracts", "/rendering/terrain/erosion", "/rendering/terrain/hydrology", "/rendering/terrain/page-cache", "/rendering/terrain/regions", "/rendering/terrain/streaming", "/rendering/water", "/tools/dev-server", "/world/fields", "/world/planet-coordinates", "/world/surface-registry", "/world/universe"]
verify = [
  "No test is registered for this module under its own name; changes are exercised through the tests of the modules that use it (see used_by).",
]
verified = "b0a0de7f"
+++


