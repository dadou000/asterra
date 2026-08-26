extends "res://scripts/terrain/spherical_geometry_clipmap_cached.gd"
## Final horizon-safe terrain layer.
##
## Horizon distance is allowed to choose how many logical rings exist, but it must
## never alter rendered vertex positions or shorten the submitted ring prefix. The
## previous dynamic visible-cap uniform made far spherical projection follow camera
## altitude, so descending could pull the apparent terrain edge toward the player.
## The inherited nadir optimisation also shortened the ring prefix from AGL. Both
## are disabled here without changing the proven cache, handoff, topology or LOD
## process chain.
##
## Sector topology is authored in the stable anchor tangent frame. Camera azimuth
## culling must use that same frame: using the moving centre tangent frame can rotate
## the cull wedges away from the actual compact sector meshes after long movement.
##
## Stable near-field displacement uses a gnomonic tangent coordinate in the shader:
## offset = R * tan(theta). Older CPU tracking projected the surface chord instead,
## giving R * sin(theta). That error is tiny at the original 8 km anchor distance but
## reaches roughly one complete L0 radius near the current 65 km anchor boundary.
## This layer keeps centre tracking and the A/B handoff in the shader's exact frame.

const RENDER_HEMISPHERE_CAP_RAD: float = PI * 0.5
const GNOMONIC_DENOM_MIN: float = 1e-5
const REANCHOR_SOLVE_STEPS: int = 26

var _sector_cull_failsafe_frames: int = 0
var _l0_center_projection_corrections: int = 0


func _gnomonic_offset_from_dir(surface_dir: Vector3, anchor_dir: Vector3,
		right: Vector3, up: Vector3) -> Vector2:
	if Planet.cfg == null:
		return Vector2.ZERO
	var denom: float = surface_dir.dot(anchor_dir)
	if not is_finite(denom) or denom <= GNOMONIC_DENOM_MIN:
		return Vector2.ZERO
	var scale: float = Planet.cfg.planet_radius / denom
	return Vector2(surface_dir.dot(right) * scale, surface_dir.dot(up) * scale)


func _gnomonic_offset_from_world(surface_world: Vec3D, anchor_dir: Vector3,
		right: Vector3, up: Vector3) -> Vector2:
	var surface_dir: Vector3 = surface_world.normalized().to_v3()
	return _gnomonic_offset_from_dir(surface_dir, anchor_dir, right, up)


func _project_surface_to_active_anchor(surface_world: Vec3D) -> Vector2:
	return _gnomonic_offset_from_world(
		surface_world, _anchor_dir, _anchor_right, _anchor_up)


func _project_surface_to_pending_anchor(surface_world: Vec3D) -> Vector2:
	return _gnomonic_offset_from_world(
		surface_world, _pending_anchor_dir, _pending_anchor_right, _pending_anchor_up)


func _project_surface_to_anchor(surface_world: Vec3D, _anchor_world: Vec3D,
		anchor_dir: Vector3) -> Vector2:
	var tangent: Array = CubeSphere.tangent_basis(anchor_dir)
	return _gnomonic_offset_from_world(surface_world, anchor_dir, tangent[0], tangent[1])


func _update_center_basis() -> void:
	if Planet.cfg == null:
		return
	# Exact inverse of the shader's near-field direction_for_basis_offset():
	# normalize(anchor + right*x/R + up*y/R).
	var radius: float = maxf(Planet.cfg.planet_radius, 1.0)
	_center_dir = (_anchor_dir
		+ _anchor_right * (_center_plane.x / radius)
		+ _anchor_up * (_center_plane.y / radius)).normalized()
	var tangent: Array = CubeSphere.tangent_basis(_center_dir)
	_center_right = tangent[0]
	_center_up = tangent[1]


func _correct_l0_center_to_observer() -> void:
	if _debug_freeze or Planet.cfg == null or not Planet.ready_state:
		return
	var state: Dictionary = _observer_surface_state()
	if state.is_empty():
		return
	var observer_dir: Vector3 = state["observer_dir"]
	var exact_offset: Vector2 = _gnomonic_offset_from_dir(
		observer_dir, _anchor_dir, _anchor_right, _anchor_up)
	var snapped := Vector2(
		round(exact_offset.x / _base_spacing) * _base_spacing,
		round(exact_offset.y / _base_spacing) * _base_spacing)
	if snapped.distance_squared_to(_center_plane) <= 1e-8:
		return
	_center_plane = snapped
	_update_center_basis()
	_l0_center_projection_corrections += 1

	# The parent has already published uniforms earlier in this frame. Republish only
	# the position-dependent terrain uniforms after correcting the centre; rendering
	# and the cache update below will therefore consume one coherent coordinate frame.
	var origin := Vector3(float(Frames.origin.x), float(Frames.origin.y), float(Frames.origin.z))
	_sync_uniforms(origin)


func _update_terrain_caches() -> void:
	# cached.gd calls this once at the end of its normal process chain, after handoff
	# planning and before rendering. It is the safest point to replace the parent's
	# old chord-projected centre with the exact shader-space centre.
	_correct_l0_center_to_observer()
	super._update_terrain_caches()


func _parent_reanchor_metric(plane_offset: Vector2) -> float:
	# procedural.gd currently detects reanchor from the tangent component of the
	# surface chord. Convert our exact gnomonic coordinate back to that quantity so
	# the staging target lands exactly where the parent will actually reset.
	if Planet.cfg == null:
		return 0.0
	var radius: float = maxf(Planet.cfg.planet_radius, 1.0)
	var inv_root: float = 1.0 / sqrt(1.0 + plane_offset.length_squared() / (radius * radius))
	return maxf(absf(plane_offset.x), absf(plane_offset.y)) * inv_root


func _predict_handoff_target(plane_offset: Vector2, heading: Vector2) -> Vector2:
	if heading.length_squared() < 0.5:
		return plane_offset

	# Find the exact gnomonic coordinate at which the unchanged parent process sees
	# its chord-projected REANCHOR_M threshold. Binary search is cheap (one call only
	# while planning/retargeting) and avoids a 100+ m staging error near 65 km.
	if _parent_reanchor_metric(plane_offset) < REANCHOR_M:
		var lo: float = 0.0
		var hi: float = maxf(REANCHOR_M * 0.25, 4096.0)
		for grow: int in 8:
			if _parent_reanchor_metric(plane_offset + heading * hi) >= REANCHOR_M:
				break
			hi *= 2.0
		for iteration: int in REANCHOR_SOLVE_STEPS:
			var mid: float = (lo + hi) * 0.5
			if _parent_reanchor_metric(plane_offset + heading * mid) >= REANCHOR_M:
				hi = mid
			else:
				lo = mid
		return plane_offset + heading * hi

	# Same overdue behaviour as cached.gd: if a warm handoff was missed, stage under
	# a stationary observer or lead a moving observer by one estimated warm-up span.
	if _last_motion_m < HANDOFF_MIN_MOTION_M:
		return plane_offset
	var lead: float = clampf(
		_last_motion_m * HANDOFF_WARM_FRAMES_ESTIMATE,
		HANDOFF_OVERDUE_LEAD_MIN_M, HANDOFF_OVERDUE_LEAD_MAX_M)
	return plane_offset + heading * lead


func _start_handoff(target_offset: Vector2, retarget: bool) -> void:
	if Planet.cfg == null:
		return
	if retarget:
		_handoff_retargets += 1
	_handoff_active = true
	_handoff_ready = false
	_handoff_staging_failed = false
	_handoff_quiet_frames = 0
	_pending_target_offset = target_offset

	# Exact inverse of the shader/CPU gnomonic mapping.
	var radius: float = maxf(Planet.cfg.planet_radius, 1.0)
	_pending_anchor_dir = (_anchor_dir
		+ _anchor_right * (target_offset.x / radius)
		+ _anchor_up * (target_offset.y / radius)).normalized()
	var tangent: Array = CubeSphere.tangent_basis(_pending_anchor_dir)
	_pending_anchor_right = tangent[0]
	_pending_anchor_up = tangent[1]
	_pending_anchor_world = Vec3D.from_v3(_pending_anchor_dir).mul(radius)


func _sync_uniforms(origin: Vector3) -> void:
	super._sync_uniforms(origin)
	if _material != null:
		# Keep the shader's geometric projection/cap independent of observer altitude.
		# _visible_cap_arc_m still drives automatic ring-count selection on the CPU.
		_material.set_shader_parameter("u_visible_cap_angle", RENDER_HEMISPHERE_CAP_RAD)


func _update_sector_visibility() -> void:
	# Keep global_gpu authoritative for sky/underground rejection and any other
	# whole-surface view decisions, then replace only the azimuth wedge selection.
	super._update_sector_visibility()
	if _view_surface_culled:
		return

	# Side-cut is an explicit topology debug mode; do not reinterpret its visibility.
	if _debug_side_cut:
		_set_view_ring_prefix(_physical_ring_count)
		return

	var camera: Camera3D = get_viewport().get_camera_3d()
	if camera == null:
		return

	var forward: Vector3 = -camera.global_transform.basis.z.normalized()
	# IMPORTANT: compact sector ownership is defined by cell angle in the stable
	# anchor basis (anchor_right/anchor_up), not the moving centre tangent basis.
	var radial_dot: float = forward.dot(_anchor_dir)
	var forward_plane := Vector2(
		forward.dot(_anchor_right),
		forward.dot(_anchor_up))

	var show_all: bool = not is_finite(radial_dot) \
		or not is_finite(forward_plane.x) or not is_finite(forward_plane.y) \
		or absf(radial_dot) >= SECTOR_SHOW_ALL_RADIAL_DOT \
		or forward_plane.length_squared() < 1e-6

	var cos_limit: float = -1.0
	var forward_2d := Vector2.RIGHT
	if not show_all:
		forward_2d = forward_plane.normalized()
		var viewport_size: Vector2 = get_viewport().get_visible_rect().size
		var viewport_h: float = maxf(viewport_size.y, 1.0)
		var aspect: float = viewport_size.x / viewport_h
		var vertical_half: float = deg_to_rad(camera.fov) * 0.5
		var horizontal_half: float
		if camera.keep_aspect == Camera3D.KEEP_WIDTH:
			horizontal_half = vertical_half
		else:
			horizontal_half = atan(tan(vertical_half) * aspect)
		var limit: float = minf(
			horizontal_half + SECTOR_HALF_ANGLE + SECTOR_CULL_MARGIN_RAD,
			PI)
		cos_limit = cos(limit)

	var visible_mask := PackedByteArray()
	visible_mask.resize(SECTOR_COUNT)
	var visible_count: int = 0
	for sector: int in SECTOR_COUNT:
		var sector_visible: bool = show_all
		if not show_all:
			var angle: float = (float(sector) + 0.5) * TAU / float(SECTOR_COUNT)
			var sector_dir := Vector2(cos(angle), sin(angle))
			sector_visible = sector_dir.dot(forward_2d) >= cos_limit
		visible_mask[sector] = 1 if sector_visible else 0
		if sector_visible:
			visible_count += 1

	# A valid horizontal view can never mathematically select zero 30-degree
	# sectors with the current FOV/margins. Treat that state as numerical/stale-frame
	# corruption and fail open for one frame instead of making L0 disappear.
	if visible_count <= 0:
		_sector_cull_failsafe_frames += 1
		visible_count = SECTOR_COUNT
		for sector: int in SECTOR_COUNT:
			visible_mask[sector] = 1

	var ring_count: int = _active_ring_count()
	_visible_sector_count = visible_count
	for sector: int in SECTOR_COUNT:
		var sector_visible: bool = visible_mask[sector] != 0
		_center_sector_batches[sector].visible = sector_visible
		_sector_batches[sector].visible = sector_visible and ring_count > 0

	# Preserve the complete logical ring stack selected by the geometric horizon.
	# The old AGL-dependent nadir prefix is intentionally disabled in this layer.
	_set_view_ring_prefix(_physical_ring_count)


func gpu_stream_stats() -> Dictionary:
	var out: Dictionary = super.gpu_stream_stats()
	out["terrain_horizon_safe"] = true
	out["terrain_render_cap_dynamic"] = false
	out["terrain_nadir_ring_prefix_cull"] = false
	out["terrain_sector_cull_anchor_basis"] = true
	out["terrain_sector_cull_failsafe_frames"] = _sector_cull_failsafe_frames
	out["terrain_l0_gnomonic_center"] = true
	out["terrain_l0_center_corrections"] = _l0_center_projection_corrections
	return out
