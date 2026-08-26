extends "res://scripts/terrain/spherical_geometry_clipmap_handoff.gd"
## Final cached terrain-rendering layer.
##
## Two independent GPU caches are kept resident. The active cache remains bound to
## visible terrain while the spare cache synthesizes the predicted next tangent
## anchor. Geometry and cache references are swapped only after the spare cache is
## completely warm, so an ordinary reanchor never exposes a cold cache to rendering.

const CACHED_SURFACE_SHADER_PATH := "res://shaders/spherical_geometry_clipmap_cached_surface.gdshader"
const TerrainClipmapCacheScript := preload("res://scripts/terrain/gpu_terrain_clipmap_cache.gd")

# Begin early enough that even a fast vehicle/aircraft gives the 15-layer cache
# many frames to fill. The target itself remains the normal 65.536 km anchor edge.
const HANDOFF_PREWARM_M: float = 16384.0
const HANDOFF_RETARGET_M: float = 2048.0
const HANDOFF_SWAP_MAX_OFFSET_M: float = 64.0
const HANDOFF_QUIET_FRAMES: int = 2
const HANDOFF_MIN_MOTION_M: float = 2.0
const HANDOFF_OVERDUE_LEAD_MIN_M: float = 16384.0
const HANDOFF_OVERDUE_LEAD_MAX_M: float = 49152.0
const HANDOFF_WARM_FRAMES_ESTIMATE: float = 300.0

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
var _handoff_source_anchor_dir := Vector3.ZERO
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
	# leaves the entire ring/sector/micro topology unchanged.
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

	# Allocate both RenderingDevice caches up front. Reanchor time performs only a
	# reference swap; it never creates a texture/pipeline or waits on allocation.
	_terrain_cache_active = TerrainClipmapCacheScript.new()
	_terrain_cache_active.name = "GPUTerrainClipmapCacheA"
	add_child(_terrain_cache_active)
	_terrain_cache_staging = TerrainClipmapCacheScript.new()
	_terrain_cache_staging.name = "GPUTerrainClipmapCacheB"
	add_child(_terrain_cache_staging)


func _process(dt: float) -> void:
	# The handoff parent resolves anchor movement before publishing render uniforms.
	# Cache work follows, so an atomic A/B swap is rebound before this frame renders.
	super._process(dt)
	_update_terrain_caches()


func _resolve_reanchor(observer_dir: Vector3, observer_surface_world: Vec3D,
		plane_offset: Vector2) -> Vector2:
	# A world rebuild or other explicit anchor reset invalidates only handoff state;
	# each cache independently tracks world/context generation.
	if _handoff_active and _handoff_source_anchor_dir.distance_squared_to(_anchor_dir) > 1e-10:
		_cancel_handoff()
		_last_plane_valid = false

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

	if not _handoff_active and extent >= HANDOFF_PREWARM_M and heading.length_squared() > 0.5:
		_start_handoff(_predict_handoff_target(plane_offset, heading), false)
	elif _handoff_active and heading.length_squared() > 0.5:
		var revised_target: Vector2 = _predict_handoff_target(plane_offset, heading)
		if revised_target.distance_to(_pending_target_offset) >= HANDOFF_RETARGET_M:
			_start_handoff(revised_target, true)

	if _handoff_active and _handoff_ready:
		var pending_offset: Vector2 = _project_to_pending_anchor(observer_surface_world)
		if maxf(absf(pending_offset.x), absf(pending_offset.y)) <= HANDOFF_SWAP_MAX_OFFSET_M:
			return _commit_handoff(observer_surface_world)

	# If the spare cache itself failed, retain the old safe behavior rather than
	# allowing the tangent anchor to drift without bound. This is an error fallback,
	# not part of the normal handoff path.
	if _handoff_staging_failed and extent > REANCHOR_M:
		_cancel_handoff()
		_last_plane_valid = false
		return super._resolve_reanchor(observer_dir, observer_surface_world, plane_offset)

	# Crucially, crossing REANCHOR_M no longer forces a visible reset. The old anchor
	# and old cache continue rendering until the staged destination is actually ready.
	return plane_offset


func _predict_handoff_target(plane_offset: Vector2, heading: Vector2) -> Vector2:
	var extent: float = maxf(absf(plane_offset.x), absf(plane_offset.y))
	if extent < REANCHOR_M:
		var best_t := INF
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

		# Degenerate heading fallback: project radially to the same square boundary.
		var denom: float = maxf(absf(plane_offset.x), absf(plane_offset.y))
		if denom > 1e-6:
			return plane_offset * (REANCHOR_M / denom)

	# If a sharp turn invalidated the prediction after the old threshold, choose a
	# new point far enough ahead to finish warming before the player reaches it.
	var lead: float = clampf(
		maxf(_last_motion_m * HANDOFF_WARM_FRAMES_ESTIMATE, HANDOFF_OVERDUE_LEAD_MIN_M),
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
	_handoff_source_anchor_dir = _anchor_dir
	_pending_target_offset = target_offset
	_pending_anchor_dir = _direction_for_offset(
		_anchor_dir, _anchor_right, _anchor_up, target_offset, Planet.cfg.planet_radius)
	var tangent: Array = CubeSphere.tangent_basis(_pending_anchor_dir)
	_pending_anchor_right = tangent[0]
	_pending_anchor_up = tangent[1]
	_pending_anchor_world = Vec3D.from_v3(_pending_anchor_dir).mul(Planet.cfg.planet_radius)


func _project_to_pending_anchor(surface_world: Vec3D) -> Vector2:
	var rel: Vec3D = surface_world.sub(_pending_anchor_world)
	return Vector2(
		rel.x * _pending_anchor_right.x + rel.y * _pending_anchor_right.y
			+ rel.z * _pending_anchor_right.z,
		rel.x * _pending_anchor_up.x + rel.y * _pending_anchor_up.y
			+ rel.z * _pending_anchor_up.z)


func _commit_handoff(observer_surface_world: Vec3D) -> Vector2:
	# The destination cache is already complete. Swap cache ownership first, then
	# publish exactly the matching tangent basis and stable world anchor.
	var previous_active: Node = _terrain_cache_active
	_terrain_cache_active = _terrain_cache_staging
	_terrain_cache_staging = previous_active

	var next_dir: Vector3 = _pending_anchor_dir
	var next_world: Vec3D = _pending_anchor_world.dup()
	_reset_anchor(next_dir)
	_capture_stable_anchor(next_world)

	var rel: Vec3D = observer_surface_world.sub(_stable_anchor_world)
	var next_offset := Vector2(
		rel.x * _anchor_right.x + rel.y * _anchor_right.y + rel.z * _anchor_right.z,
		rel.x * _anchor_up.x + rel.y * _anchor_up.y + rel.z * _anchor_up.z)

	_handoff_swaps += 1
	_cancel_handoff()
	_last_plane_offset = next_offset
	_last_plane_valid = true
	_force_cache_rebind()
	return next_offset


func _cancel_handoff() -> void:
	_handoff_active = false
	_handoff_ready = false
	_handoff_staging_failed = false
	_handoff_quiet_frames = 0
	_handoff_source_anchor_dir = Vector3.ZERO
	_pending_target_offset = Vector2.ZERO


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

	# Visible cache follows the real clipmap exactly.
	_terrain_cache_active.call("update_cache",
		_anchor_dir, _anchor_right, _anchor_up,
		_center_plane, _base_spacing,
		_active_min_level, _active_max_level)
	_bind_active_cache()

	# Staging is centred exactly on the predicted handoff anchor. It does not chase
	# the current camera; this guarantees a fully populated centre around the point
	# where the atomic switch is allowed to happen.
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
		out["terrain_cache_staging_generation"] = int(staging_stats.get("generation", 0))
		out["terrain_cache_staging_queued_jobs"] = int(staging_stats.get("queued_jobs", 0))
		out["terrain_cache_staging_last_jobs"] = int(staging_stats.get("last_frame_jobs", 0))
		out["terrain_cache_staging_samples_total"] = int(staging_stats.get("samples_dispatched", 0))
	return out
