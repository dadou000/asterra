extends Node3D
## Unified camera-centred spherical geometry clipmap.
##
## This is the only visual terrain renderer. L0 is a circular 400-cell-diameter
## grid disc and L1..L14 share a concentric annular topology. All positions are
## reconstructed in the vertex shader from VERTEX_ID and projected onto the
## planet. Cube faces exist only in the sparse height-page address space.
##
## Adjacent levels overlap. Fine geometry morphs to its parent over the outer
## 12.5%, while the coarse ring begins inside that region and is radially sunk.
## At the shared circular boundary both levels evaluate the same parent height,
## eliminating cracks without quadtree stitching or skirts.

const TARGET_FINE_DEPTH: int = 16
const MAX_LEVEL: int = 14
const LEVEL_COUNT: int = MAX_LEVEL + 1

# 400 cells is deliberate for the 4K/68deg target: the outgoing fine level is
# ~8 px/vertex at its outer edge and the incoming 2x-coarser level is ~16 px.
const GRID_CELLS: int = 400
const GRID_VERTS: int = GRID_CELLS + 1
const HALF_CELLS: int = GRID_CELLS >> 1
const RING_INNER_HALF_CELLS: int = 88

const REANCHOR_M: float = 8000.0
const HORIZON_MARGIN_M: float = 20000.0
const REQUEST_INTERVAL: float = 0.20
const REQUEST_GRID_STEPS: int = 33
const REQUEST_PRIORITY_BASE: float = -600.0
const REQUEST_PRIORITY_LEVEL_STEP: float = 12.0
const GLOBAL_BOUNDS_M: float = 4000000.0

const MATERIAL_RES: float = 128.0
const MATERIAL_TEXEL_M := Vector3(8.0, 128.0, 2048.0)

var _material: ShaderMaterial
var _center_batch: MultiMeshInstance3D
var _ring_batch: MultiMeshInstance3D

var _anchor_dir := Vector3(1.0, 0.0, 0.0)
var _anchor_right := Vector3(0.0, 0.0, -1.0)
var _anchor_up := Vector3(0.0, 1.0, 0.0)
var _center_dir := Vector3(1.0, 0.0, 0.0)
var _center_right := Vector3(0.0, 0.0, -1.0)
var _center_up := Vector3(0.0, 1.0, 0.0)
var _center_plane := Vector2.ZERO
var _have_anchor := false
var _base_spacing: float = 0.75

var _active_max_level: int = 0
var _visible_cap_arc_m: float = 0.0
var _request_left: float = 0.0
var _height_enabled := true

var _bound_atlas: Texture2D
var _bound_table: Texture2D
var _bound_orbit: Texture2DArray
var _bound_orbit_res: int = -1
var _last_material_control: Texture2DArray


func _ready() -> void:
	process_priority = 9
	_material = ShaderMaterial.new()
	_material.shader = load("res://shaders/spherical_geometry_clipmap.gdshader")
	_build_batches()
	_set_visible(false)

	Planet.world_ready.connect(_on_world_ready)
	Planet.coast_profile_changed.connect(_on_coast_profile_changed)
	Frames.origin_shifted.connect(_on_origin_shifted)
	Deltas.region_changed.connect(_on_region_changed)

	if Planet.ready_state and Planet.cfg != null:
		_configure_world()


func _process(dt: float) -> void:
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

	# Keep sub-metre snapping in a stable local tangent frame. Reanchoring changes
	# only uniforms; no geometry or height pages are rebuilt.
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
		_request_left = 0.0

	_update_visible_cap(planet_pos.length(), radius)
	_update_active_levels()
	_bind_gpu_resources(false)
	_sync_uniforms(origin)
	_sync_material_control()

	_request_left -= dt
	if _request_left <= 0.0:
		_request_left = REQUEST_INTERVAL
		_request_visible_pages()

	_set_visible(_bound_orbit != null or GroundHeightPageAtlas.ready_for_shader())


func _configure_world() -> void:
	_base_spacing = PI * 0.5 * Planet.cfg.planet_radius \
		/ (float(Planet.cfg.chunk_grid) * pow(2.0, float(TARGET_FINE_DEPTH)))
	_have_anchor = false
	_request_left = 0.0
	_bound_atlas = null
	_bound_table = null
	_bound_orbit = null
	_bound_orbit_res = -1
	_bind_gpu_resources(true)


func _reset_anchor(observer_dir: Vector3) -> void:
	_anchor_dir = observer_dir.normalized()
	var tangent: Array = CubeSphere.tangent_basis(_anchor_dir)
	_anchor_right = tangent[0]
	_anchor_up = tangent[1]
	_center_plane = Vector2.ZERO
	_center_dir = _anchor_dir
	_center_right = _anchor_right
	_center_up = _anchor_up
	_have_anchor = true
	_request_left = 0.0


func _update_center_basis() -> void:
	_center_dir = _direction_for_offset(_anchor_dir, _anchor_right, _anchor_up,
		_center_plane, Planet.cfg.planet_radius)
	var tangent: Array = CubeSphere.tangent_basis(_center_dir)
	_center_right = tangent[0]
	_center_up = tangent[1]


func _update_visible_cap(observer_radius: float, planet_radius: float) -> void:
	var safe_r: float = maxf(observer_radius, planet_radius + 0.01)
	var horizon_angle: float = acos(clampf(planet_radius / safe_r, -1.0, 1.0))
	var horizon_arc: float = horizon_angle * planet_radius
	_visible_cap_arc_m = minf(PI * 0.5 * planet_radius,
		horizon_arc + HORIZON_MARGIN_M)


func _update_active_levels() -> void:
	var target_radius: float = maxf(_visible_cap_arc_m, _base_spacing * float(HALF_CELLS))
	var level: int = 0
	var outer: float = _base_spacing * float(HALF_CELLS)
	while level < MAX_LEVEL and outer < target_radius:
		level += 1
		outer *= 2.0
	_active_max_level = level

	if _ring_batch != null and _ring_batch.multimesh != null:
		# Ring instance 0 is L1, so the visible count equals active max level.
		_ring_batch.multimesh.visible_instance_count = _active_max_level


func _bind_gpu_resources(force: bool) -> void:
	var atlas: Texture2D = GroundHeightPageAtlas.atlas_texture()
	var table: Texture2D = GroundHeightPageAtlas.page_table_texture()
	if force or atlas != _bound_atlas:
		_bound_atlas = atlas
		_material.set_shader_parameter("u_height_page_atlas", atlas)
	if force or table != _bound_table:
		_bound_table = table
		_material.set_shader_parameter("u_height_page_table", table)

	var orbit: Texture2DArray = Planet.orbit_elevation_texture
	var orbit_res: int = Planet.orbit_texture_face_res
	if force or orbit != _bound_orbit:
		_bound_orbit = orbit
		_material.set_shader_parameter("u_orbit_elevation", orbit)
	if force or orbit_res != _bound_orbit_res:
		_bound_orbit_res = orbit_res
		_material.set_shader_parameter("u_orbit_face_res", float(orbit_res))


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

	_material.set_shader_parameter("u_page_atlas_ready",
		1.0 if GroundHeightPageAtlas.ready_for_shader() else 0.0)
	_material.set_shader_parameter("u_page_atlas_size", GroundHeightPageAtlas.atlas_size())
	_material.set_shader_parameter("u_page_table_capacity",
		float(GroundHeightPageAtlas.table_capacity()))
	_material.set_shader_parameter("u_page_table_probes",
		float(GroundHeightPageAtlas.table_max_probes()))
	_material.set_shader_parameter("u_page_atlas_cols",
		float(GroundHeightPageAtlas.atlas_columns()))
	_material.set_shader_parameter("u_page_tile_cells", 32.0)
	_material.set_shader_parameter("u_page_tile_verts", 33.0)
	_material.set_shader_parameter("u_finest_cells",
		float(GroundHeightStore.cells_per_face(0)))

	_material.set_shader_parameter("u_orbit_ready", 1.0 if _bound_orbit != null else 0.0)


func _sync_material_control() -> void:
	var source: Node = get_node_or_null("/root/MaterialClipmap")
	if source == null:
		_material.set_shader_parameter("u_material_clipmap_ready", 0.0)
		return
	var value: Variant = source.get("_texture")
	if not (value is Texture2DArray):
		_material.set_shader_parameter("u_material_clipmap_ready", 0.0)
		return
	var texture: Texture2DArray = value
	if texture == _last_material_control:
		return
	_last_material_control = texture
	_material.set_shader_parameter("u_material_clipmap", texture)
	_material.set_shader_parameter("u_material_clipmap_ready", 1.0)
	_material.set_shader_parameter("u_material_center", source.get("_center"))
	_material.set_shader_parameter("u_material_right", source.get("_right"))
	_material.set_shader_parameter("u_material_up", source.get("_up"))
	_material.set_shader_parameter("u_material_res", MATERIAL_RES)
	_material.set_shader_parameter("u_material_texel_m", MATERIAL_TEXEL_M)


func _request_visible_pages() -> void:
	if not _have_anchor or Planet.cfg == null:
		return
	var request_max: int = mini(_active_max_level + 1, MAX_LEVEL)
	for level: int in range(0, request_max + 1):
		var directions: Array[Vector3] = _request_directions_for_level(level)
		if directions.is_empty():
			continue
		var priority: float = REQUEST_PRIORITY_BASE + float(level) * REQUEST_PRIORITY_LEVEL_STEP
		GroundHeightStore.request_samples(directions, level, priority)
		GroundHeightPageAtlas.touch_samples(directions, level)


func _request_directions_for_level(level: int) -> Array[Vector3]:
	var result: Array[Vector3] = []
	var spacing: float = _base_spacing * pow(2.0, float(level))
	var level_half: float = float(HALF_CELLS) * spacing
	var half: float = minf(level_half * 1.02, _visible_cap_arc_m * 1.08)
	var radial_limit: float = minf(level_half * 1.03, _visible_cap_arc_m * 1.08)
	var denom: float = float(REQUEST_GRID_STEPS - 1)
	for yi: int in REQUEST_GRID_STEPS:
		var fy: float = -1.0 + 2.0 * float(yi) / denom
		for xi: int in REQUEST_GRID_STEPS:
			var fx: float = -1.0 + 2.0 * float(xi) / denom
			var offset := Vector2(fx, fy) * half
			if offset.length() > radial_limit:
				continue
			result.append(_direction_for_offset(_center_dir, _center_right, _center_up,
				offset, Planet.cfg.planet_radius))
	return result


static func _direction_for_offset(center: Vector3, right: Vector3, up: Vector3,
		offset_m: Vector2, radius: float) -> Vector3:
	var arc: float = offset_m.length()
	if arc <= 1e-6:
		return center.normalized()
	var theta: float = arc / maxf(radius, 1.0)
	var tangent: Vector3 = (right * offset_m.x + up * offset_m.y).normalized()
	return (center * cos(theta) + tangent * sin(theta)).normalized()


func _build_batches() -> void:
	var full: ArrayMesh = _build_strip_mesh(false)
	var ring: ArrayMesh = _build_strip_mesh(true)
	var bounds := AABB(
		Vector3(-GLOBAL_BOUNDS_M, -GLOBAL_BOUNDS_M, -GLOBAL_BOUNDS_M),
		Vector3(GLOBAL_BOUNDS_M * 2.0, GLOBAL_BOUNDS_M * 2.0, GLOBAL_BOUNDS_M * 2.0))

	_center_batch = _make_batch("SphericalClipmapL0", full, 1, bounds)
	_center_batch.multimesh.set_instance_custom_data(0, Color(0.0, 0.0, 0.0, 0.0))
	add_child(_center_batch)

	_ring_batch = _make_batch("SphericalClipmapRings", ring, MAX_LEVEL, bounds)
	for instance_index: int in MAX_LEVEL:
		var level: int = instance_index + 1
		_ring_batch.multimesh.set_instance_custom_data(instance_index,
			Color(float(level), 0.0, 0.0, 0.0))
	_ring_batch.multimesh.visible_instance_count = 0
	add_child(_ring_batch)


func _make_batch(node_name: String, mesh: ArrayMesh, count: int,
		bounds: AABB) -> MultiMeshInstance3D:
	var mm := MultiMesh.new()
	mm.transform_format = MultiMesh.TRANSFORM_3D
	mm.use_custom_data = true
	mm.mesh = mesh
	mm.custom_aabb = bounds
	mm.instance_count = count
	mm.visible_instance_count = count
	for instance_index: int in count:
		mm.set_instance_transform(instance_index, Transform3D.IDENTITY)

	var batch := MultiMeshInstance3D.new()
	batch.name = node_name
	batch.multimesh = mm
	batch.material_override = _material
	# The terrain is camera-centred and displaced entirely in its vertex shader.
	# Treating it as static GI geometry makes SDFGI rebuild its cascades while the
	# observer moves, even though terrain lighting is already handled by the planet
	# shader. It should receive GI, but must not be voxelized as a contributor.
	batch.gi_mode = GeometryInstance3D.GI_MODE_DISABLED
	batch.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_ON
	return batch


static func _build_strip_mesh(with_hole: bool) -> ArrayMesh:
	var vertices := PackedVector3Array()
	vertices.resize(GRID_VERTS * GRID_VERTS)
	var indices := PackedInt32Array()
	if with_hole:
		_build_ring_strip_indices(indices)
	else:
		_build_full_strip_indices(indices)
	var arrays: Array = []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = vertices
	arrays[Mesh.ARRAY_INDEX] = indices
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	return mesh


static func _build_full_strip_indices(indices: PackedInt32Array) -> void:
	var outer_sq: float = float(HALF_CELLS * HALF_CELLS)
	for y: int in GRID_CELLS:
		var cy: float = float(y) + 0.5 - float(HALF_CELLS)
		for x: int in GRID_CELLS:
			var cx: float = float(x) + 0.5 - float(HALF_CELLS)
			if cx * cx + cy * cy > outer_sq:
				continue
			_append_cell_triangles(indices, x, y)


static func _build_ring_strip_indices(indices: PackedInt32Array) -> void:
	var outer_sq: float = float(HALF_CELLS * HALF_CELLS)
	var inner_sq: float = float(RING_INNER_HALF_CELLS * RING_INNER_HALF_CELLS)
	for y: int in GRID_CELLS:
		var cy: float = float(y) + 0.5 - float(HALF_CELLS)
		for x: int in GRID_CELLS:
			var cx: float = float(x) + 0.5 - float(HALF_CELLS)
			var r_sq: float = cx * cx + cy * cy
			if r_sq > outer_sq or r_sq < inner_sq:
				continue
			_append_cell_triangles(indices, x, y)


static func _append_cell_triangles(indices: PackedInt32Array, x: int, y: int) -> void:
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


func rebuild_static_topology() -> void:
	# Used by wireframe debug after RenderingServer wireframe generation is enabled.
	if _center_batch != null and _center_batch.multimesh != null:
		_center_batch.multimesh.mesh = _build_strip_mesh(false)
	if _ring_batch != null and _ring_batch.multimesh != null:
		_ring_batch.multimesh.mesh = _build_strip_mesh(true)


func set_heightmap_enabled(value: bool) -> void:
	_height_enabled = value
	if _material != null:
		_material.set_shader_parameter("u_height_enabled", 1.0 if value else 0.0)


func terrain_material() -> ShaderMaterial:
	return _material


func _set_visible(value: bool) -> void:
	if _center_batch != null:
		_center_batch.visible = value
	if _ring_batch != null:
		_ring_batch.visible = value


func _on_world_ready(_fields: PlanetFields) -> void:
	_configure_world()


func _on_coast_profile_changed() -> void:
	_configure_world()


func _on_origin_shifted(_delta: Vector3) -> void:
	# Positions are reconstructed from planet coordinates every frame; the next
	# process pass publishes the new origin uniform. No mesh movement is required.
	pass


func _on_region_changed(_center: Vector3, _radius_m: float) -> void:
	# GroundHeightPageAtlas refreshes edited resident pages directly.
	_request_left = 0.0


func gpu_stream_stats() -> Dictionary:
	var page_stats: Dictionary = GroundHeightPageAtlas.stats()
	return {
		"draw_batches": 2,
		"active_levels": _active_max_level + 1,
		"max_level": MAX_LEVEL,
		"visible_cap_km": _visible_cap_arc_m / 1000.0,
		"page_resident": int(page_stats.get("resident_pages", 0)),
		"page_capacity": int(page_stats.get("slot_capacity", 0)),
		"page_uploads": int(page_stats.get("pages_uploaded", 0)),
		"page_reuploads": int(page_stats.get("page_reuploads", 0)),
		"page_evictions": int(page_stats.get("evictions", 0)),
		"page_texels": int(page_stats.get("uploaded_texels", 0)),
		"table_updates": int(page_stats.get("table_updates", 0)),
		"table_rebuilds": int(page_stats.get("table_rebuilds", 0)),
		"table_tombstones": int(page_stats.get("table_tombstones", 0)),
		"table_failures": int(page_stats.get("table_insert_failures", 0)),
		"coverage_ready": _bound_orbit != null or GroundHeightPageAtlas.ready_for_shader(),
		"spherical": true,
		"grid_cells": GRID_CELLS,
		"concentric": true,
	}
