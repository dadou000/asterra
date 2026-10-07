+++
path = "/rendering/render-graph"
title = "Render graph and GPU pass timing"
kind = "subsystem"
status = "stable"
summary = "RenderGraph records passes with declared texture and buffer uses, validates hazards, topologically orders them and executes; GpuPassTimer writes a GPU timestamp after every pass so the CPU profiler can show per-pass GPU time."
owner_module = "OrbitRenderGraph"
keywords = ["render graph", "pass", "hazard", "barrier", "texture handle", "import", "gpu pass timer", "per pass gpu time", "compile", "execute"]
sources = [
  "engine/render_graph/include/orbit/render_graph/GpuPassTimer.hpp",
  "engine/render_graph/include/orbit/render_graph/RenderGraph.hpp",
  "engine/render_graph/CMakeLists.txt",
]
symbols = ["GpuPassTiming", "TextureHandle"]
invariants = [
  "All passes share one hazard authority: reimporting the same resource returns its existing handle and the first import establishes its state.",
  "Compile validates hazards and topologically orders passes; Execute compiles automatically when necessary.",
  "GPU pass timestamps are BOTTOM_OF_PIPE: the gap between two neighbours is the GPU time of the pass between them, including time stuck behind earlier work.",
  "Call order per frame slot: Resolve(slot) once the slot's fence has completed; results are published on the profiler's 'GPU passes' and 'GPU frames' lanes.",
]
related = ["/rendering/rhi", "/rendering/render-view", "/legacy/orbit-profiler"]
depends_on = ["/foundation/core", "/rendering/rhi"]
used_by = ["/apps/studio", "/editor/studio-ui", "/rendering/render-view", "/rendering/volumes/fields", "/rendering/volumes/render", "/rendering/volumes/solver"]
verify = [
  "ctest -R Orbit.RenderGraph",
  "ctest -R Orbit.RenderGraphBuffers",
]
verified = "b0a0de7f"
+++


