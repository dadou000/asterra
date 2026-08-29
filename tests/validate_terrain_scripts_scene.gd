extends Node
## Scene-launched counterpart of validate_terrain_scripts.gd. Autoload globals are
## available when Godot launches a normal scene, matching the production runtime.

const SCRIPT_CHAIN := [
	"res://scripts/terrain/gpu_terrain_scatter.gd",
	"res://scripts/terrain/gpu_terrain_scatter_compact.gd",
	"res://scripts/terrain/gpu_terrain_scatter_global.gd",
	"res://scripts/terrain/spherical_geometry_clipmap.gd",
	"res://scripts/terrain/spherical_geometry_clipmap_authoritative.gd",
]

func _ready() -> void:
	for script_path: String in SCRIPT_CHAIN:
		var resource: Resource = load(script_path)
		if resource == null or not (resource is Script):
			push_error("TERRAIN_SCRIPT_LOAD_FAILED: %s" % script_path)
			get_tree().quit(1)
			return
		var script := resource as Script
		if not script.can_instantiate():
			push_error("TERRAIN_SCRIPT_NOT_INSTANTIABLE: %s" % script_path)
			get_tree().quit(1)
			return
		print("TERRAIN_SCRIPT_LOAD_OK: %s" % script_path)
	print("TERRAIN_SCRIPT_STACK_OK: %d scripts" % SCRIPT_CHAIN.size())
	get_tree().quit(0)
