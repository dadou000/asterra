+++
path = "/world"
title = "World model"
kind = "section"
status = "stable"
summary = """
What the simulated world is made of, independent of rendering: planet coordinates and cube-sphere tiles, the universe of bodies with \
frames and transforms, generic fields, the surface registry that attaches terrain to bodies, surface composition from the semantic \
scene, authored terrain constraints, the procedural dependency graph and path networks (authored, geometry, routing)."""
keywords = ["world", "planet", "universe", "body", "fields", "surface", "paths", "procedural graph", "coordinates"]
related = ["/authoring", "/rendering/terrain"]

[routes]
"cube faces, tiles, neighbours, WorldPosition" = "planet-coordinates"
"bodies, shapes, rotation, orbits as transforms" = "universe"
"generic spatial fields, residency, resolution policy" = "fields"
"attach terrain to a body, surface coordinates" = "surface-registry"
"terrain services per body, surface material blends" = "surface-composition"
"authored terrain constraints, brushes, splines, masks" = "terrain-constraints"
"dependency graph scheduling, dirty/clean/building nodes" = "procedural-graph"
"authored path networks" = "paths"
"road/path meshes and centrelines" = "path-geometry"
"route planning" = "path-routing"
"terrain edit invalidation" = "/rendering/terrain/invalidation"
+++

Persistent world state is compact authoritative data; generated geometry, collision and caches are disposable
(`/rules/architecture`). World state never contains GPU handles.
