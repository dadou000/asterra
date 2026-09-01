extends "res://scripts/terrain/spherical_geometry_clipmap_blankaware.gd"
## Phase 29 terrain runtime bridge.
##
## Keep the mature Blank/Procedural clipmap ownership from blankaware, but install
## the Phase 29 displacement compiler whose production graphs use absolute-height
## semantics. Everything else remains inherited from the validated terrain stack.

const PHASE29_DISPLACEMENT_RUNTIME := preload(
	"res://scripts/world_authoring/terrain_displacement_runtime_phase29.gd")


func _ensure_displacement_runtime() -> void:
	if _displacement_runtime != null and is_instance_valid(_displacement_runtime):
		# Replace an older runtime if this script was hot-reloaded into an existing
		# editor session.
		if _displacement_runtime.get_script() == PHASE29_DISPLACEMENT_RUNTIME:
			return
		_displacement_runtime.remove_from_group(&"terrain_displacement_runtime")
		_displacement_runtime.queue_free()
		_displacement_runtime = null
	_displacement_runtime = PHASE29_DISPLACEMENT_RUNTIME.new() as Node
	if _displacement_runtime == null:
		return
	_displacement_runtime.name = "TerrainDisplacementRuntime"
	add_child(_displacement_runtime)
	_displacement_fingerprint = ""
