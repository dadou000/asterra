+++
path = "/authoring/content"
title = "Content, assets and derived data"
kind = "subsystem"
status = "stable"
summary = "ContentService indexes project Content mounts and owns asset kinds (including shading shaders and shader materials), AssetPipeline runs importers through the ImporterRegistry, DerivedDataCache stores immutable derived artifacts, ThumbnailService renders previews and RuntimeTexture is the platform-neutral texture container."
owner_module = "OrbitContent"
keywords = ["content", "asset", "import", "ddc", "derived data cache", "material", "decal", "thumbnail", "runtime texture", "orbitmaterial", "orbitdecal", "cook"]
sources = [
  "engine/content/include/orbit/content/AssetPipeline.hpp",
  "engine/content/include/orbit/content/ContentHash.hpp",
  "engine/content/include/orbit/content/ContentService.hpp",
  "engine/content/include/orbit/content/DerivedDataCache.hpp",
  "engine/content/include/orbit/content/RuntimeTexture.hpp",
  "engine/content/include/orbit/content/ThumbnailService.hpp",
  "engine/content/CMakeLists.txt",
]
symbols = ["ImportArtifact", "ContentHash", "ShaderMaterialParameter", "DerivedDataCache", "RuntimeTexture", "ThumbnailRequest"]
invariants = [
  "Asset IDs derive from normalised project-relative paths and stay stable across rescans.",
  "Importing does not replace or mutate project authority; the source asset in Content stays canonical and derived artifacts live in the DerivedDataCache.",
  "Derived artifacts are immutable: concurrent writers of the same key converge on the same path and the first completed rename wins.",
  "The cook path used by BuildService preserves the asset's canonical .orbitimport.toml settings so editor imports and headless cooks share exactly the same DDC key semantics.",
  "A material instance stores only overrides and its base material stays immutable; ambiguous duplicate PBR channels are rejected rather than guessed.",
  "Decals are semantic .orbitdecal assets whose texture dependency is tracked through the same DDC dependency manifest as materials.",
  "A ShadingShader (*.shade.hlsl) holds the Shade() contract function; it is not a standalone HLSL stage, needs no .orbitshader.toml sidecar and is not cooked as a Shader.",
]
related = ["/legacy/orbit-shading", "/legacy/v0-0-3-spec/17-asset-database-and-material-service", "/authoring/content-wic"]
depends_on = ["/foundation/core", "/rules/hot-iteration"]
used_by = ["/authoring/content-wic", "/rendering/shading"]
verify = [
  "ctest -R Orbit.Content",
  "ctest -R Orbit.AssetPipeline",
  "ctest -R Orbit.RuntimeTexture",
]
verified = "b0a0de7f"
+++


