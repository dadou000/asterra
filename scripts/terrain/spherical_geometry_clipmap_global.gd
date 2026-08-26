extends "res://scripts/terrain/spherical_geometry_clipmap_procedural_safe.gd"
## Final visual binding for the resident whole-planet terrain architecture.
##
## The procedural/safe parents own concentric geometry, debug tools and GPU detail.
## This layer binds immutable global height/material-control textures. It does not
## alter ring submission: the final GPU submission layer owns that state explicitly.

var _bound_global_material: Texture2DArray
var _bound_global_material_res: int = 0


func _ready() -> void:
	super._ready()
	_bind_gpu_resources(true)
	_sync_material_control()


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


## Override the old camera-centred material clipmap binding. The parent calls this
## every frame, but after the first reference comparison it only updates readiness
## scalars; no image construction/upload is possible here.
func _sync_material_control() -> void:
	if _material == null:
		return
	var texture: Texture2DArray = Planet.global_material_texture \
		if Planet.ready_state and Planet.global_material_texture != null else null
	var face_res: int = Planet.global_material_face_res if Planet.ready_state else 0
	if texture != _bound_global_material:
		_bound_global_material = texture
		_material.set_shader_parameter("u_material_global", texture)
	if face_res != _bound_global_material_res:
		_bound_global_material_res = face_res
		_material.set_shader_parameter("u_material_global_res", float(face_res))
	_material.set_shader_parameter("u_material_global_ready", 1.0 if texture != null else 0.0)
	_material.set_shader_parameter("u_material_clipmap_ready", 0.0)


func gpu_stream_stats() -> Dictionary:
	var out: Dictionary = super.gpu_stream_stats()
	out["coverage_ready"] = _bound_orbit != null
	out["global_heightmap"] = true
	out["global_face_res"] = Planet.global_height_face_res if Planet.ready_state else 0
	out["global_materialmap"] = true
	out["global_material_face_res"] = Planet.global_material_face_res if Planet.ready_state else 0
	out["material_streaming"] = false
	out["terrain_streaming"] = false
	out["visual_pages"] = false
	out["implicit_ring_restore"] = false
	return out
