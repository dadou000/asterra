extends "res://scripts/terrain/spherical_geometry_clipmap.gd"
## Playability-oriented performance pass for the 400-cell spherical clipmap.
##
## Geometry remains at the 4K/16-px target, but the topology is genuinely
## concentric: L0 is a circular disc and every outer LOD is a circular annulus.
## Outer annuli are partitioned into angular sectors so terrain well outside the
## camera's horizontal view is not submitted to the vertex shader.
##
## Runtime visual streaming is bounded: sparse refinement is requested only
## through L6 (~48 m spacing); L7+ uses orbit elevation immediately. Broad visual
## requests are disk/RAM-only in GroundHeightStore.

const FAST_HORIZON_MARGIN_M: float = 2500.0
# 15 samples across 400 cells gives ~28.6 cells/probe, smaller than the 32-cell
# height page. The previous 9x9 lattice could skip entire cached pages.
const FAST_REQUEST_GRID_STEPS: int = 15
const MIN_VISIBLE_REQUEST_INTERVAL_MS: int = 250
const REQUEST_INNER_SKIP_Q: float = 0.42
const REQUEST_RADIAL_PRIORITY_SPAN: float = 80.0
const SPARSE_VISUAL_MAX_LEVEL: int = 6
const FAST_MATERIAL_RES: float = 64.0
const FAST_MATERIAL_TEXEL_M := Vector3(16.0, 256.0, 4096.0)

# Twelve 30-degree wedges keep culling useful without turning the terrain into a
# draw-call-heavy renderer. At a normal horizontal view roughly half are visible.
const SECTOR_COUNT: int = 12
const SECTOR_HALF_ANGLE: float = 0.2617993877991494
const SECTOR_CULL_MARGIN_RAD: float = 0.3490658503988659
const SECTOR_SHOW_ALL_RADIAL_DOT: float = 0.65

var _last_visible_request_msec: int = -1000000
var _sector_batches: Array[MultiMeshInstance3D] = []
var _terrain_visible: bool = false
var _visible_sector_count: int = 0


func _ready() -> void:
	super._ready()
	_material.shader = load("res://shaders/spherical_geometry_clipmap_fast.gdshader")

	# Until a cheap dedicated terrain-shadow representation exists, do not run the
	# expensive displaced clipmap through directional shadow cascades.
	if _center_batch != null:
		_center_batch.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	for batch: MultiMeshInstance3D in _sector_batches:
		batch.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF


func _process(dt: float) -> void:
	super._process(dt)
	if _terrain_visible:
		_update_sector_visibility()


func _build_batches() -> void:
	_sector_batches.clear()
	_ring_batch = null
	var full: ArrayMesh = _build_strip_mesh(false)
	var bounds := AABB(
		Vector3(-GLOBAL_BOUNDS_M, -GLOBAL_BOUNDS_M, -GLOBAL_BOUNDS_M),
		Vector3(GLOBAL_BOUNDS_M * 2.0, GLOBAL_BOUNDS_M * 2.0, GLOBAL_BOUNDS_M * 2.0))

	_center_batch = _make_batch("SphericalClipmapL0", full, 1, bounds)
	_center_batch.multimesh.set_instance_custom_data(0, Color(0.0, 0.0, 0.0, 0.0))
	add_child(_center_batch)

	for sector: int in SECTOR_COUNT:
		var mesh: ArrayMesh = _build_sector_mesh(sector)
		var batch: MultiMeshInstance3D = _make_batch(
			"SphericalClipmapSector%02d" % sector, mesh, MAX_LEVEL, bounds)
		for instance_index: int in MAX_LEVEL:
			var level: int = instance_index + 1
			batch.multimesh.set_instance_custom_data(instance_index,
				Color(float(level), float(sector), 0.0, 0.0))
		batch.multimesh.visible_instance_count = 0
		batch.visible = false
		_sector_batches.append(batch)
		add_child(batch)


static func _build_sector_mesh(sector_index: int) -> ArrayMesh:
	# Keep the full logical vertex lattice so VERTEX_ID remains the exact grid
	# coordinate used by the shader. Only the sector's annular cells are indexed,
	# therefore unreferenced square-grid vertices never reach the vertex shader.
	var vertices := PackedVector3Array()
	vertices.resize(GRID_VERTS * GRID_VERTS)
	var indices := PackedInt32Array()
	var outer_sq: float = float(HALF_CELLS * HALF_CELLS)
	var inner_sq: float = float(RING_INNER_HALF_CELLS * RING_INNER_HALF_CELLS)

	for y: int in GRID_CELLS:
		var cy: float = float(y) + 0.5 - float(HALF_CELLS)
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
			_append_sector_cell(indices, x, y)

	var arrays: Array = []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = vertices
	arrays[Mesh.ARRAY_INDEX] = indices
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	return mesh


static func _append_sector_cell(indices: PackedInt32Array, x: int, y: int) -> void:
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


func _update_active_levels() -> void:
	var target_radius: float = maxf(_visible_cap_arc_m, _base_spacing * float(HALF_CELLS))
	var level: int = 0
	var outer: float = _base_spacing * float(HALF_CELLS)
	while level < MAX_LEVEL and outer < target_radius:
		level += 1
		outer *= 2.0
	_active_max_level = level
	for batch: MultiMeshInstance3D in _sector_batches:
		if batch.multimesh != null:
			# Instance 0 is L1, so active_max_level is also the instance count.
			batch.multimesh.visible_instance_count = _active_max_level


func _set_visible(value: bool) -> void:
	_terrain_visible = value
	if _center_batch != null:
		_center_batch.visible = value
	if not value:
		_visible_sector_count = 0
		for batch: MultiMeshInstance3D in _sector_batches:
			batch.visible = false
		return
	_update_sector_visibility()


func _update_sector_visibility() -> void:
	if not _terrain_visible or _active_max_level <= 0:
		_visible_sector_count = 0
		for batch: MultiMeshInstance3D in _sector_batches:
			batch.visible = false
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
	for sector: int in _sector_batches.size():
		var sector_visible: bool = show_all
		if not show_all:
			var angle: float = (float(sector) + 0.5) * TAU / float(SECTOR_COUNT)
			var sector_dir := Vector2(cos(angle), sin(angle))
			sector_visible = sector_dir.dot(forward_2d) >= cos_limit
		_sector_batches[sector].visible = sector_visible
		if sector_visible:
			_visible_sector_count += 1


func _update_visible_cap(observer_radius: float, planet_radius: float) -> void:
	var safe_r: float = maxf(observer_radius, planet_radius + 0.01)
	var horizon_angle: float = acos(clampf(planet_radius / safe_r, -1.0, 1.0))
	var horizon_arc: float = horizon_angle * planet_radius
	_visible_cap_arc_m = minf(PI * 0.5 * planet_radius,
		horizon_arc + FAST_HORIZON_MARGIN_M)


func _request_visible_pages() -> void:
	if not _have_anchor or Planet.cfg == null:
		return
	var now: int = Time.get_ticks_msec()
	if now - _last_visible_request_msec < MIN_VISIBLE_REQUEST_INTERVAL_MS:
		return
	_last_visible_request_msec = now

	# Do not ask the sparse cache to cover the entire horizon. L7+ is intentionally
	# handled by the orbit macro texture in the shader.
	var request_max: int = mini(_active_max_level + 1, SPARSE_VISUAL_MAX_LEVEL)
	for level: int in range(0, request_max + 1):
		var directions: Array[Vector3] = _request_directions_for_level(level)
		if directions.is_empty():
			continue
		var base_priority: float = REQUEST_PRIORITY_BASE + float(level) * REQUEST_PRIORITY_LEVEL_STEP
		var level_half: float = maxf(float(HALF_CELLS) * _base_spacing
			* pow(2.0, float(level)), 1.0)
		var priorities := PackedFloat32Array()
		priorities.resize(directions.size())
		for i: int in directions.size():
			var angular_distance: float = acos(clampf(_center_dir.dot(directions[i]), -1.0, 1.0))
			var distance_m: float = angular_distance * Planet.cfg.planet_radius
			var radial_q: float = clampf(distance_m / level_half, 0.0, 1.0)
			priorities[i] = base_priority + radial_q * REQUEST_RADIAL_PRIORITY_SPAN

		if GroundHeightStore.has_method("request_samples_prioritized"):
			GroundHeightStore.request_samples_prioritized(directions, level, priorities)
		else:
			GroundHeightStore.request_samples(directions, level, base_priority)
		GroundHeightPageAtlas.touch_samples(directions, level)


func _request_directions_for_level(level: int) -> Array[Vector3]:
	var result: Array[Vector3] = []
	if level > SPARSE_VISUAL_MAX_LEVEL:
		return result
	var spacing: float = _base_spacing * pow(2.0, float(level))
	var level_half: float = float(HALF_CELLS) * spacing
	var half: float = minf(level_half * 1.015, _visible_cap_arc_m * 1.04)
	var radial_limit: float = minf(level_half * 1.03, _visible_cap_arc_m * 1.04)
	var denom: float = float(FAST_REQUEST_GRID_STEPS - 1)
	for yi: int in FAST_REQUEST_GRID_STEPS:
		var fy: float = -1.0 + 2.0 * float(yi) / denom
		for xi: int in FAST_REQUEST_GRID_STEPS:
			var fx: float = -1.0 + 2.0 * float(xi) / denom
			var normalized_offset := Vector2(fx, fy)
			# L1+ are circular annuli; don't probe the disc already represented by
			# finer LODs.
			if level > 0 and normalized_offset.length() < REQUEST_INNER_SKIP_Q:
				continue
			var offset: Vector2 = normalized_offset * half
			if offset.length() > radial_limit:
				continue
			result.append(_direction_for_offset(_center_dir, _center_right, _center_up,
				offset, Planet.cfg.planet_radius))
	return result


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
	_material.set_shader_parameter("u_material_res", FAST_MATERIAL_RES)
	_material.set_shader_parameter("u_material_texel_m", FAST_MATERIAL_TEXEL_M)


func rebuild_static_topology() -> void:
	if _center_batch != null and _center_batch.multimesh != null:
		_center_batch.multimesh.mesh = _build_strip_mesh(false)
	for sector: int in _sector_batches.size():
		var batch: MultiMeshInstance3D = _sector_batches[sector]
		if batch.multimesh != null:
			batch.multimesh.mesh = _build_sector_mesh(sector)


func gpu_stream_stats() -> Dictionary:
	var out: Dictionary = super.gpu_stream_stats()
	out["fast_path"] = true
	out["request_grid"] = FAST_REQUEST_GRID_STEPS
	out["far_shadow_pass"] = false
	out["sparse_visual_max_level"] = SPARSE_VISUAL_MAX_LEVEL
	out["concentric"] = true
	out["sector_count"] = SECTOR_COUNT
	out["visible_sectors"] = _visible_sector_count
	out["draw_batches"] = 1 + _visible_sector_count if _active_max_level > 0 else 1
	return out
