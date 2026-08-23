extends "res://scripts/terrain/spherical_geometry_clipmap_fast.gd"
## Fully procedural visual spherical terrain.
##
## Visual height pages are no longer part of the terrain path. Every L0-L14
## clipmap vertex samples one interpolated 2x-smoothed macro elevation texture and
## evaluates the same deterministic GPU detail spectrum, band-limited to that
## level's spacing. The outer morph therefore transitions between two evaluations
## of the same continuous world-space function rather than between streamed pages.

const PROCEDURAL_DETAIL_STRENGTH: float = 1.0


func _ready() -> void:
	super._ready()
	_material.shader = load("res://shaders/spherical_geometry_clipmap_procedural.gdshader")
	_bind_gpu_resources(true)
	_sync_detail_seed()


func _process(_dt: float) -> void:
	if not Planet.ready_state or Planet.cfg == null:
		_set_visible(false)
		return

	var camera: Camera3D = get_viewport().get_camera_3d()
	if camera == null:
		_set_visible(false)
		return

	var origin := Vector3(float(Frames.origin.x), float(Frames.origin.y), float(Frames.origin.z))
	var planet_pos: Vector3 = camera.global_position + origin
	if planet_pos.length_squared() <= 1.0:
		_set_visible(false)
		return

	var radius: float = Planet.cfg.planet_radius
	var observer_dir: Vector3 = planet_pos.normalized()
	if not _have_anchor:
		_reset_anchor(observer_dir)

	var observer_surface: Vector3 = observer_dir * radius
	var anchor_surface: Vector3 = _anchor_dir * radius
	var rel: Vector3 = observer_surface - anchor_surface
	var px: float = rel.dot(_anchor_right)
	var py: float = rel.dot(_anchor_up)
	if absf(px) > REANCHOR_M or absf(py) > REANCHOR_M:
		_reset_anchor(observer_dir)
		px = 0.0
		py = 0.0

	var snapped := Vector2(
		round(px / _base_spacing) * _base_spacing,
		round(py / _base_spacing) * _base_spacing)
	if snapped.distance_squared_to(_center_plane) > 1e-8:
		_center_plane = snapped
		_update_center_basis()

	_update_visible_cap(planet_pos.length(), radius)
	_update_active_levels()
	_bind_gpu_resources(false)
	_sync_uniforms(origin)
	_sync_material_control()
	_set_visible(_bound_orbit != null)
	if _terrain_visible:
		_update_sector_visibility()


func _bind_gpu_resources(force: bool) -> void:
	if _material == null:
		return
	var macro: Texture2DArray = Planet.orbit_elevation_texture if Planet.ready_state else null
	var macro_res: int = Planet.orbit_texture_face_res if Planet.ready_state else 0
	if force or macro != _bound_orbit:
		_bound_orbit = macro
		_material.set_shader_parameter("u_macro_elevation", macro)
	if force or macro_res != _bound_orbit_res:
		_bound_orbit_res = macro_res
		_material.set_shader_parameter("u_macro_face_res", float(macro_res))
	_material.set_shader_parameter("u_macro_ready", 1.0 if macro != null else 0.0)


func _sync_uniforms(origin: Vector3) -> void:
	_material.set_shader_parameter("u_origin", origin)
	_material.set_shader_parameter("u_planet_radius", Planet.cfg.planet_radius)
	_material.set_shader_parameter("u_center_dir", _center_dir)
	_material.set_shader_parameter("u_center_right", _center_right)
	_material.set_shader_parameter("u_center_up", _center_up)
	_material.set_shader_parameter("u_base_spacing", _base_spacing)
	_material.set_shader_parameter("u_grid_cells", float(GRID_CELLS))
	_material.set_shader_parameter("u_visible_cap_angle",
		minf(_visible_cap_arc_m / Planet.cfg.planet_radius * 1.03, PI * 0.5))
	_material.set_shader_parameter("u_height_enabled", 1.0 if _height_enabled else 0.0)
	_material.set_shader_parameter("u_macro_ready", 1.0 if _bound_orbit != null else 0.0)
	_sync_detail_seed()


func _sync_detail_seed() -> void:
	if _material == null or Planet.cfg == null:
		return
	# Keep the shader integer in the exact 24-bit range so conversion through the
	# rendering parameter path cannot lose seed bits.
	var seed: int = Planet.cfg.stream_seed("gpu_visual_detail") & 0x00ffffff
	_material.set_shader_parameter("u_detail_seed", maxi(seed, 1))
	_material.set_shader_parameter("u_detail_strength", PROCEDURAL_DETAIL_STRENGTH
		* maxf(0.05, Planet.cfg.detail_amplitude / 260.0))


func _request_visible_pages() -> void:
	# Intentionally empty. The complete visual terrain function is evaluated on
	# GPU at clipmap vertices. GroundHeightStore exists only for CPU physics/edit
	# queries and is not driven by camera-visible terrain anymore.
	pass


func gpu_stream_stats() -> Dictionary:
	return {
		"draw_batches": 1 + _visible_sector_count if _active_max_level > 0 else 1,
		"active_levels": _active_max_level + 1,
		"max_level": MAX_LEVEL,
		"visible_cap_km": _visible_cap_arc_m / 1000.0,
		"page_resident": 0,
		"page_capacity": 0,
		"page_uploads": 0,
		"page_reuploads": 0,
		"page_evictions": 0,
		"page_texels": 0,
		"table_updates": 0,
		"table_rebuilds": 0,
		"table_tombstones": 0,
		"table_failures": 0,
		"coverage_ready": _bound_orbit != null,
		"spherical": true,
		"grid_cells": GRID_CELLS,
		"fast_path": true,
		"concentric": true,
		"sector_count": SECTOR_COUNT,
		"visible_sectors": _visible_sector_count,
		"procedural_gpu": true,
		"visual_pages": false,
		"macro_upsample": 2,
	}
