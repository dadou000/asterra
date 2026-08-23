extends "res://scripts/terrain/ground_geometry_clipmap_pages.gd"
## Camera-following sparse-page ground clipmap.
##
## The render centre and the streaming state are deliberately independent:
## - geometry follows the observer immediately on the finest (~0.75 m) lattice;
## - L6 (~48 m) is the mandatory safety backing for the whole local footprint;
## - L0-L5 are best-effort refinement and may arrive later;
## - if L6 is not ready, the local clipmap is hidden and the uncut global terrain
##   remains visible. The renderer never freezes an old local tile while waiting.
##
## This replaces the experimental atomic-centre implementation. That version
## required a complete L0-L6 working set before moving the mesh, which made the
## desired centre move faster than the streamer could ever commit it.

const FOLLOW_REQUEST_INTERVAL: float = 0.05
const FOLLOW_GUARD_INTERVAL: float = 0.05
const FOLLOW_FINE_GRID_STEPS: int = 5
const FOLLOW_GUARD_GRID_STEPS: int = 7
const FOLLOW_GUARD_PRIORITY: float = -500.0
const FOLLOW_COARSE_PRIORITY: float = -180.0
const FOLLOW_FINE_PRIORITY: float = -40.0
const HANDOFF_CUT_FRACTION: float = 0.94

var _follow_request_left: float = 0.0
var _follow_guard_left: float = 0.0
var _guard_ready: bool = false
var _level_batches: Array[MultiMeshInstance3D] = []


func _reset_frame(center: Vector3) -> void:
	super._reset_frame(center)
	_follow_request_left = 0.0
	_follow_guard_left = 0.0
	_guard_ready = false


func _on_world_ready(fields: PlanetFields) -> void:
	super._on_world_ready(fields)
	_follow_request_left = 0.0
	_follow_guard_left = 0.0
	_guard_ready = false


func _process(dt: float) -> void:
	if not Planet.ready_state or Planet.cfg == null:
		_guard_ready = false
		_set_active(false)
		return

	var camera: Camera3D = get_viewport().get_camera_3d()
	if camera == null:
		_guard_ready = false
		_set_active(false)
		return

	var origin := Vector3(float(Frames.origin.x), float(Frames.origin.y), float(Frames.origin.z))
	var planet_pos: Vector3 = camera.global_position + origin
	if planet_pos.length_squared() <= 1.0:
		_guard_ready = false
		_set_active(false)
		return

	var radius: float = Planet.cfg.planet_radius
	var observer_dir: Vector3 = planet_pos.normalized()
	var agl: float = maxf(planet_pos.length() - radius - Planet.macro_height(observer_dir), 0.0)
	if agl > ACTIVE_AGL_M:
		_guard_ready = false
		_set_active(false)
		return

	if not _have_frame:
		_reset_frame(observer_dir)

	var observer_surface: Vector3 = observer_dir * radius
	var frame_surface: Vector3 = _frame_dir * radius
	var rel: Vector3 = observer_surface - frame_surface
	var px: float = rel.dot(_frame_right)
	var py: float = rel.dot(_frame_up)

	# Re-anchor only the tangent frame. The sparse page addresses are global and
	# survive this operation; the new centre starts at (0,0) under the observer.
	if absf(px) > REANCHOR_M or absf(py) > REANCHOR_M:
		_reset_frame(observer_dir)
		px = 0.0
		py = 0.0

	# IMPORTANT: this is a render decision, not a streaming commit. The mesh must
	# follow the camera even if fine pages are still loading. The shader already
	# falls back through coarser levels, so waiting for L0-L5 here is incorrect.
	var target := Vector2(
		round(px / _base_spacing) * _base_spacing,
		round(py / _base_spacing) * _base_spacing)
	if target.distance_squared_to(_published_center) > 1e-8:
		_published_center = target
		_requested_center = target

	_bind_page_resources(false)

	_follow_request_left -= dt
	if _follow_request_left <= 0.0:
		_follow_request_left = FOLLOW_REQUEST_INTERVAL
		_request_visible_pages()

	_follow_guard_left -= dt
	if _coverage_dirty or _follow_guard_left <= 0.0:
		_coverage_dirty = false
		_follow_guard_left = FOLLOW_GUARD_INTERVAL
		_guard_ready = _check_backing_coverage()
		_coverage_ready = _guard_ready

	_sync_page_uniforms()
	_sync_common_uniforms(origin)
	_sync_material_control()
	_set_active(_guard_ready and GroundHeightPageAtlas.ready_for_shader())


## Keep every visible LOD warm, but do not make fine residency a condition for
## movement. Coarser levels are requested first and with stronger priority so the
## fallback chain becomes usable before spending bandwidth on 0.75 m detail.
func _request_visible_pages() -> void:
	if not _have_frame or Planet.cfg == null:
		return

	# Mandatory backing first.
	var guard_dirs: Array[Vector3] = _footprint_directions(
		STORAGE_LEVELS - 1, FOLLOW_GUARD_GRID_STEPS, _guard_half_extent())
	GroundHeightStore.request_samples(guard_dirs, STORAGE_LEVELS - 1, FOLLOW_GUARD_PRIORITY)
	GroundHeightPageAtlas.touch_samples(guard_dirs, STORAGE_LEVELS - 1)

	# L5 -> L0. The shader can render at whatever level has arrived so these are
	# refinement requests, not movement barriers.
	for level: int in range(RENDER_LEVELS - 1, -1, -1):
		var directions: Array[Vector3] = _footprint_directions(
			level, FOLLOW_FINE_GRID_STEPS, _full_level_half_extent(level))
		var priority: float = FOLLOW_COARSE_PRIORITY if level >= 3 else FOLLOW_FINE_PRIORITY
		GroundHeightStore.request_samples(directions, level, priority)
		GroundHeightPageAtlas.touch_samples(directions, level)


## The only hard residency requirement is the coarse L6 backing covering the
## complete local L5 footprint (including the normal/morph sampling margin).
func _check_backing_coverage() -> bool:
	if not _have_frame or not GroundHeightPageAtlas.ready_for_shader():
		return false
	var directions: Array[Vector3] = _footprint_directions(
		STORAGE_LEVELS - 1, FOLLOW_GUARD_GRID_STEPS, _guard_half_extent())
	GroundHeightStore.request_samples(directions, STORAGE_LEVELS - 1, FOLLOW_GUARD_PRIORITY)
	return GroundHeightPageAtlas.touch_samples(directions, STORAGE_LEVELS - 1)


func _guard_half_extent() -> float:
	var outer_spacing: float = _base_spacing * pow(2.0, float(RENDER_LEVELS - 1))
	return (float(GRID_CELLS) * 0.5 + FOOTPRINT_MARGIN_CELLS) * outer_spacing


## Keep the six levels independently culled until the previous MultiMesh ring
## batching issue is revisited. This is six tiny submissions but is deterministic
## and makes wireframe inspection unambiguous.
func _build_ring_nodes() -> void:
	_level_batches.clear()
	var full: ArrayMesh = _build_strip_mesh(false)
	var ring: ArrayMesh = _build_strip_mesh(true)
	var bounds := AABB(
		Vector3(-100000.0, -100000.0, -100000.0),
		Vector3(200000.0, 200000.0, 200000.0))

	for level: int in RENDER_LEVELS:
		var mesh: ArrayMesh = full if level == 0 else ring
		var batch: MultiMeshInstance3D = _make_single_level_batch(
			"GroundClipmapL%d" % level, mesh, level,
			level == RENDER_LEVELS - 1, bounds)
		_level_batches.append(batch)
		add_child(batch)


func _make_single_level_batch(node_name: String, mesh: ArrayMesh, level: int,
		outer: bool, bounds: AABB) -> MultiMeshInstance3D:
	var mm := MultiMesh.new()
	mm.transform_format = MultiMesh.TRANSFORM_3D
	mm.use_custom_data = true
	mm.mesh = mesh
	mm.custom_aabb = bounds
	mm.instance_count = 1
	mm.visible_instance_count = 1
	mm.set_instance_transform(0, Transform3D.IDENTITY)
	mm.set_instance_custom_data(0, Color(
		float(level), 1.0 if outer else 0.0, 0.0, 0.0))

	var batch := MultiMeshInstance3D.new()
	batch.name = node_name
	batch.multimesh = mm
	batch.material_override = _material
	return batch


func _set_visible(value: bool) -> void:
	for batch: MultiMeshInstance3D in _level_batches:
		batch.visible = value


## Remove the coarse global mesh only under the opaque local region. If the L6
## guard becomes incomplete, _set_active(false) disables this cutout first, so a
## streamer miss can degrade to global terrain but cannot create a black hole.
func _sync_global_cutout(enabled: bool) -> void:
	var terrain: PlanetTerrain = _terrain_ref.get_ref() if _terrain_ref != null else null
	if terrain == null:
		terrain = _find_terrain(get_tree().root)
		if terrain != null:
			_terrain_ref = weakref(terrain)
	if terrain == null:
		return
	var mats: Array = terrain.debug_materials()
	if mats.is_empty():
		return
	var ground: ShaderMaterial = mats[0]
	ground.set_shader_parameter("u_ground_clipmap_cutout", 1.0 if enabled else 0.0)
	if not enabled:
		return

	var outer_spacing: float = _base_spacing * pow(2.0, float(RENDER_LEVELS - 1))
	var outer_half: float = float(GRID_CELLS) * 0.5 * outer_spacing
	ground.set_shader_parameter("u_ground_clipmap_frame_dir", _frame_dir)
	ground.set_shader_parameter("u_ground_clipmap_right", _frame_right)
	ground.set_shader_parameter("u_ground_clipmap_up", _frame_up)
	ground.set_shader_parameter("u_ground_clipmap_center_plane", _published_center)
	ground.set_shader_parameter("u_ground_clipmap_cut_half_extent",
		outer_half * HANDOFF_CUT_FRACTION)


func gpu_stream_stats() -> Dictionary:
	var result: Dictionary = super.gpu_stream_stats()
	result["draw_batches"] = _level_batches.size()
	result["coverage_ready"] = _guard_ready
	result["guard_ready"] = _guard_ready
	result["center_follow_lag_m"] = 0.0
	return result
