+++
path = "/rendering/volumes/solver"
title = "Surface volume solver"
kind = "reference"
status = "stable"
summary = "SurfaceVolumeSolverService is the solver service for volumes near a surface (settings, debug views along a slice axis, reference cells, diagnostics)."
owner_module = "OrbitVolumeSolver"
keywords = ["volume solver", "surface volume", "slice", "debug view", "reference cell", "solver service"]
sources = [
  "engine/volume_solver/include/orbit/volume_solver/SurfaceVolumeSolver.hpp",
  "engine/volume_solver/CMakeLists.txt",
]
symbols = ["SurfaceVolumeSolverSettings"]
related = ["/rendering/volumes", "/legacy/research-v007-universal-volumetrics"]
depends_on = ["/authoring/scene", "/foundation/core", "/foundation/math", "/rendering/render-graph", "/rendering/rhi", "/rendering/shader-compiler", "/rendering/volumes/fields", "/rendering/volumes/representation", "/world/world-model"]
used_by = ["/apps/studio"]
verify = [
  "ctest -R Orbit.SurfaceVolumeSolver",
]
verified = "b0a0de7f"
+++


