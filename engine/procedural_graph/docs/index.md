+++
path = "/world/procedural-graph"
title = "Procedural dependency graph"
kind = "subsystem"
status = "stable"
summary = "ProceduralGraph is the CPU-authoritative dependency graph that schedules derived builds: nodes are Clean, Dirty, Building or Failed, builds run on the CPU or issue GPU work through an injected service, and stale generations are rejected."
owner_module = "OrbitProceduralGraph"
keywords = ["procedural graph", "dependency graph", "node", "invalidate", "build", "generation", "stale", "backend", "dirty"]
sources = [
  "engine/procedural_graph/include/orbit/procedural_graph/ProceduralGraph.hpp",
  "engine/procedural_graph/CMakeLists.txt",
]
symbols = ["BuildContext"]
invariants = [
  "The graph is a sparse CPU scheduler and revision authority; a GPU node's build function issues GPU work through an injected GPU service, the graph does not touch the GPU itself.",
  "Async builds consume an immutable snapshot captured only after all dependencies are Clean; worker threads never read graph state.",
  "Invalidate marks a node's own configuration changed and invalidates all derived descendants; the node keeps its previous product until a newer build commits.",
  "RemoveNodes removes a closed subgraph only when none of its nodes are building and no surviving node depends on it, and returns false instead of blocking.",
]
related = ["/rendering/terrain/invalidation", "/foundation/jobs", "/legacy/v0-0-3-spec/9-procedural-dependency-graph"]
depends_on = ["/foundation/core", "/foundation/jobs"]
verify = [
  "ctest -R Orbit.ProceduralGraph",
]
verified = "b0a0de7f"
+++

Terrain pages use it through TerrainDependencyGraph (`/rendering/terrain/invalidation`).
