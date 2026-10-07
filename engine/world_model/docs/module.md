+++
path = "/world/world-model"
title = "World model (capability bindings and composition)"
kind = "subsystem"
status = "stable"
summary = "Maps the semantic scene's capabilities to the runtime parameters of every celestial and surface subsystem (ResolveAtmosphereBody, cloud/ocean/ring/giant/compact/magnetosphere/small-body bindings, UniverseComposition, lighting service, primitive/proxy/light/material bindings), defines the capability schemas and hosts property provenance and the atmosphere property solver. The V0.0.6 physical property solver (mean density, rotation speed relations) was removed in 0.0.9 because nothing linked it."
owner_module = "OrbitWorldModel"
keywords = ["world model", "binding", "capability", "universe composition", "resolve body", "schema", "provenance", "composition", "semantic to runtime"]
sources = [
  "engine/world_model/include/orbit/world_model/AtmospherePropertySolver.hpp",
  "engine/world_model/include/orbit/world_model/CelestialAtmosphereBinding.hpp",
  "engine/world_model/include/orbit/world_model/CelestialCloudBinding.hpp",
  "engine/world_model/include/orbit/world_model/CelestialCompactObjectBinding.hpp",
  "engine/world_model/include/orbit/world_model/CelestialGiantBinding.hpp",
  "engine/world_model/include/orbit/world_model/CelestialLightingService.hpp",
  "engine/world_model/include/orbit/world_model/CelestialMagnetosphereBinding.hpp",
  "engine/world_model/include/orbit/world_model/CelestialOceanBinding.hpp",
  "engine/world_model/include/orbit/world_model/PropertyProvenance.hpp",
  "engine/world_model/include/orbit/world_model/PropertyProvenanceStore.hpp",
  "engine/world_model/CMakeLists.txt",
]
symbols = ["ResolveAtmosphereBody", "ResolvedCloudLayer", "ResolvedGiantAppearance", "ResolvedCompactObject", "AtmospherePropertySolver", "PropertyProvenance", "DirectBodyLighting", "CelestialLightingService"]
invariants = [
  "Semantic capabilities are the only persistent authority; runtime parameters are resolved from them (Resolve*Body), so runtime products are derived and invalidated by revision.",
  "Duplicate enabled capabilities of one kind on a body are rejected (for example Physical Scattering for the atmosphere).",
  "A stored property with no provenance record counts as Explicit + Locked, so solvers never overwrite legacy or hand-authored values; solvers return conflicts instead (/rendering/atmosphere/authoring-solver).",
  "The permanent ownership of world semantic IDs lives in Orbit::WorldModel; editor aliases exist only for compatibility.",
]
related = ["/world/universe", "/celestial", "/rendering/atmosphere/authoring-solver", "/editor/model", "/legacy/tree-history-research-v006-physical-property-solver-v1"]
depends_on = ["/authoring/commands", "/authoring/documents", "/authoring/scene", "/authoring/schema", "/celestial/compact-objects", "/celestial/giants", "/celestial/gravity", "/celestial/lighting", "/celestial/magnetosphere", "/celestial/ocean", "/celestial/orbits", "/celestial/radiometry", "/celestial/rings", "/celestial/rotation", "/celestial/small-bodies", "/celestial/stellar", "/foundation/core", "/foundation/frames", "/foundation/math", "/foundation/time", "/rendering/atmosphere", "/rendering/clouds", "/world/universe"]
used_by = ["/apps/studio", "/editor/model", "/editor/session", "/editor/studio-session", "/rendering/lighting/radiance-cache", "/rendering/volumes/fields", "/rendering/volumes/render", "/rendering/volumes/representation", "/rendering/volumes/solver", "/world/surface-composition"]
verify = [
  "ctest -R Orbit.UniverseComposition",
  "ctest -R Orbit.CelestialCapabilitySchemas",
  "ctest -R Orbit.PropertyProvenance",
  "ctest -R Orbit.AtmospherePropertySolver",
  "ctest -R Orbit.CelestialAtmosphereBinding",
  "ctest -R Orbit.CelestialCloudComposition",
  "ctest -R Orbit.AnalyticOrbitComposition",
  "ctest -R Orbit.RotationComposition",
  "ctest -R Orbit.GravityComposition",
]
verified = "db348ce94035630577b705cffe0c69c6f8a6061f"
+++


