extends "res://scripts/terrain/spherical_geometry_clipmap_phase29.gd"
## Phase 30/31 runtime bridge.
##
## Keep the validated Phase 30 clipmap/contact behavior while installing the
## production-stage-aware displacement and material graph compilers.

const PHASE31_DISPLACEMENT_RUNTIME := preload(
	"res://scripts/world_authoring/terrain_displacement_runtime_phase31.gd")
const PHASE31_MATERIAL_RUNTIME := preload(
	"res://scripts/world_authoring/terrain_material_runtime_phase31.gd")


func _ensure_displacement_runtime() -> void:
	if _displacement_runtime != null and is_instance_valid(_displacement_runtime):
		if _displacement_runtime.get_script() == PHASE31_DISPLACEMENT_RUNTIME:
			return
		_displacement_runtime.remove_from_group(&"terrain_displacement_runtime")
		_displacement_runtime.queue_free()
		_displacement_runtime = null
	_displacement_runtime = PHASE31_DISPLACEMENT_RUNTIME.new() as Node
	if _displacement_runtime == null:
		return
	_displacement_runtime.name = "TerrainDisplacementRuntime"
	add_child(_displacement_runtime)
	_displacement_fingerprint = ""


func _ensure_material_runtime() -> void:
	if _material_runtime != null and is_instance_valid(_material_runtime):
		if _material_runtime.get_script() == PHASE31_MATERIAL_RUNTIME:
			return
		_material_runtime.remove_from_group(&"terrain_material_runtime")
		_material_runtime.queue_free()
		_material_runtime = null
	_material_runtime = PHASE31_MATERIAL_RUNTIME.new() as Node
	if _material_runtime == null:
		return
	_material_runtime.name = "TerrainMaterialRuntime"
	add_child(_material_runtime)
	_material_fingerprint = ""
