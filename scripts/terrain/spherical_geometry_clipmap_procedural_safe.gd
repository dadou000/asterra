extends "res://scripts/terrain/spherical_geometry_clipmap_procedural.gd"
## Robust compact-sector topology for the procedural spherical clipmap.
##
## The sector optimisation previously kept a 401x401 global placeholder vertex
## buffer in every sparse sector mesh and reconstructed logical coordinates from
## VERTEX_ID. That is brittle for indexed/compacted draws and can make triangles
## in the same logical grid cell reference unrelated positions. This renderer
## instead compacts each sector to only the vertices it actually uses and stores
## the original logical grid coordinate explicitly in UV.
##
## Every sector remains comfortably below 65k vertices/indices, removing any
## dependence on large sparse index ranges while preserving angular culling.

var _center_sector_batches: Array[MultiMeshInstance3D] = []


func _ready() -> void:
	super._ready()
	_material.shader = load("res://shaders/spherical_geometry_clipmap_procedural_uv.gdshader")
	_bind_gpu_resources(true)
	_sync_detail_seed()
	_sync_debug_uniforms()
	for batch: MultiMeshInstance3D in _center_sector_batches:
		batch.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	for batch: MultiMeshInstance3D in _sector_batches:
		batch.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF


func _build_batches() -> void:
	_center_sector_batches.clear()
	_sector_batches.clear()
	_center_batch = null
	_ring_batch = null

	var bounds := AABB(
		Vector3(-GLOBAL_BOUNDS_M, -GLOBAL_BOUNDS_M, -GLOBAL_BOUNDS_M),
		Vector3(GLOBAL_BOUNDS_M * 2.0, GLOBAL_BOUNDS_M * 2.0, GLOBAL_BOUNDS_M * 2.0))

	for sector: int in SECTOR_COUNT:
		var center_mesh: ArrayMesh = _build_compact_sector_mesh(sector, false, false)
		var center_batch: MultiMeshInstance3D = _make_batch(
			"SphericalClipmapCenterSector%02d" % sector, center_mesh, 1, bounds)
		center_batch.multimesh.set_instance_custom_data(0,
			Color(0.0, float(sector), 0.0, 0.0))
		center_batch.visible = false
		_center_sector_batches.append(center_batch)
		add_child(center_batch)

		var ring_mesh: ArrayMesh = _build_compact_sector_mesh(sector, true, false)
		var ring_batch: MultiMeshInstance3D = _make_batch(
			"SphericalClipmapRingSector%02d" % sector, ring_mesh, MAX_LEVEL, bounds)
		for instance_index: int in MAX_LEVEL:
			var level: int = instance_index + 1
			ring_batch.multimesh.set_instance_custom_data(instance_index,
				Color(float(level), float(sector), 0.0, 0.0))
		ring_batch.multimesh.visible_instance_count = 0
		ring_batch.visible = false
		_sector_batches.append(ring_batch)
		add_child(ring_batch)


func _update_active_levels() -> void:
	var target_radius: float = maxf(_visible_cap_arc_m,
		_base_spacing * float(HALF_CELLS))
	var level: int = 0
	var outer: float = _base_spacing * float(HALF_CELLS)
	while level < MAX_LEVEL and outer < target_radius:
		level += 1
		outer *= 2.0
	_active_max_level = level
	for batch: MultiMeshInstance3D in _sector_batches:
		if batch.multimesh != null:
			batch.multimesh.visible_instance_count = _active_max_level


func _set_visible(value: bool) -> void:
	_terrain_visible = value
	if not value:
		_visible_sector_count = 0
		for batch: MultiMeshInstance3D in _center_sector_batches:
			batch.visible = false
		for batch: MultiMeshInstance3D in _sector_batches:
			batch.visible = false
		return
	_update_sector_visibility()


func _update_sector_visibility() -> void:
	if not _terrain_visible:
		return

	var camera: Camera3D = get_viewport().get_camera_3d()
	if camera == null:
		return
	var forward: Vector3 = -camera.global_transform.basis.z.normalized()
	var radial_dot: float = forward.dot(_center_dir)
	var forward_plane := Vector2(forward.dot(_center_right), forward.dot(_center_up))
	var show_all: bool = absf(radial_dot) >= SECTOR_SHOW_ALL_RADIAL_DOT \
		or forward_plane.length_squared() < 1e-5

	var cos_limit: float = -1.0
	var forward_2d := Vector2.RIGHT
	if not show_all:
		forward_2d = forward_plane.normalized()
		var viewport_size: Vector2 = get_viewport().get_visible_rect().size
		var aspect: float = viewport_size.x / maxf(viewport_size.y, 1.0)
		var vertical_half: float = deg_to_rad(camera.fov) * 0.5
		var horizontal_half: float = atan(tan(vertical_half) * aspect)
		var limit: float = minf(horizontal_half + SECTOR_HALF_ANGLE
			+ SECTOR_CULL_MARGIN_RAD, PI)
		cos_limit = cos(limit)

	_visible_sector_count = 0
	for sector: int in SECTOR_COUNT:
		var sector_visible: bool = show_all
		if not show_all:
			var angle: float = (float(sector) + 0.5) * TAU / float(SECTOR_COUNT)
			var sector_dir := Vector2(cos(angle), sin(angle))
			sector_visible = sector_dir.dot(forward_2d) >= cos_limit
		_center_sector_batches[sector].visible = sector_visible
		_sector_batches[sector].visible = sector_visible and _active_max_level > 0
		if sector_visible:
			_visible_sector_count += 1


func _show_all_active_sectors() -> void:
	_visible_sector_count = 0
	for sector: int in SECTOR_COUNT:
		# The debug cut removes +clipmap-up, corresponding to sectors 0..5.
		var sector_visible: bool = not _debug_side_cut or sector >= (SECTOR_COUNT >> 1)
		_center_sector_batches[sector].visible = sector_visible
		_sector_batches[sector].visible = sector_visible and _active_max_level > 0
		if _sector_batches[sector].multimesh != null:
			_sector_batches[sector].multimesh.visible_instance_count = \
				_active_max_level if sector_visible else 0
		if sector_visible:
			_visible_sector_count += 1


func rebuild_static_topology() -> void:
	for sector: int in SECTOR_COUNT:
		var center: MultiMeshInstance3D = _center_sector_batches[sector]
		if center.multimesh != null:
			center.multimesh.mesh = _build_compact_sector_mesh(
				sector, false, _debug_side_cut)
		var ring: MultiMeshInstance3D = _sector_batches[sector]
		if ring.multimesh != null:
			ring.multimesh.mesh = _build_compact_sector_mesh(
				sector, true, _debug_side_cut)


static func _build_compact_sector_mesh(sector_index: int, ring: bool,
		half_cut: bool) -> ArrayMesh:
	var vertices := PackedVector3Array()
	var uvs := PackedVector2Array()
	var indices := PackedInt32Array()
	var remap: Dictionary = {}
	var outer_sq: float = float(HALF_CELLS * HALF_CELLS)
	var inner_sq: float = float(RING_INNER_HALF_CELLS * RING_INNER_HALF_CELLS)

	for y: int in GRID_CELLS:
		var cy: float = float(y) + 0.5 - float(HALF_CELLS)
		if half_cut and cy > 0.0:
			continue
		for x: int in GRID_CELLS:
			var cx: float = float(x) + 0.5 - float(HALF_CELLS)
			var r_sq: float = cx * cx + cy * cy
			if r_sq > outer_sq:
				continue
			if ring and r_sq < inner_sq:
				continue

			var angle: float = atan2(cy, cx)
			if angle < 0.0:
				angle += TAU
			var owner_sector: int = int(floor(angle / TAU * float(SECTOR_COUNT)))
			owner_sector = clampi(owner_sector, 0, SECTOR_COUNT - 1)
			if owner_sector != sector_index:
				continue

			var i00: int = _compact_vertex(remap, vertices, uvs, x, y)
			var i10: int = _compact_vertex(remap, vertices, uvs, x + 1, y)
			var i01: int = _compact_vertex(remap, vertices, uvs, x, y + 1)
			var i11: int = _compact_vertex(remap, vertices, uvs, x + 1, y + 1)
			indices.append(i00)
			indices.append(i10)
			indices.append(i11)
			indices.append(i00)
			indices.append(i11)
			indices.append(i01)

	var mesh := ArrayMesh.new()
	if indices.is_empty():
		return mesh
	var arrays: Array = []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = vertices
	arrays[Mesh.ARRAY_TEX_UV] = uvs
	arrays[Mesh.ARRAY_INDEX] = indices
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	return mesh


static func _compact_vertex(remap: Dictionary, vertices: PackedVector3Array,
		uvs: PackedVector2Array, gx: int, gy: int) -> int:
	var logical_index: int = gy * GRID_VERTS + gx
	var existing: Variant = remap.get(logical_index, null)
	if existing != null:
		return int(existing)
	var local_index: int = vertices.size()
	remap[logical_index] = local_index
	# Position is only a placeholder; the vertex shader reconstructs the spherical
	# position. UV carries the original logical coordinate exactly.
	vertices.append(Vector3.ZERO)
	uvs.append(Vector2(float(gx), float(gy)))
	return local_index


func gpu_stream_stats() -> Dictionary:
	var out: Dictionary = super.gpu_stream_stats()
	out["draw_batches"] = _visible_sector_count * (2 if _active_max_level > 0 else 1)
	out["explicit_grid_coords"] = true
	out["compact_sector_indices"] = true
	return out
