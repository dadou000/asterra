extends "res://scripts/terrain/spherical_geometry_clipmap_micro.gd"
## Final cached terrain-rendering layer.
##
## The proven micro/global renderer remains authoritative for process order,
## topology, LOD, snapping, culling and debug controls. This layer only replaces
## the surface shader and owns two incremental GPU caches. The normal parent
## reanchor is allowed to run, but before the frame renders we either replace it
## with an already-warm staged anchor or restore the previous anchor. The visible
## cache is therefore never invalidated by an ordinary high-speed reanchor.

const CACHED_SURFACE_SHADER_PATH := "res://shaders/spherical_geometry_clipmap_cached_surface.gdshader"
const TerrainClipmapCacheScript := preload("res://scripts/terrain/gpu_terrain_clipmap_cache.gd")

# Start staging very early. A full 15-layer 512x512 cache is about 3.9 M samples;
# the cache's own scheduler already spreads that work over many frames.
const HANDOFF_PREWARM_M: float = 8192.0
const HANDOFF_RETARGET_M: float = 8192.0
const HANDOFF_SWAP_MAX_OFFSET_M: float = 16.0
const HANDOFF_MIN_MOTION_M: float = 0.5
const HANDOFF_QUIET_FRAMES: int = 2
const HANDOFF_WARM_FRAMES_ESTIMATE: float = 360.0
const HANDOFF_OVERDUE_LEAD_MIN_M: float = 2048.0
const HANDOFF_OVERDUE_LEAD_MAX_M: float = 32768.0
# Do not keep an old tangent anchor indefinitely after a teleport or failed cache.
# Normal high-speed travel should never reach this because staging begins at 8 km.
const HANDOFF_MAX_HOLD_M: float = 86000.0

var _terrain_cache_active: Node
var _terrain_cache_staging: Node
var _bound_cache_texture: Variant = null
var _bound_cache_generation := -1
var _bound_cache_res := -1
var _bound_cache_ready := false

var _handoff_active := false
var _handoff_ready := false
var _handoff_staging_failed := false
var _handoff_quiet_frames := 0
var _handoff_swaps := 0
var _handoff_retargets := 0
var _handoff_delayed_parent_resets := 0
var _handoff_cold_fallbacks := 0
var _pending_target_offset := Vector2.ZERO
var _pending_anchor_dir := Vector3(1.0, 0.0, 0.0)
var _pending_anchor_right := Vector3(0.0, 0.0, -1.0)
var _pending_anchor_up := Vector3(0.0, 1.0, 0.0)
var _pending_anchor_world: Vec3D = Vec3D.new()
var _last_plane_offset := Vector2.ZERO
var _last_plane_valid := false
var _last_motion_m := 0.0


func _ready() -> void:
	# Build the proven production renderer first. Replacing only the material here
	# leaves the complete parent process/culling chain unchanged.
	super._ready()
	_material.shader = load(CACHED_SURFACE_SHADER_PATH)

	# Shader replacement clears every parameter binding; restore the same resources
	# that global_gpu normally owns before enabling either cache.
	_bind_gpu_resources(true)
	_sync_material_control()
	_bind_gpu_context(true)
	_bind_surface_pbr(true)
	_sync_detail_seed()
	_sync_debug_uniforms()
	_material.set_shader_parameter("u_terrain_cache_ready", 0.0)

	# Allocate both cache nodes at startup. Only A is visible. B remains invisible
	# until prewarm starts, then becomes the next active cache after an atomic swap.
	_terrain_cache_active = TerrainClipmapCacheScript.new()
	_terrain_cache_active.name = "GPUTerrainClipmapCacheA"
	add_child(_terrain_cache_active)
	_terrain_cache_staging = TerrainClipmapCacheScript.new()
	_terrain_cache_staging.name = "GPUTerrainClipmapCacheB"
	add_child(_terrain_cache_staging)


func _process(dt: float) -> void:
	# Capture only the anchor state. The parent still executes its exact proven
	# process path, including all visibility, ring and microgeometry updates.
	var had_anchor: bool = _have_anchor
	var previous_anchor_dir: Vector3 = _anchor_dir
	var previous_anchor_world: Vec3D = _stable_anchor_world.dup()

	super._process(dt)

	if _material == null or Planet.cfg == null or not Planet.ready_state:
		return

	var observer_state: Dictionary = _observer_surface_state()
	if observer_state.is_empty():
		_update_terrain_caches()
		return

	var parent_reanchored: bool = had_anchor \
		and previous_anchor_dir.distance_squared_to(_anchor_dir) > 1e-10
	if parent_reanchored:
		_resolve_parent_reanchor(previous_anchor_dir, previous_anchor_world, observer_state)

	# Recompute against the final anchor chosen for this frame, then stage the next
	# destination. This happens after parent rendering state is valid, never instead
	# of it.
	var surface_world: Vec3D = observer_state["surface_world"]
	var plane_offset: Vector2 = _project_surface_to_active_anchor(surface_world)
	_plan_handoff(plane_offset)
	_update_terrain_caches()


func _observer_surface_state() -> Dictionary:
	var camera: Camera3D = get_viewport().get_camera_3d()
	if camera == null or Planet.cfg == null:
		return {}
	var observer_world: Vec3D = Frames.to_world(camera.global_position)
	var observer_radius: float = observer_world.length()
	if observer_radius <= 1.0:
		return {}
	var observer_unit: Vec3D = observer_world.normalized()
	return {
		"observer_radius": observer_radius,
		"observer_dir": observer_unit.to_v3(),
		"surface_world": observer_unit.mul(Planet.cfg.planet_radius),
	}


func _project_surface_to_active_anchor(surface_world: Vec3D) -> Vector2:
	var rel: Vec3D = surface_world.sub(_stable_anchor_world)
	return Vector2(
		rel.x * _anchor_right.x + rel.y * _anchor_right.y + rel.z * _anchor_right.z,
		rel.x * _anchor_up.x + rel.y * _anchor_up.y + rel.z * _anchor_up.z)


func _project_surface_to_pending_anchor(surface_world: Vec3D) -> Vector2:
	var rel: Vec3D = surface_world.sub(_pending_anchor_world)
	return Vector2(
		rel.x * _pending_anchor_right.x + rel.y * _pending_anchor_right.y
			+ rel.z * _pending_anchor_right.z,
		rel.x * _pending_anchor_up.x + rel.y * _pending_anchor_up.y
			+ rel.z * _pending_anchor_up.z)


func _resolve_parent_reanchor(previous_dir: Vector3, previous_world: Vec3D,
		observer_state: Dictionary) -> void:
	var surface_world: Vec3D = observer_state["surface_world"]
	var previous_offset: Vector2 = _project_surface_to_anchor(
		surface_world, previous_world, previous_dir)
	var previous_extent: float = maxf(absf(previous_offset.x), absf(previous_offset.y))

	# A completely warm staged cache may replace the temporary parent reset, but
	# only when the observer is essentially at its centre. That keeps the new L0
	# window hot instead of immediately exposing a large uncached strip.
	if _handoff_active and _handoff_ready:
		var staged_offset: Vector2 = _project_surface_to_pending_anchor(surface_world)
		if maxf(absf(staged_offset.x), absf(staged_offset.y)) <= HANDOFF_SWAP_MAX_OFFSET_M:
			_commit_staged_anchor(observer_state)
			return
		# The old prediction was missed (usually a sharp turn). Never expose it just
		# because it is warm; discard the plan and build a new target while retaining
		# the old visible anchor.
		_cancel_handoff()

	# Cache failure or a giant teleport must not hold an old tangent frame forever.
	# In those exceptional cases accept the parent's normal immediate reset and let
	# the analytic miss fallback cover the cold cache exactly as before.
	if _staging_cache_failed() or previous_extent > HANDOFF_MAX_HOLD_M:
		_handoff_cold_fallbacks += 1
		_cancel_handoff()
		_last_plane_valid = false
		return

	# Undo only the parent's temporary anchor reset. No cache update has happened yet
	# this frame, so the active cache still contains the correct previous-anchor data.
	_reset_anchor(previous_dir)
	_capture_stable_anchor(previous_world)
	_republish_anchor_state(observer_state)
	_handoff_delayed_parent_resets += 1


func _project_surface_to_anchor(surface_world: Vec3D, anchor_world: Vec3D,
		anchor_dir: Vector3) -> Vector2:
	var tangent: Array = CubeSphere.tangent_basis(anchor_dir)
	var right: Vector3 = tangent[0]
	var up: Vector3 = tangent[1]
	var rel: Vec3D = surface_world.sub(anchor_world)
	return Vector2(
		rel.x * right.x + rel.y * right.y + rel.z * right.z,
		rel.x * up.x + rel.y * up.y + rel.z * up.z)


func _republish_anchor_state(observer_state: Dictionary) -> void:
	var surface_world: Vec3D = observer_state["surface_world"]
	var observer_radius: float = float(observer_state["observer_radius"])
	var offset: Vector2 = _project_surface_to_active_anchor(surface_world)
	_center_plane = Vector2(
		round(offset.x / _base_spacing) * _base_spacing,
		round(offset.y / _base_spacing) * _base_spacing)
	_update_center_basis()
	_update_visible_cap(observer_radius, Planet.cfg.planet_radius)
	_update_active_levels()

	var origin := Vector3(float(Frames.origin.x), float(Frames.origin.y), float(Frames.origin.z))
	_sync_uniforms(origin)
	_sync_material_control()
	if _terrain_visible:
		if _debug_side_cut:
			_show_all_active_sectors()
		else:
			_update_sector_visibility()


func _plan_handoff(plane_offset: Vector2) -> void:
	if _debug_freeze or Planet.cfg == null:
		return

	var motion := Vector2.ZERO
	if _last_plane_valid:
		motion = plane_offset - _last_plane_offset
	_last_plane_offset = plane_offset
	_last_plane_valid = true
	_last_motion_m = motion.length()

	var extent: float = maxf(absf(plane_offset.x), absf(plane_offset.y))
	var heading := Vector2.ZERO
	if motion.length() >= HANDOFF_MIN_MOTION_M:
		heading = motion.normalized()
	elif plane_offset.length() > 1e-6:
		heading = plane_offset.normalized()

	if not _handoff_active:
		if extent >= HANDOFF_PREWARM_M:
			_start_handoff(_predict_handoff_target(plane_offset, heading), false)
		return

	if _handoff_ready:
		return

	var revised_target: Vector2 = _predict_handoff_target(plane_offset, heading)
	if revised_target.distance_to(_pending_target_offset) >= HANDOFF_RETARGET_M:
		_start_handoff(revised_target, true)


func _predict_handoff_target(plane_offset: Vector2, heading: Vector2) -> Vector2:
	var extent: float = maxf(absf(plane_offset.x), absf(plane_offset.y))
	if heading.length_squared() < 0.5:
		return plane_offset

	# Before the normal threshold, predict the exact tangent-plane point where the
	# moving observer will hit the old anchor's square reanchor boundary.
	if extent < REANCHOR_M:
		var best_t: float = INF
		if absf(heading.x) > 1e-6:
			var edge_x: float = REANCHOR_M if heading.x > 0.0 else -REANCHOR_M
			var tx: float = (edge_x - plane_offset.x) / heading.x
			if tx > 0.0:
				best_t = minf(best_t, tx)
		if absf(heading.y) > 1e-6:
			var edge_y: float = REANCHOR_M if heading.y > 0.0 else -REANCHOR_M
			var ty: float = (edge_y - plane_offset.y) / heading.y
			if ty > 0.0:
				best_t = minf(best_t, ty)
		if is_finite(best_t):
			return plane_offset + heading * best_t

	# If we deliberately held the old anchor past its normal threshold, aim ahead by
	# approximately one full staggered warm-up duration. A stationary observer simply
	# stages exactly under itself and swaps as soon as that cache finishes.
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

	# Exact inverse of tangent-plane projection onto a sphere. Using normalize(R*n +
	# tangent) would introduce a measurable position error near a 65 km boundary.
	var radius: float = Planet.cfg.planet_radius
	var tangent_sq: float = target_offset.length_squared()
	var radial: float = sqrt(maxf(radius * radius - tangent_sq, 1.0))
	var target_surface: Vector3 = _anchor_dir * radial \
		+ _anchor_right * target_offset.x + _anchor_up * target_offset.y
	_pending_anchor_dir = target_surface.normalized()
	var tangent: Array = CubeSphere.tangent_basis(_pending_anchor_dir)
	_pending_anchor_right = tangent[0]
	_pending_anchor_up = tangent[1]
	_pending_anchor_world = Vec3D.from_v3(_pending_anchor_dir).mul(radius)


func _commit_staged_anchor(observer_state: Dictionary) -> void:
	# Cache ownership and tangent state switch in the same script frame, before the
	# renderer consumes either one. The previous active cache becomes the next spare.
	var previous_active: Node = _terrain_cache_active
	_terrain_cache_active = _terrain_cache_staging
	_terrain_cache_staging = previous_active

	_reset_anchor(_pending_anchor_dir)
	_capture_stable_anchor(_pending_anchor_world)
	_republish_anchor_state(observer_state)
	_handoff_swaps += 1
	_cancel_handoff()
	_last_plane_valid = false
	_force_cache_rebind()


func _cancel_handoff() -> void:
	_handoff_active = false
	_handoff_ready = false
	_handoff_staging_failed = false
	_handoff_quiet_frames = 0
	_pending_target_offset = Vector2.ZERO


func _staging_cache_failed() -> bool:
	return _terrain_cache_staging != null and is_instance_valid(_terrain_cache_staging) \
		and bool(_terrain_cache_staging.get("failed"))


func _force_cache_rebind() -> void:
	_bound_cache_texture = null
	_bound_cache_generation = -1
	_bound_cache_res = -1
	_bound_cache_ready = false
	if _material != null:
		_material.set_shader_parameter("u_terrain_cache_ready", 0.0)


func _update_terrain_caches() -> void:
	if _terrain_cache_active == null or not is_instance_valid(_terrain_cache_active) \
			or _material == null or Planet.cfg == null or not Planet.ready_state:
		return

	# The visible cache follows the final anchor selected for this frame.
	_terrain_cache_active.call("update_cache",
		_anchor_dir, _anchor_right, _anchor_up,
		_center_plane, _base_spacing,
		_active_min_level, _active_max_level)
	_bind_active_cache()

	# The spare cache remains centred on the predicted handoff point. Its own cache
	# scheduler rotates LODs and has a fixed per-frame dispatch budget, so prewarming
	# is naturally staggered instead of becoming one large GPU spike.
	if _handoff_active and not _debug_freeze \
			and _terrain_cache_staging != null and is_instance_valid(_terrain_cache_staging):
		_terrain_cache_staging.call("update_cache",
			_pending_anchor_dir, _pending_anchor_right, _pending_anchor_up,
			Vector2.ZERO, _base_spacing,
			_active_min_level, _active_max_level)
		_update_staging_readiness()


func _bind_active_cache() -> void:
	var texture: Variant = _terrain_cache_active.call("texture")
	if texture != null and texture != _bound_cache_texture:
		_bound_cache_texture = texture
		_material.set_shader_parameter("u_terrain_cache", texture)

	var ready := bool(_terrain_cache_active.call("cache_ready")) and texture != null
	if ready != _bound_cache_ready:
		_bound_cache_ready = ready
		_material.set_shader_parameter("u_terrain_cache_ready", 1.0 if ready else 0.0)

	var generation := int(_terrain_cache_active.call("anchor_generation"))
	if generation != _bound_cache_generation:
		_bound_cache_generation = generation
		_material.set_shader_parameter("u_terrain_cache_generation", generation)

	var cache_res := int(_terrain_cache_active.call("cache_resolution"))
	if cache_res != _bound_cache_res:
		_bound_cache_res = cache_res
		_material.set_shader_parameter("u_terrain_cache_res", cache_res)


func _update_staging_readiness() -> void:
	var cache_stats: Dictionary = _terrain_cache_staging.call("stats")
	_handoff_staging_failed = bool(cache_stats.get("failed", false))
	if _handoff_staging_failed:
		_handoff_quiet_frames = 0
		_handoff_ready = false
		return

	var quiet: bool = bool(cache_stats.get("ready", false)) \
		and bool(cache_stats.get("bindings_ready", false)) \
		and int(cache_stats.get("queued_jobs", 1)) == 0 \
		and int(cache_stats.get("last_frame_jobs", 1)) == 0
	if quiet:
		_handoff_quiet_frames += 1
	else:
		_handoff_quiet_frames = 0
	_handoff_ready = _handoff_quiet_frames >= HANDOFF_QUIET_FRAMES


func gpu_stream_stats() -> Dictionary:
	var out: Dictionary = super.gpu_stream_stats()
	out["terrain_cache_architecture"] = "double_buffered_toroidal_gpu"
	out["terrain_cache_active"] = _bound_cache_ready
	out["terrain_cache_double_buffered"] = true
	out["terrain_cache_handoff"] = _handoff_active
	out["terrain_cache_handoff_ready"] = _handoff_ready
	out["terrain_cache_handoff_failed"] = _handoff_staging_failed
	out["terrain_cache_handoff_quiet_frames"] = _handoff_quiet_frames
	out["terrain_cache_handoff_swaps"] = _handoff_swaps
	out["terrain_cache_handoff_retargets"] = _handoff_retargets
	out["terrain_cache_handoff_delays"] = _handoff_delayed_parent_resets
	out["terrain_cache_cold_fallbacks"] = _handoff_cold_fallbacks
	out["terrain_cache_anchor_distance_m"] = maxf(absf(_center_plane.x), absf(_center_plane.y))
	out["terrain_cache_pending_target_m"] = _pending_target_offset.length() if _handoff_active else 0.0

	if _terrain_cache_active != null and is_instance_valid(_terrain_cache_active):
		var cache_stats: Dictionary = _terrain_cache_active.call("stats")
		out["terrain_cache_ready"] = bool(cache_stats.get("ready", false))
		out["terrain_cache_bindings_ready"] = bool(cache_stats.get("bindings_ready", false))
		out["terrain_cache_res"] = int(cache_stats.get("resolution", 0))
		out["terrain_cache_levels"] = int(cache_stats.get("levels", 0))
		out["terrain_cache_generation"] = int(cache_stats.get("generation", 0))
		out["terrain_cache_queued_jobs"] = int(cache_stats.get("queued_jobs", 0))
		out["terrain_cache_last_frame_jobs"] = int(cache_stats.get("last_frame_jobs", 0))
		out["terrain_cache_last_frame_samples"] = int(cache_stats.get("last_frame_samples", 0))
		out["terrain_cache_sample_budget"] = int(cache_stats.get("sample_budget", 0))
		out["terrain_cache_samples_total"] = int(cache_stats.get("samples_dispatched", 0))
		out["terrain_cache_strip_updates"] = int(cache_stats.get("strip_updates", 0))
		out["terrain_cache_anchor_resets"] = int(cache_stats.get("anchor_resets", 0))
		out["terrain_cache_toroidal"] = bool(cache_stats.get("toroidal", false))
		out["terrain_cache_staggered"] = bool(cache_stats.get("staggered", false))

	if _terrain_cache_staging != null and is_instance_valid(_terrain_cache_staging):
		var staging_stats: Dictionary = _terrain_cache_staging.call("stats")
		out["terrain_cache_staging_ready"] = bool(staging_stats.get("ready", false))
		out["terrain_cache_staging_bindings_ready"] = bool(staging_stats.get("bindings_ready", false))
		out["terrain_cache_staging_generation"] = int(staging_stats.get("generation", 0))
		out["terrain_cache_staging_queued_jobs"] = int(staging_stats.get("queued_jobs", 0))
		out["terrain_cache_staging_last_jobs"] = int(staging_stats.get("last_frame_jobs", 0))
		out["terrain_cache_staging_samples_total"] = int(staging_stats.get("samples_dispatched", 0))
	return out
