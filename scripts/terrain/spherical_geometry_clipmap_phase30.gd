extends "res://scripts/terrain/spherical_geometry_clipmap_phase29.gd"
## Phase 30 clipmap/contact behavior with the Phase 32 production-stage graph
## runtimes. Startup/autoload paths stay stable; only the authoring compilers are
## upgraded underneath the existing authoritative terrain runtime.

const ACTIVE_DISPLACEMENT_RUNTIME := preload(
	"res://scripts/world_authoring/terrain_displacement_runtime_phase32.gd")
const ACTIVE_MATERIAL_RUNTIME := preload(
	"res://scripts/world_authoring/terrain_material_runtime_phase32.gd")


func _process(dt: float) -> void:
	super._process(dt)
	# Parent runtime refreshes a few production uniforms every frame. Graph-owned
	# production settings are the final authority, so bind them afterward.
	_bind_production_graph_controls()


func _ensure_displacement_runtime() -> void:
	if _displacement_runtime != null and is_instance_valid(_displacement_runtime):
		if _displacement_runtime.get_script() == ACTIVE_DISPLACEMENT_RUNTIME:
			return
		_displacement_runtime.remove_from_group(&"terrain_displacement_runtime")
		_displacement_runtime.queue_free()
		_displacement_runtime = null
	_displacement_runtime = ACTIVE_DISPLACEMENT_RUNTIME.new() as Node
	if _displacement_runtime == null:
		return
	_displacement_runtime.name = "TerrainDisplacementRuntime"
	add_child(_displacement_runtime)
	_displacement_fingerprint = ""


func _ensure_material_runtime() -> void:
	if _material_runtime != null and is_instance_valid(_material_runtime):
		if _material_runtime.get_script() == ACTIVE_MATERIAL_RUNTIME:
			return
		_material_runtime.remove_from_group(&"terrain_material_runtime")
		_material_runtime.queue_free()
		_material_runtime = null
	_material_runtime = ACTIVE_MATERIAL_RUNTIME.new() as Node
	if _material_runtime == null:
		return
	_material_runtime.name = "TerrainMaterialRuntime"
	add_child(_material_runtime)
	_material_fingerprint = ""


func _bind_production_graph_controls() -> void:
	if _material == null:
		return
	if _displacement_runtime != null and is_instance_valid(_displacement_runtime) \
			and _displacement_runtime.has_method("bind_production_controls"):
		_displacement_runtime.call("bind_production_controls", _material)
	if _material_runtime != null and is_instance_valid(_material_runtime) \
			and _material_runtime.has_method("bind_production_controls"):
		_material_runtime.call("bind_production_controls", _material)
