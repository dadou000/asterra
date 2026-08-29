extends SceneTree
## Parse/load smoke test for dependency-heavy terrain scripts.
##
## Running these through a tiny SceneTree is more representative than executing a
## Node3D script directly with --script/--check-only, which asks Godot to treat the
## target itself as the MainLoop and can fail to resolve its inheritance chain.

const SCRIPT_CHAIN := [
	"res://scripts/terrain/gpu_terrain_scatter.gd",
	"res://scripts/terrain/gpu_terrain_scatter_compact.gd",
	"res://scripts/terrain/gpu_terrain_scatter_global.gd",
	"res://scripts/terrain/spherical_geometry_clipmap.gd",
	"res://scripts/terrain/spherical_geometry_clipmap_authoritative.gd",
]


func _init() -> void:
	for script_path: String in SCRIPT_CHAIN:
		var resource: Resource = load(script_path)
		if resource == null or not (resource is Script):
			push_error("TERRAIN_SCRIPT_LOAD_FAILED: %s" % script_path)
			quit(1)
			return
		var script := resource as Script
		if not script.can_instantiate():
			push_error("TERRAIN_SCRIPT_NOT_INSTANTIABLE: %s" % script_path)
			quit(1)
			return
		print("TERRAIN_SCRIPT_LOAD_OK: %s" % script_path)

	print("TERRAIN_SCRIPT_STACK_OK: %d scripts" % SCRIPT_CHAIN.size())
	quit(0)
