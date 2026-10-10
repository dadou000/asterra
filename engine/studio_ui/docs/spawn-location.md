+++
path = "/editor/spawn-location"
title = "Spawn Location: where the camera lands when a project opens"
kind = "subsystem"
status = "stable"
summary = """
A Spawn Location (world_model::kSpawnLocationType) is a world object under a celestial body that says where Studio puts the \
camera when the project opens: a body-local position, eye height above the terrain, yaw and pitch. StudioSpawn.cpp lists \
them, moves the camera (GoToSpawn) and registers spawn.list / spawn.go / spawn.set_here; StudioViewContinuity applies the \
project spawn instead of the remembered camera."""
owner_module = "OrbitStudioUi"
keywords = ["spawn", "spawn location", "start position", "camera spawn", "project open camera", "spawn.go", "spawn.set_here"]
sources = [
  "engine/studio_ui/src/StudioSpawn.cpp",
  "engine/studio_ui/include/orbit/studio_ui/StudioSpawn.hpp",
  "engine/studio_ui/src/StudioViewContinuity.cpp",
  "engine/world_model/include/orbit/world_model/WorldSchemas.hpp",
]
symbols = ["StudioSpawnLocation", "ListSpawnLocations", "FindProjectSpawn", "GoToSpawn", "RegisterStudioSpawnRpc", "kSpawnLocationType"]
invariants = [
  "The project spawn is the first ENABLED Spawn Location in depth-first object order. When the project has one, StudioViewContinuity::TryRestore applies it and ignores the remembered pose (the remembered simulation time still applies); a project without one keeps the old remembered-camera behaviour.",
  "GoToSpawn never edits the world. A view looking at another body gets the spawn's body selected and focused (the active body follows the selection) and the call returns false until that body's terrain runtime is current, so callers retry per frame until the restore grace period ends.",
  "The camera lands on the terrain below the spawn position (FocusTerrainDirection, the double-click vantage) and is then lowered to the authored Eye Height above the terrain; the terrain navigation's own ground clearance stays a floor. Position is body-local (planet-fixed) like a Visibility Proxy's, so only its direction matters; yaw/pitch are degrees as in viewport.fly_to.",
  "spawn.set_here is one undoable transaction that updates the project spawn when it sits under the view's target body, else creates a new Spawn Location there.",
]
related = ["/editor/studio-ui", "/editor/mcp-rpc"]
depends_on = ["/editor/studio-ui", "/world/world-model"]
used_by = ["/editor/studio-ui"]
verify = [
  "ctest -R Orbit.StudioViewContinuity",
  "spawn.set_here, restart Studio, and confirm pose.get shows the camera at the stored position.",
]
verified = "022bc07b"

[routes]
"camera on project open, remembered pose, StudioView.ini" = "/editor/studio-ui"

[[diagnose]]
symptom = "project opens at the old camera instead of the Spawn Location"
steps = [
  "spawn.list: project_spawn must be non-null; a disabled Spawn Location is skipped.",
  "The spawn must be a child of a celestial body that has a terrain surface; a body without terrain never reports a current terrain runtime and the restore gives up after StudioViewContinuity::kRestoreGrace.",
]
docs = ["/editor/studio-ui"]
