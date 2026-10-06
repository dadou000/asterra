+++
path = "/world/paths"
title = "Path networks (authored)"
kind = "subsystem"
status = "stable"
summary = "Authoring model for procedural path networks: PathNetworkRecord with nodes and edges anchored to frame points, body surfaces or entity sockets, edge modes with handle vectors, profiles (PathProfile) and evaluation of anchors at an explicit simulation time."
owner_module = "OrbitPaths"
keywords = ["path", "network", "road", "edge", "anchor", "profile", "handle", "socket", "bezier"]
sources = [
  "engine/paths/include/orbit/paths/PathEvaluation.hpp",
  "engine/paths/include/orbit/paths/PathNetwork.hpp",
  "engine/paths/include/orbit/paths/PathProfile.hpp",
  "engine/paths/CMakeLists.txt",
]
symbols = ["FramePointAnchor", "PathProfile"]
invariants = [
  "An anchor is resolved into a target frame at an explicit simulation time; surface anchors use the body's reference shape and frame, entity/socket anchors are delegated to the owning runtime or entity system.",
  "Surface anchor components are latitude radians, longitude radians, offset metres; edge handles are endpoint-relative vectors in the edge evaluation frame.",
  "The semantic network is authoritative; centrelines, meshes and routes are derived (/world/path-geometry, /world/path-routing).",
]
related = ["/world/path-geometry", "/world/path-routing", "/legacy/v0-0-3-spec/20-procedural-path-network-system"]
depends_on = ["/authoring/commands", "/authoring/documents", "/authoring/scene", "/authoring/schema", "/foundation/core", "/foundation/frames", "/foundation/math", "/world/universe"]
used_by = ["/world/path-geometry", "/world/path-routing"]
verify = [
  "ctest -R Orbit.PathNetwork",
  "ctest -R Orbit.PathProfile",
  "ctest -R Orbit.PathEvaluation",
]
verified = "b0a0de7f"
+++


