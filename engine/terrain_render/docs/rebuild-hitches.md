+++
path = "/rendering/terrain/clipmaps/rebuild-hitches"
title = "Rebuild hitches (clipmap config changes)"
kind = "concept"
status = "stable"
owner_module = "OrbitTerrainRender"
summary = """
Multi-second main-thread stalls at fixed altitudes came from ComposeBase treating any clipmap config change as \
'rebuild everything', including GpuFieldGenerator, which recompiled its compute shader on the frame thread. A \
clipmap-only change now rebuilds just TerrainPreviewRenderer, and DxcShaderCompiler memoises compiles per process. \
What remains is a one-time cost on the first terrain build of a process."""
keywords = ["hitch", "freeze", "stall", "dxc", "shader compile", "composebase", "recreate generator", "altitude", "pipeline cache", "frame time"]
sources = [
  "engine/studio_ui/src/StudioViewportRendererBase.cpp",
  "engine/shader/dxc/include/orbit/shader/dxc/DxcShaderCompiler.hpp",
  "engine/terrain_gpu/src/GpuFieldGenerator.cpp",
  "docs/ORBIT_PERFORMANCE.md",
]
symbols = ["recreateGenerator", "ComposeBase", "DxcShaderCompiler", "GpuFieldGenerator", "QuantizedSkyObserverRadius"]
invariants = [
  "GpuFieldGenerator depends only on the planet and its terrain source; a clipmap-only config change must not rebuild it (recreateGenerator is false for it).",
  "DxcShaderCompiler memoises per process keyed by stage, shader model, flags, entry point and source text; edited sources hash differently, so live shader editing still recompiles.",
  "Nothing may compile shaders or build pipelines on the frame thread during ordinary motion; a stall of seconds at a fixed altitude means a rebuild was triggered by a config change.",
  "The sky-view table is rebuilt only when the observer altitude quantised to ~2% steps changes (QuantizedSkyObserverRadius), not every frame.",
]
related = ["/rendering/terrain/clipmaps/level-planning"]
depends_on = ["/rendering/terrain/clipmaps"]
verify = [
  "0 -> orbit -> 0 altitude sweep (933 navigate steps, 6,297 frames) shows no hitch after startup (before: nine ~3 s stalls).",
  "Micro-profiler capture (orbit_profiler_capture): no multi-ms slice under dxcompiler.dll during motion.",
]
verified = "b0a0de7f"

[[diagnose]]
symptom = "multi-second freeze when the altitude crosses a threshold"
steps = [
  "Capture a trace (orbit_profiler_capture) and look for a long main-thread slice; the original ones sat inside dxcompiler.dll under GpuFieldGenerator::GpuFieldGenerator.",
  "Find what changed at that altitude (clipmap config, sky-view table, coverage tier) and what ComposeBase rebuilt because of it.",
  "Fix by narrowing the rebuild to what the changed input actually feeds (recreateGenerator pattern), not by caching around it.",
]
docs = ["/legacy/orbit-profiler"]
+++

## Remaining known cost

The first terrain build of a process costs DXC compilation plus about 8 s in the NVIDIA driver compiling the field
compute pipeline. A persisted `VkPipelineCache` would remove it (not done).
