extends "res://scripts/terrain/gpu_terrain_scatter_global.gd"
## Final terrain scatter binding with optional Planet Studio biome overrides.
##
## Runtime scatter remains unchanged until Planet Studio supplies a local authored
## biome texture. Because every current production procedural/ecology material uses
## gpu_planet_context.gdshaderinc, forwarding the same categorical override makes
## material appearance and scatter suitability agree with the live terrain preview.

var _author_biome_ready: bool = false
var _author_biome_texture: Texture2D
var _author_biome_center: Vector3 = Vector3.RIGHT
var _author_biome_right: Vector3 = Vector3.BACK
var _author_biome_up: Vector3 = Vector3.UP
var _author_biome_half_extent_m: float = 512.0
var _author_biome_planet_radius: float = 1000000.0


func set_author_biome_preview(texture: Texture2D, ready: bool,
		center: Vector3, right: Vector3, up: Vector3,
		half_extent_m: float, planet_radius: float) -> void:
	_author_biome_texture = texture
	_author_biome_ready = ready and texture != null
	_author_biome_center = center.normalized() if center.length_squared() > 0.5 else Vector3.RIGHT
	_author_biome_right = right.normalized() if right.length_squared() > 0.5 else Vector3.BACK
	_author_biome_up = up.normalized() if up.length_squared() > 0.5 else Vector3.UP
	_author_biome_half_extent_m = maxf(half_extent_m, 0.001)
	_author_biome_planet_radius = maxf(planet_radius, 1.0)
	_sync_author_biome_materials()


func clear_author_biome_preview() -> void:
	_author_biome_ready = false
	_sync_author_biome_materials()


func _sync_author_biome_materials() -> void:
	for material: ShaderMaterial in _materials:
		material.set_shader_parameter("u_author_biome_override", _author_biome_texture)
		material.set_shader_parameter("u_author_biome_ready", 1.0 if _author_biome_ready else 0.0)
		material.set_shader_parameter("u_author_biome_center", _author_biome_center)
		material.set_shader_parameter("u_author_biome_right", _author_biome_right)
		material.set_shader_parameter("u_author_biome_up", _author_biome_up)
		material.set_shader_parameter("u_author_biome_half_extent_m", _author_biome_half_extent_m)
		material.set_shader_parameter("u_author_biome_planet_radius", _author_biome_planet_radius)


func gpu_scatter_stats() -> Dictionary:
	var out: Dictionary = super.gpu_scatter_stats()
	out["author_biome_preview_ready"] = _author_biome_ready
	out["author_biome_preview_half_extent_m"] = _author_biome_half_extent_m
	return out
