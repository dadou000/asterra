+++
path = "/celestial/representation"
title = "Representation resolver (production surface to point proxy)"
kind = "subsystem"
status = "stable"
summary = "Chooses how a body is drawn from measurable view error, not raw altitude: Production Surface / toroidal clipmaps -> macro-displaced globe -> smooth globe -> analytic or cached disc impostor -> point/stellar point proxy, with quality scaling, hysteresis and an overlap blend weight toward the next lower fidelity."
owner_module = "OrbitCelestialRepresentation"
keywords = ["representation", "lod", "impostor", "point proxy", "hysteresis", "projected error", "ladder", "globe transition", "blend"]
sources = [
  "engine/celestial_representation/include/orbit/celestial_representation/RepresentationResolver.hpp",
  "engine/celestial_representation/include/orbit/celestial_representation/RepresentationTracker.hpp",
  "engine/celestial_representation/CMakeLists.txt",
]
symbols = ["FeatureRequirements", "RepresentationTracker"]
invariants = [
  "A body has one semantic authority and possibly several render representations; no representation ever becomes authoritative and no second body hierarchy exists.",
  "Selection is driven by projected radius (asin(radius/distance) / vertical FOV * viewport height) and projected geometric error (projectedRadiusPx * amplitude / bodyRadius), so a telephoto view keeps richer representations farther away than a wide view.",
  "The production surface stays selected while unresolved production-detail error exceeds its threshold; the macro globe while macro displacement still shows error; then smooth globe, impostor and point by apparent radius.",
  "qualityScale > 1 keeps richer representations longer; hysteresis retains the previous representation inside a threshold band and blendToLower reports an overlap weight so renderers can cross-fade instead of hard-switching; RepresentationTracker keeps previous state per stable subject.",
  "Feature requirements are runtime capabilities, not body classes: complex far appearance picks the cached disc, a radiative emitter picks the stellar point proxy when sub-pixel.",
  "In Studio the full clipmap (full_clipmap, default on) overrides this ladder and draws the production surface from ground to orbit; the ladder applies when full_clipmap is off (/rendering/terrain/clipmaps).",
]
related = ["/celestial/globe", "/celestial/far-render", "/rendering/terrain/clipmaps", "/legacy/research-v006-representation-resolver", "/legacy/research-v006-surface-globe-transition"]
depends_on = ["/foundation/core"]
used_by = ["/celestial/compact-objects", "/celestial/far-render", "/editor/studio-session"]
verify = [
  "ctest -R Orbit.CelestialRepresentation",
]
verified = "b0a0de7f"
+++


