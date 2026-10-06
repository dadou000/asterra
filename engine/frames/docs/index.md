+++
path = "/foundation/frames"
title = "Reference frames (FrameGraph)"
kind = "subsystem"
status = "stable"
summary = "FrameGraph is a sparse, CPU-authoritative hierarchy of reference frames. A frame stores only its transform relative to its parent; resolving two frames walks to their lowest common ancestor instead of converting through a potentially astronomical root, which preserves local precision."
owner_module = "OrbitFrames"
keywords = ["frame", "frame graph", "reference frame", "precision", "lowest common ancestor", "coordinates", "frame point"]
sources = [
  "engine/frames/include/orbit/frames/FrameGraph.hpp",
  "engine/frames/CMakeLists.txt",
]
symbols = ["FramePoint"]
invariants = [
  "A frame stores only its transform relative to its parent.",
  "Pairwise resolution walks to the lowest common ancestor instead of converting through a potentially astronomical root, so nearby objects keep local precision.",
  "The graph is sparse and CPU-authoritative.",
]
related = ["/foundation/time", "/world/universe", "/legacy/v0-0-3-spec/6-universe-frames-and-time"]
depends_on = ["/foundation/core", "/foundation/math", "/foundation/time"]
used_by = ["/apps/studio", "/celestial/gravity", "/editor/ui-toolkit", "/rendering/render-view", "/world/fields", "/world/path-geometry", "/world/path-routing", "/world/paths", "/world/universe", "/world/world-model"]
verify = [
  "ctest -R Orbit.FrameGraph",
]
verified = "b0a0de7f"
+++


