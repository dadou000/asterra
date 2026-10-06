+++
path = "/foundation/core"
title = "Core primitives"
kind = "subsystem"
status = "stable"
summary = "Broadly reusable primitives only: fixed-width type aliases (u8..u64, i8..i64, f32, f64), Assert and Log, StrongId (typed opaque IDs, including a 128-bit form with an RFC 4122-compatible textual layout), thread naming for debuggers and profilers, and the micro-profiler (per-thread ring buffers, frame summaries, hitch capture)."
owner_module = "OrbitCore"
keywords = ["core", "types", "strong id", "assert", "log", "profiler", "thread name", "uuid", "primitives"]
sources = [
  "engine/core/include/orbit/core/Assert.hpp",
  "engine/core/include/orbit/core/Log.hpp",
  "engine/core/include/orbit/core/StrongId.hpp",
  "engine/core/include/orbit/core/ThreadName.hpp",
  "engine/core/include/orbit/core/Types.hpp",
  "engine/core/include/orbit/profiler/Profiler.hpp",
  "engine/core/CMakeLists.txt",
]
symbols = ["StrongId", "Config"]
invariants = [
  "OrbitCore contains only broadly reusable primitives: terrain helpers, vehicle math, rendering utilities or gameplay concepts do not belong here just because several files want them (/rules/architecture).",
  "StrongId values are opaque: the RFC 4122-style version/variant bit placement only makes the textual form recognisable to external tools; code must not interpret the bits.",
  "ThreadName is best effort: it does nothing where the platform has no thread names and is safe to call repeatedly; pool threads are named 'Orbit.<pool>.<index>'.",
  "Every thread that records a profiler scope gets a fixed-size ring buffer, so recording stays cheap and bounded.",
]
related = ["/rules/architecture", "/legacy/orbit-profiler"]
used_by = ["/apps/build-service", "/apps/player", "/apps/sandbox", "/apps/studio", "/authoring/commands", "/authoring/content", "/authoring/cooked-project", "/authoring/documents", "/authoring/plugins", "/authoring/scene", "/authoring/schema", "/authoring/selection", "/celestial/appearance", "/celestial/compact-objects", "/celestial/far-render", "/celestial/giants", "/celestial/globe", "/celestial/gravity", "/celestial/lighting", "/celestial/magnetosphere", "/celestial/ocean", "/celestial/orbits", "/celestial/radiometry", "/celestial/representation", "/celestial/rings", "/celestial/rotation", "/celestial/scheduler", "/celestial/small-bodies", "/celestial/stellar", "/editor/model", "/editor/reports", "/editor/studio-session", "/editor/ui-toolkit", "/foundation/frames", "/foundation/hot-reload", "/foundation/jobs", "/foundation/math", "/foundation/platform", "/foundation/platform-services", "/foundation/rpc", "/foundation/runtime-session", "/foundation/time", "/rendering/debug-overlay", "/rendering/free-camera", "/rendering/planet-map", "/rendering/post-process", "/rendering/render-graph", "/rendering/render-view", "/rendering/rhi", "/rendering/shader-compiler", "/rendering/shading", "/rendering/terrain/biomes", "/rendering/terrain/contracts", "/rendering/terrain/debug-fields", "/rendering/terrain/erosion", "/rendering/terrain/geology", "/rendering/terrain/hydrology", "/rendering/terrain/impacts", "/rendering/terrain/macro-geology", "/rendering/terrain/material-column", "/rendering/terrain/page-cache", "/rendering/terrain/regions", "/rendering/terrain/relief", "/rendering/terrain/scatter", "/rendering/terrain/streaming", "/rendering/terrain/water", "/rendering/volumes/fields", "/rendering/volumes/render", "/rendering/volumes/representation", "/rendering/volumes/solver", "/rendering/water", "/tools/dev-server", "/tools/eye-adaptation-module", "/world/fields", "/world/path-geometry", "/world/path-routing", "/world/paths", "/world/planet-coordinates", "/world/procedural-graph", "/world/surface-composition", "/world/surface-registry", "/world/terrain-constraints", "/world/universe", "/world/world-model"]
verify = [
  "ctest -R Orbit.Profiler",
  "ctest -R Orbit.TimeAndIds",
]
verified = "b0a0de7f"
+++

Most modules depend on `Core`; it depends on nothing in Orbit. The profiler's UI and RPC are described in `/legacy/orbit-profiler`.
