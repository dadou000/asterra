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
used_by = ["/apps/studio", "/editor/studio-session", "/editor/ui-toolkit"]
verify = [
  "ctest -R Orbit.PathGeometry",
  "ctest -R Orbit.PathSource",
]
verified = "b0a0de7f"
+++



## Canonical road / rail specification

The complete road, railway and supported-construction design contract lives at **`/world/roads-railways`** (source: `docs/tree/world/roads-railways.md`), including alignment, earthworks, bridge/tunnel support, vehicles aboard moving ships, Studio authoring, traffic and validation. This module implements only a portion of the complete contract; do not infer feature completion from source presence.
