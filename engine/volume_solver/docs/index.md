+++
path = "/rendering/volumes/solver"
title = "Surface volume solver"
kind = "subsystem"
status = "stable"
summary = "SurfaceVolumeSolverService runs the near-surface volume solve with explicit run controls (live, paused, single step, reset, follow camera), a per-frame GPU budget, LOD thresholds, dissipation and source scaling, debug views (density, velocity, field slice) and per-frame diagnostics."
owner_module = "OrbitVolumeSolver"
keywords = ["volume solver", "surface volume", "slice", "debug view", "reference cell", "solver service", "gpu budget", "dissipation", "lod distance"]
sources = [
  "engine/volume_solver/include/orbit/volume_solver/SurfaceVolumeSolver.hpp",
  "engine/volume_solver/CMakeLists.txt",
]
symbols = ["SurfaceVolumeSolverSettings"]
invariants = [
  "Defaults are explicit settings, not constants: time step 1/60 s, scalar dissipation 0.08/s, velocity dissipation 0.04/s, 1 iteration per frame, GPU budget 2 ms.",
  "LOD is by distance and projected size with hysteresis: live inside 120 m or 96 px, passive out to 1200 m or 12 px, hysteresis fraction 0.12; the ray-march uses 24 steps coarse and 8 passive.",
  "Run state is explicit: live, paused, singleStepRequested, resetRequested and followCamera are separate flags; diagnostics report eligibility, whether it stepped or reset this frame, iterations, solved scalar channels, source/effector counts, scratch bytes and GPU milliseconds with a validity flag.",
  "A CPU reference cell model (SurfaceVolumeReferenceConfig, LocalVolumeReferenceCell) exists next to the GPU solve for deterministic tests.",
]
related = ["/rendering/volumes", "/rendering/volumes/representation", "/legacy/tree-history-research-v007-universal-volumetrics"]
depends_on = ["/authoring/scene", "/foundation/core", "/foundation/math", "/rendering/render-graph", "/rendering/rhi", "/rendering/shader-compiler", "/rendering/volumes/fields", "/rendering/volumes/representation", "/world/world-model"]
used_by = ["/apps/studio", "/editor/studio-ui"]
verify = [
  "ctest -R Orbit.SurfaceVolumeSolver",
]
verified = "db348ce94035630577b705cffe0c69c6f8a6061f"
+++


