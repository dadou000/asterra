+++
path = "/editor/model"
title = "Editor model (explorer, inspector, command surfaces, recipes)"
kind = "subsystem"
status = "stable"
summary = "The UI-independent authoring model Studio and plugins share: ExplorerModel and InspectorModel over the semantic scene, the authoring command set, CommandSurfaceRegistry (which commands show where), ShortcutRegistry, OutputLog, celestial/surface/system-view models and CelestialRecipeService (star, rocky planet, moon and seeded-system recipes)."
owner_module = "OrbitEditorModel"
keywords = ["editor model", "explorer", "inspector", "command surface", "shortcut", "recipe", "star recipe", "rocky planet", "seeded system", "output log", "authoring commands", "provenance"]
sources = [
  "engine/editor_model/include/orbit/editor_model/AuthoringCommands.hpp",
  "engine/editor_model/include/orbit/editor_model/BuiltinSchemas.hpp",
  "engine/editor_model/include/orbit/editor_model/CelestialAuthoringModel.hpp",
  "engine/editor_model/include/orbit/editor_model/CelestialRecipeService.hpp",
  "engine/editor_model/include/orbit/editor_model/CommandSurfaces.hpp",
  "engine/editor_model/include/orbit/editor_model/ExplorerModel.hpp",
  "engine/editor_model/include/orbit/editor_model/InspectorModel.hpp",
  "engine/editor_model/include/orbit/editor_model/OutputLog.hpp",
  "engine/editor_model/include/orbit/editor_model/PlanetSurface.hpp",
  "engine/editor_model/include/orbit/editor_model/ShortcutRegistry.hpp",
  "engine/editor_model/include/orbit/editor_model/SurfaceAuthoringModel.hpp",
  "engine/editor_model/include/orbit/editor_model/SystemViewModel.hpp",
  "engine/editor_model/CMakeLists.txt",
]
symbols = ["CelestialDiagnostic", "StarRecipe", "PresentedCommand", "ExplorerModel", "InspectedProperty", "OutputEntry", "ShortcutChord", "SurfaceAuthoringSelection"]
invariants = [
  "editor_model does not depend on the UI toolkit (engine/editor_ui): it is the shared model panels render and plugins reuse; every mutation goes through the command layer (/authoring/commands).",
  "Material-specific commands stay in the same shared command registry but in their own translation unit so the core authoring command implementation stays focused on hierarchy/path operations.",
  "BuiltinSchemas keeps compatibility aliases for existing editor/plugin code while the permanent ownership of world semantic IDs lives in Orbit::WorldModel.",
  "Recipes create ordinary semantic objects inside one transaction (for example rocky-planet recipes also run the atmosphere solver); they never create hidden runtime types.",
]
related = ["/authoring/commands", "/world/world-model", "/editor/studio-session", "/rendering/atmosphere/authoring-solver", "/rules/placement"]
depends_on = ["/authoring/commands", "/authoring/documents", "/authoring/scene", "/authoring/schema", "/authoring/selection", "/foundation/core", "/foundation/platform", "/rendering/terrain/biomes", "/world/paths", "/world/surface-composition", "/world/terrain-constraints", "/world/world-model"]
used_by = ["/apps/studio", "/authoring/plugins", "/editor/session", "/editor/studio-session", "/editor/studio-ui"]
verify = [
  "ctest -R Orbit.EditorModel",
  "ctest -R Orbit.ExplorerInspector",
  "ctest -R Orbit.CommandSurfaces",
  "ctest -R Orbit.TerrainAuthoringCommand",
  "ctest -R Orbit.VolumeAuthoringCommand",
  "ctest -R Orbit.SurfaceAuthoringModel",
  "ctest -R Orbit.RockyPlanetAuthoring",
  "ctest -R Orbit.CelestialAuthoringModel",
  "ctest -R Orbit.SystemViewModel",
  "ctest -R Orbit.CelestialRecipeService",
  "ctest -R Orbit.InspectorProvenance",
]
verified = "b0a0de7f"
+++


