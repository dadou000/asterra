extends "res://scripts/terrain/spherical_geometry_clipmap_submission_debug.gd"
## Dense near-field material microgeometry for the corrected global clipmap.
##
## Unlike the old implementation, this does not assume that L0 is permanently
## present. The dense patch and corresponding centre-disc hole exist only while
## logical L0 is active, enabled by the render debugger, and microgeometry itself
## is enabled. Otherwise the ordinary full centre-sector meshes are restored.

const MICRO_STEP_L0: float = 0.25
const MICRO_GRID_CELLS: int = 256
const MICRO_GRID_VERTS: int = MICRO_GRID_CELLS + 1
const MICRO_HALF_CELLS: int = MICRO_GRID_CELLS >> 1
const MICRO_OUTER_L0_CELLS: float = 32.0
const MICRO_L0_HOLE_CELLS: float = 24.0

var _micro_batch: MultiMeshInstance3D
var _full_center_meshes: Array[Mesh] = []
var _hole_center_meshes: Array[ArrayMesh] = []
var _micro_l0_active := false
var _debug_microrelief_enabled := true


func _build_batches() -> void:
	super._build_batches()
	_capture_full_center_meshes()
	_build_hole_center_meshes(_debug_side_cut)

	var bounds := AABB(
		Vector3(-GLOBAL_BOUNDS_M, -GLOBAL_BOUNDS_M, -GLOBAL_BOUNDS_M),
		Vector3(GLOBAL_BOUNDS_M * 2.0, GLOBAL_BOUNDS_M * 2.0, GLOBAL_BOUNDS_M * 2.0))
	_micro_batch = _make_batch("SphericalClipmapMicroL0", _build_micro_disc_mesh(false), 1, bounds)
	_micro_batch.multimesh.set_instance_custom_data(0, Color(0.0, 0.0, 0.0, 1.0))
	_micro_batch.visible = false
	_micro_batch.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	add_child(_micro_batch)


func _apply_active_level_window() -> void:
	super._apply_active_level_window()
	_sync_micro_lod_state()


func _sync_micro_lod_state() -> void:
	var want_micro: bool = _active_min_level == 0 \
		and _debug_microrelief_enabled and debug_level_enabled(0)
	if want_micro != _micro_l0_active:
		_micro_l0_active = want_micro
		_apply_center_mesh_variant()
	if _micro_batch != null:
		_micro_batch.visible = _micro_should_be_visible()


func _micro_should_be_visible() -> bool:
	return _terrain_visible and _micro_l0_active and not _view_surface_culled \
		and debug_level_enabled(0)


func _apply_center_mesh_variant() -> void:
	if _full_center_meshes.size() != SECTOR_COUNT or _hole_center_meshes.size() != SECTOR_COUNT:
		return
	for sector: int in SECTOR_COUNT:
		var center: MultiMeshInstance3D = _center_sector_batches[sector]
		if center.multimesh == null:
			continue
		center.multimesh.mesh = _hole_center_meshes[sector] if _micro_l0_active \
			else _full_center_meshes[sector]


func _capture_full_center_meshes() -> void:
	_full_center_meshes.clear()
	for sector: int in SECTOR_COUNT:
		var center: MultiMeshInstance3D = _center_sector_batches[sector]
		_full_center_meshes.append(center.multimesh.mesh if center.multimesh != null else null)


func _build_hole_center_meshes(half_cut: bool) -> void:
	_hole_center_meshes.clear()
	for sector: int in SECTOR_COUNT:
		_hole_center_meshes.append(_build_center_sector_with_micro_hole(sector, half_cut))


func _set_visible(value: bool) -> void:
	super._set_visible(value)
	if _micro_batch != null:
		_micro_batch.visible = _micro_should_be_visible()


func _update_sector_visibility() -> void:
	super._update_sector_visibility()
	if _micro_batch != null:
		_micro_batch.visible = _micro_should_be_visible()


func _show_all_active_sectors() -> void:
	super._show_all_active_sectors()
	if _micro_batch != null:
		_micro_batch.visible = _terrain_visible and _micro_l0_active and debug_level_enabled(0)


func rebuild_static_topology() -> void:
	super.rebuild_static_topology()
	_capture_full_center_meshes()
	_build_hole_center_meshes(_debug_side_cut)
	if _micro_batch != null and _micro_batch.multimesh != null:
		_micro_batch.multimesh.mesh = _build_micro_disc_mesh(_debug_side_cut)
	_apply_center_mesh_variant()


func _sync_debug_uniforms() -> void:
	super._sync_debug_uniforms()
	if _material != null:
		_material.set_shader_parameter("u_microrelief_enabled",
			1.0 if _debug_microrelief_enabled else 0.0)


func set_debug_microrelief_enabled(value: bool) -> void:
	_debug_microrelief_enabled = value
	_sync_debug_uniforms()
	_sync_micro_lod_state()
	if _terrain_visible and not _debug_side_cut:
		_update_sector_visibility()


func debug_microrelief_enabled() -> bool:
	return _debug_microrelief_enabled


func set_debug_level_enabled(level: int, enabled: bool) -> void:
	super.set_debug_level_enabled(level, enabled)
	if level == 0:
		_sync_micro_lod_state()


func set_all_debug_levels_enabled(enabled: bool) -> void:
	super.set_all_debug_levels_enabled(enabled)
	_sync_micro_lod_state()


static func _build_center_sector_with_micro_hole(sector_index: int,
		half_cut: bool) -> ArrayMesh:
	var vertices: Array[Vector3] = []
	var uvs: Array[Vector2] = []
	var indices := PackedInt32Array()
	var remap: Dictionary = {}
	var outer_sq: float = float(HALF_CELLS * HALF_CELLS)
	var hole_sq: float = MICRO_L0_HOLE_CELLS * MICRO_L0_HOLE_CELLS

	for y: int in GRID_CELLS:
		var cy: float = float(y) + 0.5 - float(HALF_CELLS)
		if half_cut and cy > 0.0:
			continue
		for x: int in GRID_CELLS:
			var cx: float = float(x) + 0.5 - float(HALF_CELLS)
			var r_sq: float = cx * cx + cy * cy
			if r_sq > outer_sq or r_sq < hole_sq:
				continue
			var angle: float = atan2(cy, cx)
			if angle < 0.0:
				angle += TAU
			var owner_sector: int = clampi(
				int(floor(angle / TAU * float(SECTOR_COUNT))), 0, SECTOR_COUNT - 1)
			if owner_sector != sector_index:
				continue
			var i00: int = _center_vertex(remap, vertices, uvs, x, y)
			var i10: int = _center_vertex(remap, vertices, uvs, x + 1, y)
			var i01: int = _center_vertex(remap, vertices, uvs, x, y + 1)
			var i11: int = _center_vertex(remap, vertices, uvs, x + 1, y + 1)
			indices.append_array(PackedInt32Array([i00, i10, i11, i00, i11, i01]))
	return _mesh_from_micro_arrays(vertices, uvs, indices)


static func _center_vertex(remap: Dictionary, vertices: Array[Vector3],
		uvs: Array[Vector2], gx: int, gy: int) -> int:
	var logical_index: int = gy * GRID_VERTS + gx
	var existing: Variant = remap.get(logical_index, null)
	if existing != null:
		return int(existing)
	var local_index: int = vertices.size()
	remap[logical_index] = local_index
	vertices.append(Vector3.ZERO)
	uvs.append(Vector2(float(gx), float(gy)))
	return local_index


static func _build_micro_disc_mesh(half_cut: bool) -> ArrayMesh:
	var vertices: Array[Vector3] = []
	var uvs: Array[Vector2] = []
	var indices := PackedInt32Array()
	var remap: Dictionary = {}
	var outer_sq: float = MICRO_OUTER_L0_CELLS * MICRO_OUTER_L0_CELLS
	var grid_center: float = float(GRID_CELLS) * 0.5

	for y: int in MICRO_GRID_CELLS:
		var cy: float = (float(y) + 0.5 - float(MICRO_HALF_CELLS)) * MICRO_STEP_L0
		if half_cut and cy > 0.0:
			continue
		for x: int in MICRO_GRID_CELLS:
			var cx: float = (float(x) + 0.5 - float(MICRO_HALF_CELLS)) * MICRO_STEP_L0
			if cx * cx + cy * cy > outer_sq:
				continue
			var i00: int = _micro_vertex(remap, vertices, uvs, x, y, grid_center)
			var i10: int = _micro_vertex(remap, vertices, uvs, x + 1, y, grid_center)
			var i01: int = _micro_vertex(remap, vertices, uvs, x, y + 1, grid_center)
			var i11: int = _micro_vertex(remap, vertices, uvs, x + 1, y + 1, grid_center)
			indices.append_array(PackedInt32Array([i00, i10, i11, i00, i11, i01]))
	return _mesh_from_micro_arrays(vertices, uvs, indices)


static func _micro_vertex(remap: Dictionary, vertices: Array[Vector3],
		uvs: Array[Vector2], gx: int, gy: int, grid_center: float) -> int:
	var logical_index: int = gy * MICRO_GRID_VERTS + gx
	var existing: Variant = remap.get(logical_index, null)
	if existing != null:
		return int(existing)
	var local_index: int = vertices.size()
	remap[logical_index] = local_index
	vertices.append(Vector3.ZERO)
	uvs.append(Vector2(
		grid_center + (float(gx) - float(MICRO_HALF_CELLS)) * MICRO_STEP_L0,
		grid_center + (float(gy) - float(MICRO_HALF_CELLS)) * MICRO_STEP_L0))
	return local_index


static func _mesh_from_micro_arrays(vertices: Array[Vector3], uvs: Array[Vector2],
		indices: PackedInt32Array) -> ArrayMesh:
	var mesh := ArrayMesh.new()
	if indices.is_empty():
		return mesh
	var arrays: Array = []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = PackedVector3Array(vertices)
	arrays[Mesh.ARRAY_TEX_UV] = PackedVector2Array(uvs)
	arrays[Mesh.ARRAY_INDEX] = indices
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	return mesh


func gpu_stream_stats() -> Dictionary:
	var out: Dictionary = super.gpu_stream_stats()
	out["draw_batches"] = int(out.get("draw_batches", 0)) + (1 if _micro_batch != null and _micro_batch.visible else 0)
	out["microgeometry"] = true
	out["micro_active"] = _micro_l0_active
	out["micro_spacing_m"] = _base_spacing * MICRO_STEP_L0
	out["micro_radius_m"] = _base_spacing * MICRO_OUTER_L0_CELLS
	out["micro_handoff_start_m"] = _base_spacing * MICRO_L0_HOLE_CELLS
	out["microrelief_enabled"] = _debug_microrelief_enabled
	return out
