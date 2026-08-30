extends SceneTree
## Parse/load smoke test for dependency-heavy terrain/authoring scripts that are not
## necessarily instantiated by the active gameplay validation scene. Keeping these
## explicit prevents stale compatibility tools from remaining invisible to CI while
## still breaking the Godot editor's project-wide language-server scan.

const SCRIPT_PATHS := [
	"res://scripts/terrain/planet_height_store_procedural.gd",
	"res://scripts/world_authoring/authored_water_runtime_query.gd",
	"res://scripts/world_authoring/authored_water_runtime_spatial.gd",
	"res://scripts/world_authoring/world_authoring_runtime_host.gd",
	"res://tools/terrain_region_compiler.gd",
]


func _init() -> void:
	for script_path: String in SCRIPT_PATHS:
		var resource: Resource = load(script_path)
		if resource == null or not (resource is Script):
			push_error("SECONDARY_TERRAIN_SCRIPT_LOAD_FAILED: %s" % script_path)
			quit(1)
			return
		var script: Script = resource as Script
		if not script.can_instantiate():
			push_error("SECONDARY_TERRAIN_SCRIPT_NOT_INSTANTIABLE: %s" % script_path)
			quit(1)
			return
		print("SECONDARY_TERRAIN_SCRIPT_LOAD_OK: %s" % script_path)

	print("SECONDARY_TERRAIN_SCRIPT_STACK_OK: %d scripts" % SCRIPT_PATHS.size())
	quit(0)
