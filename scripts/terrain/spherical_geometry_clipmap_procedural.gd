extends "res://scripts/terrain/spherical_geometry_clipmap_fast.gd"
## Fully procedural visual spherical terrain.
##
## Visual height pages are no longer part of the terrain path. Every L0-L14
## clipmap vertex samples one interpolated 2x-smoothed macro elevation texture and
## evaluates the same deterministic GPU detail spectrum, band-limited to that
## level's spacing. The outer morph therefore transitions between two evaluations
## of the same continuous world-space function rather than between streamed pages.

const PROCEDURAL_DETAIL_STRENGTH: float = 1.0
const DEFAULT_SINK_SCALE: float = 2.0

var _debug_freeze := false
var _debug_side_cut := false
var _debug_sink_scale: float = DEFAULT_SINK_SCALE
var _debug_stable_displacement := true

# Canonical double-precision surface point for the current tangent anchor. The
# active shader can reconstruct every rendered vertex as a small offset from this
# point, avoiding an absolute ~1,000 km position subtraction in float32.
var _stable_anchor_world: Vec3D = Vec3D.new()


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

	var observer_world: Vec3D = Frames.to_world(camera.global_position)
	var observer_radius: float = observer_world.length()
	if observer_radius <= 1.0:
		_set_visible(false)
		return

	var radius: float = Planet.cfg.planet_radius
	var observer_unit_world: Vec3D = observer_world.normalized()
	var observer_dir: Vector3 = observer_unit_world.to_v3()
	if not _have_anchor:
		_reset_anchor(observer_dir)
		_capture_stable_anchor(observer_unit_world.mul(radius))
		_update_visible_cap(observer_radius, radius)
		_update_active_levels()

	# Freeze affects only the planet-space clipmap state. Camera-dependent render
	# culling must remain live while frozen; otherwise the debug mode itself changes
	# GPU workload and makes profiling misleading.
	if not _debug_freeze:
		var observer_surface_world: Vec3D = observer_unit_world.mul(radius)
		# The production shaders interpret anchor-plane coordinates gnomonically:
		# offset = R * tangent / radial, then reconstruct direction by normalizing
		# anchor + tangent / R. The old CPU path used the orthographic chord
		# R*sin(theta), so the CPU centre and GPU centre diverged continuously between
		# reanchors (about 140 m at the 65.5 km threshold on a 1000 km planet).
		var observer_plane: Vector2 = _project_surface_gnomonic(
			observer_surface_world, _anchor_dir, _anchor_right, _anchor_up, radius)
		var px: float = observer_plane.x
		var py: float = observer_plane.y
		if absf(px) > REANCHOR_M or absf(py) > REANCHOR_M:
			_reset_anchor(observer_dir)
			_capture_stable_anchor(observer_surface_world)
			px = 0.0
			py = 0.0

		var snapped_center := Vector2(
			round(px / _base_spacing) * _base_spacing,
			round(py / _base_spacing) * _base_spacing)
		if snapped_center.distance_squared_to(_center_plane) > 1e-8:
			_center_plane = snapped_center
			_update_center_basis()

		_update_visible_cap(observer_radius, radius)
		_update_active_levels()

	var origin := Vector3(float(Frames.origin.x), float(Frames.origin.y), float(Frames.origin.z))
	_bind_gpu_resources(false)
	_sync_uniforms(origin)
	_sync_material_control()
	# _set_visible(true) dispatches to the active subclass' _update_sector_visibility()
	# exactly once. Do not run a second ordinary culling pass here. Side-cut is the
	# sole exception because it intentionally displays the complete diagnostic half.
	_set_visible(_bound_orbit != null)
	if _terrain_visible and _debug_side_cut:
		_show_all_active_sectors()


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
	_material.set_shader_parameter("u_anchor_render", Frames.to_render(_stable_anchor_world))
	_material.set_shader_parameter("u_anchor_dir", _anchor_dir)
	_material.set_shader_parameter("u_anchor_right", _anchor_right)
	_material.set_shader_parameter("u_anchor_up", _anchor_up)
	_material.set_shader_parameter("u_lattice_center_plane", _center_plane)
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
	_material.set_shader_parameter("u_stable_displacement",
		1.0 if _debug_stable_displacement else 0.0)


func _capture_stable_anchor(surface_world: Vec3D) -> void:
	_stable_anchor_world = surface_world.dup()


func _project_surface_gnomonic(surface_world: Vec3D, anchor_dir: Vector3,
		right: Vector3, up: Vector3, radius: float) -> Vector2:
	# Work directly from the double-precision surface point instead of subtracting
	# two ~planet-radius vectors. For a unit direction d this is exactly
	# R*(dot(d,right), dot(d,up))/dot(d,anchor), the inverse of the near-field GPU
	# projection used by the terrain and terrain-cache shaders.
	var radial: float = surface_world.x * anchor_dir.x \
		+ surface_world.y * anchor_dir.y + surface_world.z * anchor_dir.z
	if radial <= maxf(radius, 1.0) * 1.0e-6:
		return Vector2.ZERO
	var scale: float = radius / radial
	return Vector2(
		(surface_world.x * right.x + surface_world.y * right.y + surface_world.z * right.z) * scale,
		(surface_world.x * up.x + surface_world.y * up.y + surface_world.z * up.z) * scale)


func _gnomonic_direction_for_offset(center: Vector3, right: Vector3, up: Vector3,
		offset_m: Vector2, radius: float) -> Vector3:
	var safe_radius: float = maxf(radius, 1.0)
	return (center.normalized()
		+ right.normalized() * (offset_m.x / safe_radius)
		+ up.normalized() * (offset_m.y / safe_radius)).normalized()


func _update_center_basis() -> void:
	# Override the legacy arc-length helper. The active GPU shader's near-field
	# lattice is gnomonic, so its CPU culling/centre basis must use the exact same
	# inverse or the clipmap disc visibly walks away from the camera between anchors.
	_center_dir = _gnomonic_direction_for_offset(
		_anchor_dir, _anchor_right, _anchor_up, _center_plane, Planet.cfg.planet_radius)
	var tangent: Array = CubeSphere.tangent_basis(_center_dir)
	_center_right = tangent[0]
	_center_up = tangent[1]


func _show_all_active_sectors() -> void:
	_visible_sector_count = 0
	if _active_max_level <= 0:
		for batch: MultiMeshInstance3D in _sector_batches:
			batch.visible = false
		return

	for sector: int in _sector_batches.size():
		var batch: MultiMeshInstance3D = _sector_batches[sector]
		# The topology cut keeps only cy <= 0, which maps to angular sectors 6..11.
		# Hide the empty upper-half sector nodes entirely instead of submitting six
		# empty MultiMeshes and reporting them as visible debug batches.
		var sector_visible := not _debug_side_cut or sector >= (SECTOR_COUNT >> 1)
		batch.visible = sector_visible
		if batch.multimesh != null:
			batch.multimesh.visible_instance_count = _active_max_level if sector_visible else 0
		if sector_visible:
			_visible_sector_count += 1


func set_debug_freeze(value: bool) -> void:
	_debug_freeze = value
	# Freeze only stops clipmap movement/LOD evolution. The camera still moves, so
	# sky/nadir/azimuth culling must be recomputed exactly as in normal rendering.
	if _terrain_visible:
		if _debug_side_cut:
			_show_all_active_sectors()
		else:
			_update_sector_visibility()


func set_debug_side_cut(value: bool) -> void:
	if _debug_side_cut == value:
		return
	_debug_side_cut = value
	_sync_debug_uniforms()
	# Rebuild only static index topology. In side-cut mode the +clipmap-up half is
	# genuinely absent, so Godot's global wireframe debug sees the same cut as the
	# shaded renderer instead of relying on fragment discard.
	rebuild_static_topology()
	if _terrain_visible:
		if value:
			_show_all_active_sectors()
		else:
			_update_sector_visibility()


func set_debug_sink_scale(value: float) -> void:
	_debug_sink_scale = clampf(value, 0.0, 16.0)
	_sync_debug_uniforms()


func set_debug_stable_displacement(value: bool) -> void:
	_debug_stable_displacement = value
	_sync_debug_uniforms()


func debug_freeze_enabled() -> bool:
	return _debug_freeze


func debug_side_cut_enabled() -> bool:
	return _debug_side_cut


func debug_sink_scale() -> float:
	return _debug_sink_scale


func debug_stable_displacement_enabled() -> bool:
	return _debug_stable_displacement


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
	# A removed half-plane leaves sectors 0..5 with zero cells. Never submit the
	# 401x401 placeholder vertex buffer as accidental non-indexed geometry.
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
		"debug_frozen": _debug_freeze,
		"debug_side_cut": _debug_side_cut,
		"debug_sink_scale": _debug_sink_scale,
		"stable_displacement": _debug_stable_displacement,
	}
