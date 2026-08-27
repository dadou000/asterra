extends "res://scripts/terrain/gpu_terrain_edit_delta.gd"
## Same persistent edit mirror, but shader bindings are published only when the
## texture generation/center/readiness actually changes. Walking no longer queues
## identical material parameter updates every rendered frame.

var _opt_bound_material: ShaderMaterial
var _opt_bound_generation := -1
var _opt_bound_ready := false


func _sync_renderer_binding() -> void:
	var terrain: Node = get_node_or_null("/root/GroundGeometryClipmap")
	if terrain == null:
		return
	var value: Variant = terrain.get("_material")
	if not (value is ShaderMaterial):
		return
	var material := value as ShaderMaterial
	var ready_now: bool = ready_state and _center_valid
	var material_changed: bool = material != _opt_bound_material
	var generation_changed: bool = generation != _opt_bound_generation
	var ready_changed: bool = ready_now != _opt_bound_ready
	if not material_changed and not generation_changed and not ready_changed:
		return
	_opt_bound_material = material
	material.set_shader_parameter("u_edit_delta", texture)
	material.set_shader_parameter("u_edit_ready", 1.0 if ready_now else 0.0)
	material.set_shader_parameter("u_edit_center_dir", center_dir)
	material.set_shader_parameter("u_edit_center_right", center_right)
	material.set_shader_parameter("u_edit_center_up", center_up)
	material.set_shader_parameter("u_edit_half_extent_m", HALF_EXTENT_M)
	_opt_bound_generation = generation
	_opt_bound_ready = ready_now
