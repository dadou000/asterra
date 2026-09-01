extends "res://scripts/terrain/spherical_geometry_clipmap_phase30.gd"
## Phase 32 runtime bridge. The production clipmap/topology/cache remain unchanged;
## only the authoring runtimes are upgraded so serialized graph settings bind back
## into the exact renderer stages they describe.

const PHASE32_DISPLACEMENT_RUNTIME := preload(
	"res://scripts/world_authoring/terrain_displacement_runtime_phase32.gd")
const PHASE32_MATERIAL_RUNTIME := preload(
	"res://scripts/world_authoring/terrain_material_runtime_phase32.gd")


func _process(dt: float) -> void:
	super._process(dt)
	# The production parent refreshes a few debug/material uniforms during its own
	# process pass. Re-apply graph-owned settings afterward so the graph is the final
	# authority for exposed production controls.
	_bind_phase32_production_controls()


func _ensure_displacement_runtime() -> void:
	if _displacement_runtime != null and is_instance_valid(_displacement_runtime):
		if _displacement_runtime.get_script() == PHASE32_DISPLACEMENT_RUNTIME:
			return
		_displacement_runtime.remove_from_group(&"terrain_displacement_runtime")
		_displacement_runtime.queue_free()
		_displacement_runtime = null
	_displacement_runtime = PHASE32_DISPLACEMENT_RUNTIME.new() as Node
	if _displacement_runtime == null:
		return
	_displacement_runtime.name = "TerrainDisplacementRuntime"
	add_child(_displacement_runtime)
	_displacement_fingerprint = ""


func _ensure_material_runtime() -> void:
	if _material_runtime != null and is_instance_valid(_material_runtime):
		if _material_runtime.get_script() == PHASE32_MATERIAL_RUNTIME:
			return
		_material_runtime.remove_from_group(&"terrain_material_runtime")
		_material_runtime.queue_free()
		_material_runtime = null
	_material_runtime = PHASE32_MATERIAL_RUNTIME.new() as Node
	if _material_runtime == null:
		return
	_material_runtime.name = "TerrainMaterialRuntime"
	add_child(_material_runtime)
	_material_fingerprint = ""


func _bind_phase32_production_controls() -> void:
	if _material == null:
		return
	if _displacement_runtime != null and is_instance_valid(_displacement_runtime) \
			and _displacement_runtime.has_method("bind_production_controls"):
		_displacement_runtime.call("bind_production_controls", _material)
	if _material_runtime != null and is_instance_valid(_material_runtime) \
			and _material_runtime.has_method("bind_production_controls"):
		_material_runtime.call("bind_production_controls", _material)
