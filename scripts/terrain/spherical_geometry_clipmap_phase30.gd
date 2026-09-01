extends "res://scripts/terrain/spherical_geometry_clipmap_phase29.gd"
## Phase 30 installs the sculpt-aware absolute production Shape compiler while
## retaining the validated Phase 29/Blank-aware clipmap implementation.

const PHASE30_DISPLACEMENT_RUNTIME := preload(
	"res://scripts/world_authoring/terrain_displacement_runtime_phase30.gd")


func _ensure_displacement_runtime() -> void:
	if _displacement_runtime != null and is_instance_valid(_displacement_runtime):
		if _displacement_runtime.get_script() == PHASE30_DISPLACEMENT_RUNTIME:
			return
		_displacement_runtime.remove_from_group(&"terrain_displacement_runtime")
		_displacement_runtime.queue_free()
		_displacement_runtime = null
	_displacement_runtime = PHASE30_DISPLACEMENT_RUNTIME.new() as Node
	if _displacement_runtime == null:
		return
	_displacement_runtime.name = "TerrainDisplacementRuntime"
	add_child(_displacement_runtime)
	_displacement_fingerprint = ""
