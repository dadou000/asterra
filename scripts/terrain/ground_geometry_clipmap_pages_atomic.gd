extends "res://scripts/terrain/ground_geometry_clipmap_pages_debugsafe.gd"
## Atomic-centre GPU-page clipmap.
##
## A clipmap centre is published only after the complete L0-L6 working set for
## that centre is GPU-resident. The previous direct-page renderer moved the mesh
## centre immediately, then waited for pages; at a page boundary the geometry
## could therefore walk out of the resident page set and disappear while the
## global terrain had already been cut away.
##
## This renderer keeps two centres:
##   desired   = where the camera wants the clipmap to move next
##   published = the last centre whose full page set is known-good
##
## While desired is still loading, the published clipmap remains intact and the
## current/global terrain remains a valid fallback. On a warm/prebaked world the
## commit normally happens on the first residency check.

const ATOMIC_REQUEST_INTERVAL: float = 0.05
const ATOMIC_COVERAGE_INTERVAL: float = 0.05
const ATOMIC_GRID_STEPS: int = 7
const ATOMIC_VISIBLE_PRIORITY: float = -250.0
const ATOMIC_KEEP_PRIORITY: float = -180.0

var _desired_center := Vector2.ZERO
var _atomic_request_left: float = 0.0
var _atomic_coverage_left: float = 0.0
var _atomic_coverage_mask: int = 0
var _atomic_desired_ready: bool = false


func _reset_frame(center: Vector3) -> void:
	super._reset_frame(center)
	_desired_center = Vector2.ZERO
	_atomic_request_left = 0.0
	_atomic_coverage_left = 0.0
	_atomic_coverage_mask = 0
	_atomic_desired_ready = false


func _on_world_ready(fields: PlanetFields) -> void:
	super._on_world_ready(fields)
	_desired_center = Vector2.ZERO
	_atomic_request_left = 0.0
	_atomic_coverage_left = 0.0
	_atomic_coverage_mask = 0
	_atomic_desired_ready = false


func _process(dt: float) -> void:
	if not Planet.ready_state or Planet.cfg == null:
		_set_active(false)
		return

	var camera: Camera3D = get_viewport().get_camera_3d()
	if camera == null:
		_set_active(false)
		return

	var origin := Vector3(float(Frames.origin.x), float(Frames.origin.y), float(Frames.origin.z))
	var planet_pos: Vector3 = camera.global_position + origin
	if planet_pos.length_squared() <= 1.0:
		_set_active(false)
		return

	var radius: float = Planet.cfg.planet_radius
	var observer_dir: Vector3 = planet_pos.normalized()
	var agl: float = maxf(planet_pos.length() - radius - Planet.macro_height(observer_dir), 0.0)
	if agl > ACTIVE_AGL_M:
		_set_active(false)
		return

	if not _have_frame:
		_reset_frame(observer_dir)

	var observer_surface: Vector3 = observer_dir * radius
	var frame_surface: Vector3 = _frame_dir * radius
	var rel: Vector3 = observer_surface - frame_surface
	var px: float = rel.dot(_frame_right)
	var py: float = rel.dot(_frame_up)

	if absf(px) > REANCHOR_M or absf(py) > REANCHOR_M:
		_reset_frame(observer_dir)
		px = 0.0
		py = 0.0

	var target := Vector2(
		round(px / _base_spacing) * _base_spacing,
		round(py / _base_spacing) * _base_spacing)
	if target.distance_squared_to(_desired_center) > 1e-8:
		_desired_center = target
		_requested_center = target
		_atomic_desired_ready = false
		_atomic_coverage_left = 0.0

	_bind_page_resources(false)

	_atomic_request_left -= dt
	if _atomic_request_left <= 0.0:
		_atomic_request_left = ATOMIC_REQUEST_INTERVAL
		_request_center_pages(_desired_center, ATOMIC_VISIBLE_PRIORITY)
		# If the desired centre is not ready yet, keep the currently published
		# working set hot so it cannot be displaced while we wait for the new set.
		if _published_center.distance_squared_to(_desired_center) > 1e-8:
			_request_center_pages(_published_center, ATOMIC_KEEP_PRIORITY)

	_atomic_coverage_left -= dt
	if _coverage_dirty or _atomic_coverage_left <= 0.0:
		_coverage_dirty = false
		_atomic_coverage_left = ATOMIC_COVERAGE_INTERVAL
		_atomic_desired_ready = _center_has_complete_working_set(_desired_center, true)
		if _atomic_desired_ready:
			_published_center = _desired_center
			_requested_center = _desired_center
			_coverage_ready = true
		else:
			# Do not move a valid local clipmap into an incomplete page set. If the
			# existing centre itself became invalid, disable the local renderer so
			# the uncut global quadtree is the fallback instead of showing a hole.
			_coverage_ready = _center_has_complete_working_set(_published_center, false)

	_sync_page_uniforms()
	_sync_common_uniforms(origin)
	_sync_material_control()
	_set_active(_coverage_ready and GroundHeightPageAtlas.ready_for_shader())


func _request_visible_pages() -> void:
	# _process() owns page requests because it must distinguish desired from
	# published centres. Keep this override so the inherited direct-page request
	# path cannot accidentally move back to published-only requests.
	_request_center_pages(_desired_center, ATOMIC_VISIBLE_PRIORITY)


func _check_backing_coverage() -> bool:
	return _center_has_complete_working_set(_published_center, false)


func _request_center_pages(center: Vector2, priority: float) -> void:
	if not _have_frame or Planet.cfg == null:
		return
	for level: int in STORAGE_LEVELS:
		var directions: Array[Vector3] = _footprint_directions_at(
			center, ATOMIC_GRID_STEPS, _half_extent_for_level(level))
		GroundHeightStore.request_samples(directions, level, priority)
		GroundHeightPageAtlas.touch_samples(directions, level)


func _center_has_complete_working_set(center: Vector2, request_missing: bool) -> bool:
	if not _have_frame or not GroundHeightPageAtlas.ready_for_shader():
		_atomic_coverage_mask = 0
		return false

	var mask: int = 0
	var all_present: bool = true
	for level: int in STORAGE_LEVELS:
		var directions: Array[Vector3] = _footprint_directions_at(
			center, ATOMIC_GRID_STEPS, _half_extent_for_level(level))
		if request_missing:
			GroundHeightStore.request_samples(directions, level, ATOMIC_VISIBLE_PRIORITY)
		var level_present: bool = GroundHeightPageAtlas.touch_samples(directions, level)
		if level_present:
			mask |= 1 << level
		else:
			all_present = false
	_atomic_coverage_mask = mask
	return all_present


func _half_extent_for_level(level: int) -> float:
	if level == STORAGE_LEVELS - 1:
		# L6 is only the backing source for the visible L5 footprint.
		return (float(GRID_CELLS) * 0.5 + FOOTPRINT_MARGIN_CELLS) \
			* _base_spacing * pow(2.0, float(RENDER_LEVELS - 1))
	return _full_level_half_extent(level)


func _footprint_directions_at(center: Vector2, steps: int,
		half_extent: float) -> Array[Vector3]:
	var result: Array[Vector3] = []
	if steps < 2 or Planet.cfg == null:
		return result
	var radius: float = Planet.cfg.planet_radius
	var denom: float = float(steps - 1)
	for yi: int in steps:
		var fy: float = -1.0 + 2.0 * float(yi) / denom
		for xi: int in steps:
			var fx: float = -1.0 + 2.0 * float(xi) / denom
			var plane: Vector2 = center + Vector2(fx, fy) * half_extent
			result.append(_direction_for_plane_static(
				_frame_dir, _frame_right, _frame_up, plane, radius))
	return result


func gpu_stream_stats() -> Dictionary:
	var result: Dictionary = super.gpu_stream_stats()
	result["coverage_mask"] = _atomic_coverage_mask
	result["coverage_full_mask"] = (1 << STORAGE_LEVELS) - 1
	result["desired_ready"] = _atomic_desired_ready
	result["center_lag_m"] = _published_center.distance_to(_desired_center)
	return result
