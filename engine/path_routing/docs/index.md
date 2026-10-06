+++
path = "/world/path-routing"
title = "Path routing (planner)"
kind = "subsystem"
status = "stable"
summary = "RoutePlanner searches routes over a body's surface with configurable cost sources and domains (terrain-aware, or a reference-shape domain for bodies without terrain), returning RouteResult points with staleness handling."
owner_module = "OrbitPathRouting"
keywords = ["route", "routing", "planner", "search", "cost", "domain", "terrain aware", "stale"]
sources = [
  "engine/path_routing/include/orbit/path_routing/RouteDomains.hpp",
  "engine/path_routing/include/orbit/path_routing/RoutePlanner.hpp",
  "engine/path_routing/CMakeLists.txt",
]
symbols = ["RouteProjectedPoint"]
invariants = [
  "For bodies without a terrain capability, candidates are snapped to the body's sphere or ellipsoid reference shape (reference-shape domain).",
]
related = ["/world/paths", "/world/surface-registry"]
depends_on = ["/foundation/core", "/foundation/frames", "/foundation/jobs", "/foundation/math", "/foundation/time", "/rendering/terrain", "/world/fields", "/world/paths", "/world/surface-registry", "/world/universe"]
used_by = ["/world/path-geometry"]
verify = [
  "ctest -R Orbit.PathRouting",
  "ctest -R Orbit.PathRoutingStale",
]
verified = "b0a0de7f"
+++


