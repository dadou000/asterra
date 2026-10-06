+++
path = "/world/path-geometry"
title = "Path geometry (derived centrelines and meshes)"
kind = "subsystem"
status = "stable"
summary = "Derives PathCenterline samples, stations, lane references and PathMesh vertices from an authored network (PathSourceRequest, PathBuildOptions)."
owner_module = "OrbitPathGeometry"
keywords = ["path geometry", "centerline", "mesh", "lane", "station", "derived", "road mesh"]
sources = [
  "engine/path_geometry/include/orbit/path_geometry/PathDerived.hpp",
  "engine/path_geometry/include/orbit/path_geometry/PathSource.hpp",
  "engine/path_geometry/CMakeLists.txt",
]
symbols = ["PathCenterlineSample", "PathSourceRequest"]
invariants = [
  "Generated path geometry is a disposable cache; the semantic path network stays the source of truth (/rules/architecture, rule 10).",
]
related = ["/world/paths", "/rules/architecture"]
depends_on = ["/authoring/scene", "/foundation/core", "/foundation/frames", "/foundation/math", "/foundation/time", "/world/path-routing", "/world/paths", "/world/universe"]
verify = [
  "ctest -R Orbit.PathGeometry",
  "ctest -R Orbit.PathSource",
]
verified = "b0a0de7f"
+++


