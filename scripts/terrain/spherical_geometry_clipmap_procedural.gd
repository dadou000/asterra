extends "res://scripts/terrain/spherical_geometry_clipmap_fast.gd"
## Fully procedural visual spherical terrain.
##
## Every L0-L14 clipmap vertex samples the immutable coarse planet maps and then
## synthesises all sub-grid terrain on the GPU. No camera-centred CPU terrain or
## material generation participates in this path.

const PROCEDURAL_DETAIL_STRENGTH: float = 1.0
const DEFAULT_SINK_SCALE: float = 2.0

var _debug_freeze := false
var _debug_side_cut := false
var _debug_sink_scale: float = DEFAULT_SINK_SCALE
var _bound_ctx_generation: int = -1


func _ready() -> void:
	super._ready()
	_material.shader = load("res://shaders/spherical_geometry_clipmap_procedural.gdshader")
	_bind_gpu_resources(true)
	_sync_detail_seed()
	_sync_debug_uniforms()


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
		_update_visible_cap(planet_pos.length(), radius)
		_update_active_levels()

	if not _debug_freeze:
		var observer_surface: Vector3 = observer_dir * radius
		var anchor_surface: Vector3 = _anchor_dir * radius
		var rel: Vector3 = observer_surface - anchor_surface
		var px: float = rel.dot(_anchor_right)
		var py: float = rel.dot(_anchor_up)
		if absf(px) > REANCHOR_M or absf(py) > REANCHOR_M:
			_reset_anchor(observer_dir)
			px = 0.0
			py = 0.0

		var snapped_center := Vector2(
			round(px / _base_spacing) * _base_spacing,
			round(py / _base_spacing) * _base_spacing)
		if snapped_center.distance_squared_to(_center_plane) > 1e-8:
			_center_plane = snapped_center
			_update_center_basis()

		_update_visible_cap(planet_pos.length(), radius)
		_update_active_levels()

	_bind_gpu_resources(false)
	_sync_uniforms(origin)
	_set_visible(_bound_orbit != null)
	if _terrain_visible:
		if _debug_freeze or _debug_side_cut:
			_show_all_active_sectors()
		else:
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
	_bind_planet_context(force)


func _bind_planet_context(force: bool) -> void:
	var context: Node = get_node_or_null("/root/PlanetContext")
	if context == null or not bool(context.get("ready_state")):
		_material.set_shader_parameter("u_ctx_ready", 0.0)
		return

	var generation: int = int(context.get("generation"))
	if force or generation != _bound_ctx_generation:
		_bound_ctx_generation = generation
		_material.set_shader_parameter("u_ctx_soil", context.get("soil_texture"))
		_material.set_shader_parameter("u_ctx_surface", context.get("surface_texture"))
		_material.set_shader_parameter("u_ctx_geology", context.get("geology_texture"))
		_material.set_shader_parameter("u_ctx_structure", context.get("structure_texture"))
		_material.set_shader_parameter("u_ctx_climate", context.get("climate_texture"))
		_material.set_shader_parameter("u_ctx_hydrology", context.get("hydrology_texture"))
		_material.set_shader_parameter("u_ctx_rock", context.get("rock_texture"))
		_material.set_shader_parameter("u_ctx_biome", context.get("biome_texture"))
		_material.set_shader_parameter("u_ctx_face_res", float(context.get("face_res")))
	_material.set_shader_parameter("u_ctx_ready", 1.0)


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
	_sync_debug_uniforms()


func _sync_detail_seed() -> void:
	if _material == null or Planet.cfg == null:
		return
	var detail_seed: int = Planet.cfg.stream_seed("gpu_visual_detail") & 0x00ffffff
	_material.set_shader_parameter("u_detail_seed", maxi(detail_seed, 1))
	_material.set_shader_parameter("u_detail_strength", PROCEDURAL_DETAIL_STRENGTH
		* maxf(0.05, Planet.cfg.detail_amplitude / 260.0))


func _sync_debug_uniforms() -> void:
	if _material == null:
		return
	_material.set_shader_parameter("u_debug_side_cut", 1.0 if _debug_side_cut else 0.0)
	_material.set_shader_parameter("u_sink_scale", _debug_sink_scale)


func _show_all_active_sectors() -> void:
	_visible_sector_count = 0
	if _active_max_level <= 0:
		for batch: MultiMeshInstance3D in _sector_batches:
			batch.visible = false
		return

	for sector: int in _sector_batches.size():
		var batch: MultiMeshInstance3D = _sector_batches[sector]
		var sector_visible := not _debug_side_cut or sector >= (SECTOR_COUNT >> 1)
		batch.visible = sector_visible
		if batch.multimesh != null:
			batch.multimesh.visible_instance_count = _active_max_level if sector_visible else 0
		if sector_visible:
			_visible_sector_count += 1


func set_debug_freeze(value: bool) -> void:
	_debug_freeze = value
	if _terrain_visible:
		if value:
			_show_all_active_sectors()
		else:
			_update_sector_visibility()


func set_debug_side_cut(value: bool) -> void:
	if _debug_side_cut == value:
		return
	_debug_side_cut = value
	_sync_debug_uniforms()
	rebuild_static_topology()
	if _terrain_visible:
		if value or _debug_freeze:
			_show_all_active_sectors()
		else:
			_update_sector_visibility()


func set_debug_sink_scale(value: float) -> void:
	_debug_sink_scale = clampf(value, 0.0, 16.0)
	_sync_debug_uniforms()


func debug_freeze_enabled() -> bool:
	return _debug_freeze


func debug_side_cut_enabled() -> bool:
	return _debug_side_cut


func debug_sink_scale() -> float:
	return _debug_sink_scale


func rebuild_static_topology() -> void:
	if _center_batch != null and _center_batch.multimesh != null:
		_center_batch.multimesh.mesh = _build_debug_center_mesh(_debug_side_cut)
	for sector: int in _sector_batches.size():
		var batch: MultiMeshInstance3D = _sector_batches[sector]
		if batch.multimesh != null:
			batch.multimesh.mesh = _build_debug_sector_mesh(sector, _debug_side_cut)


static func _build_debug_center_mesh(half_cut: bool) -> ArrayMesh:
	var vertices := PackedVector3Array()
	vertices.resize(GRID_VERTS * GRID_VERTS)
	var indices := PackedInt32Array()
	var outer_sq: float = float(HALF_CELLS * HALF_CELLS)
	for y: int in GRID_CELLS:
		var cy: float = float(y) + 0.5 - float(HALF_CELLS)
		if half_cut and cy > 0.0:
			continue
		for x: int in GRID_CELLS:
			var cx: float = float(x) + 0.5 - float(HALF_CELLS)
			if cx * cx + cy * cy > outer_sq:
				continue
			_append_debug_cell(indices, x, y)
	return _debug_mesh_from_indices(vertices, indices)


static func _build_debug_sector_mesh(sector_index: int, half_cut: bool) -> ArrayMesh:
	var vertices := PackedVector3Array()
	vertices.resize(GRID_VERTS * GRID_VERTS)
	var indices := PackedInt32Array()
	var outer_sq: float = float(HALF_CELLS * HALF_CELLS)
	var inner_sq: float = float(RING_INNER_HALF_CELLS * RING_INNER_HALF_CELLS)
	for y: int in GRID_CELLS:
		var cy: float = float(y) + 0.5 - float(HALF_CELLS)
		if half_cut and cy > 0.0:
			continue
		for x: int in GRID_CELLS:
			var cx: float = float(x) + 0.5 - float(HALF_CELLS)
			var r_sq: float = cx * cx + cy * cy
			if r_sq > outer_sq or r_sq < inner_sq:
				continue
			var angle: float = atan2(cy, cx)
			if angle < 0.0:
				angle += TAU
			var owner_sector: int = int(floor(angle / TAU * float(SECTOR_COUNT)))
			owner_sector = clampi(owner_sector, 0, SECTOR_COUNT - 1)
			if owner_sector != sector_index:
				continue
			_append_debug_cell(indices, x, y)
	return _debug_mesh_from_indices(vertices, indices)


static func _append_debug_cell(indices: PackedInt32Array, x: int, y: int) -> void:
	var i00: int = y * GRID_VERTS + x
	var i10: int = i00 + 1
	var i01: int = (y + 1) * GRID_VERTS + x
	var i11: int = i01 + 1
	indices.append(i00)
	indices.append(i10)
	indices.append(i11)
	indices.append(i00)
	indices.append(i11)
	indices.append(i01)


static func _debug_mesh_from_indices(vertices: PackedVector3Array,
		indices: PackedInt32Array) -> ArrayMesh:
	var mesh := ArrayMesh.new()
	if indices.is_empty():
		return mesh
	var arrays: Array = []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = vertices
	arrays[Mesh.ARRAY_INDEX] = indices
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	return mesh


func _request_visible_pages() -> void:
	pass


func gpu_stream_stats() -> Dictionary:
	var context: Node = get_node_or_null("/root/PlanetContext")
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
		"context_ready": context != null and bool(context.get("ready_state")),
		"context_generation": _bound_ctx_generation,
		"spherical": true,
		"grid_cells": GRID_CELLS,
		"fast_path": true,
		"concentric": true,
		"sector_count": SECTOR_COUNT,
		"visible_sectors": _visible_sector_count,
		"procedural_gpu": true,
		"visual_pages": false,
		"cpu_material_clipmap": false,
		"macro_upsample": 2,
		"debug_frozen": _debug_freeze,
		"debug_side_cut": _debug_side_cut,
		"debug_sink_scale": _debug_sink_scale,
	}
