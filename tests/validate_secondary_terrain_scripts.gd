extends Node
## Parse/load smoke test for dependency-heavy terrain/authoring scripts that are not
## necessarily instantiated by the active gameplay validation scene. This test is
## launched through a .tscn so project autoload globals resolve exactly as they do
## in the editor/runtime instead of using --script MainLoop semantics.

const SCRIPT_PATHS := [
	"res://scripts/terrain/planet_height_store_procedural.gd",
	"res://scripts/world_authoring/authored_water_runtime_query.gd",
	"res://scripts/world_authoring/authored_water_runtime_spatial.gd",
	"res://scripts/world_authoring/world_authoring_runtime_host.gd",
	"res://tools/terrain_region_compiler.gd",
]


func _ready() -> void:
	for script_path: String in SCRIPT_PATHS:
		var resource: Resource = load(script_path)
		if resource == null or not (resource is Script):
			_fail("SECONDARY_TERRAIN_SCRIPT_LOAD_FAILED: %s" % script_path)
			return
		var script: Script = resource as Script
		if not script.can_instantiate():
			_fail("SECONDARY_TERRAIN_SCRIPT_NOT_INSTANTIABLE: %s" % script_path)
			return
		print("SECONDARY_TERRAIN_SCRIPT_LOAD_OK: %s" % script_path)

	print("SECONDARY_TERRAIN_SCRIPT_STACK_OK: %d scripts" % SCRIPT_PATHS.size())
	get_tree().quit(0)


func _fail(message: String) -> void:
	push_error(message)
	get_tree().quit(1)
