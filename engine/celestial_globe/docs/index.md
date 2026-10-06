+++
path = "/celestial/globe"
title = "Macro orbital globe and adaptive patches"
kind = "subsystem"
status = "stable"
summary = "The macro-displaced orbital globe: a cube-sphere derived from the body's terrain source, now an adaptive orbital patch hierarchy (PlanetPatchSelector, per-patch meshes and appearance) with the legacy whole-globe product kept as a stable seed/fallback."
owner_module = "OrbitCelestialGlobe"
keywords = ["globe", "macro globe", "orbital patch", "cube sphere", "patch hierarchy", "lod bias", "seed", "fallback", "orbit view"]
sources = [
  "engine/celestial_globe/include/orbit/celestial_globe/MacroGlobe.hpp",
  "engine/celestial_globe/include/orbit/celestial_globe/PlanetPatchHierarchy.hpp",
  "engine/celestial_globe/CMakeLists.txt",
]
symbols = ["MacroGlobeResolution", "PlanetPatchId"]
invariants = [
  "The globe is a disposable derived representation of the existing terrain authority; it owns no terrain, surface identity or body shape and never persists sampled elevation as authoring state.",
  "If terrain authority changes, TerrainSource::Revision() changes and the derived globe fingerprint changes.",
  "Adaptive patches carry the actual orbital LOD: the global product is only a stable seed/fallback because rebuilding the whole cube-face resolution at screen-size thresholds (33/65/129/257/513) is incompatible with the patch hierarchy; explicit fixed resolutions remain for deterministic tests and tools.",
  "Orbital patch detail bias is in stops: +1 doubles the resolution the selector asks for (half the target cell size in pixels), -1 halves it.",
  "The old renderer stays compiled privately so the adaptive wrapper reuses its proven Vulkan pipelines for each independently resident patch; the wrapper registers the terrain authority and keeps shared ownership of the source so background patch builds cannot outlive it.",
  "With full_clipmap on (default) no globe is drawn at any altitude; the globe path is used when it is off (/rendering/terrain/clipmaps).",
]
related = ["/celestial/representation", "/celestial/appearance", "/rendering/terrain/clipmaps", "/legacy/research-v006-macro-orbital-globe", "/legacy/planet-patch1"]
depends_on = ["/celestial/appearance", "/foundation/core", "/foundation/math", "/rendering/render-view", "/rendering/rhi", "/rendering/shader-compiler", "/rendering/terrain/contracts", "/world/universe"]
used_by = ["/editor/studio-ui"]
verify = [
  "ctest -R Orbit.MacroGlobe",
  "ctest -R Orbit.PlanetPatchHierarchy",
]
verified = "b0a0de7f"
+++


