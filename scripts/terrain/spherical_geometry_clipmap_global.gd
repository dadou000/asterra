extends "res://scripts/terrain/spherical_geometry_clipmap_procedural_safe.gd"
## Final visual binding for the resident whole-planet heightmap architecture.
##
## The procedural/safe parents still own the concentric geometry, sinking, debug
## tools and GPU detail spectrum. This layer only changes the low-frequency source:
## it is the immutable global height texture, never the independently refined orbit
## texture and never a streamed page cache.


func _ready() -> void:
	super._ready()
	_bind_gpu_resources(true)


func _bind_gpu_resources(force: bool) -> void:
	if _material == null:
		return
	var macro: Texture2DArray = Planet.global_height_texture if Planet.ready_state else null
	var macro_res: int = Planet.global_height_face_res if Planet.ready_state else 0
	if force or macro != _bound_orbit:
		_bound_orbit = macro
		_material.set_shader_parameter("u_macro_elevation", macro)
	if force or macro_res != _bound_orbit_res:
		_bound_orbit_res = macro_res
		_material.set_shader_parameter("u_macro_face_res", float(macro_res))
	_material.set_shader_parameter("u_macro_ready", 1.0 if macro != null else 0.0)


func gpu_stream_stats() -> Dictionary:
	var out: Dictionary = super.gpu_stream_stats()
	out["coverage_ready"] = _bound_orbit != null
	out["global_heightmap"] = true
	out["global_face_res"] = Planet.global_height_face_res if Planet.ready_state else 0
	out["terrain_streaming"] = false
	out["visual_pages"] = false
	return out
