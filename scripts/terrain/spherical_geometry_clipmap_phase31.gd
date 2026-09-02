extends "res://scripts/terrain/spherical_geometry_clipmap_phase30.gd"
## Phase 31/42: displacement-aware LOD, transactional preview source and exact
## production-control cache synchronization.
##
## The production clipmap historically assumed 220 m was enough headroom above the
## macro height field when selecting its finest visible LOD. The editable production
## geomorph alone can exceed that envelope, so a valid vertex program could survive
## compilation yet still disappear because the CPU selected/cut geometry using stale
## bounds. This layer consumes the authoritative runtime's conservative envelope.
##
## Phase 42A also closes the warm-cache/analytic split: immediately before either
## cache synthesizes texels, it receives the same last-known-good production control
## snapshot that is bound to the visible material. A changed snapshot invalidates
## cache keys first, so analytic fallback remains authoritative during refill.

const BOUNDED_DISPLACEMENT_RUNTIME := preload(
	"res://scripts/world_authoring/terrain_displacement_runtime_phase42.gd")
const TERRAIN_PREVIEW_META := &"terrain_graph_preview_enabled"

var _displacement_guard_m: float = LOD_SURFACE_GUARD_M
var _displacement_guard_fallback: bool = false
var _displacement_guard_reason: String = ""
var _cache_geomorph_snapshot_changes: int = 0


func _ensure_displacement_runtime() -> void:
	if _displacement_runtime != null and is_instance_valid(_displacement_runtime):
		if _displacement_runtime.get_script() == BOUNDED_DISPLACEMENT_RUNTIME:
			return
		_displacement_runtime.remove_from_group(&"terrain_displacement_runtime")
		_displacement_runtime.queue_free()
		_displacement_runtime = null
	_displacement_runtime = BOUNDED_DISPLACEMENT_RUNTIME.new() as Node
	if _displacement_runtime == null:
		return
	_displacement_runtime.name = "TerrainDisplacementRuntime"
	add_child(_displacement_runtime)
	_displacement_fingerprint = ""


func _update_terrain_caches() -> void:
	# cached.gd calls this virtual hook from its normal process path. Synchronize the
	# transactional snapshot before update_cache() can queue a compute dispatch.
	_sync_cache_production_controls(_terrain_cache_active)
	_sync_cache_production_controls(_terrain_cache_staging)
	super._update_terrain_caches()


func _sync_cache_production_controls(cache: Node) -> void:
	if cache == null or not is_instance_valid(cache) \
			or not cache.has_method("set_production_controls"):
		return
	if _displacement_runtime == null or not is_instance_valid(_displacement_runtime) \
			or not _displacement_runtime.has_method("active_production_controls"):
		return
	var controls_value: Variant = _displacement_runtime.call("active_production_controls")
	if not (controls_value is Dictionary):
		return
	if bool(cache.call("set_production_controls", controls_value as Dictionary)):
		_cache_geomorph_snapshot_changes += 1


func _active_authoring_terrain() -> Resource:
	var editor: Node = get_tree().root.find_child("PlanetStudioLive", true, false)
	if editor == null:
		return null
	var session_value: Variant = editor.get("_session")
	if session_value == null:
		return null

	# Preserve the historical live-preview behaviour until the Phase 36 graph UI
	# explicitly creates this metadata flag. Once present, the user owns the A/B
	# choice and the renderer never silently changes source underneath them.
	var preview_enabled: bool = true
	if session_value.has_method("has_meta") and bool(session_value.call("has_meta", TERRAIN_PREVIEW_META)):
		preview_enabled = bool(session_value.call("get_meta", TERRAIN_PREVIEW_META, true))
	if preview_enabled:
		if session_value.has_method("active_terrain_profile"):
			return session_value.call("active_terrain_profile") as Resource
		return null

	var applied_value: Variant = session_value.get("applied_system")
	var applied_system: Resource = applied_value as Resource if applied_value is Resource else null
	if applied_system == null or not applied_system.has_method("active_body"):
		return null
	var body: Resource = applied_system.call("active_body") as Resource
	if body == null:
		return null
	var profile: Resource = body.get(&"planet_profile") as Resource
	return profile.get(&"terrain") as Resource if profile != null else null


func _current_displacement_guard_m() -> float:
	_displacement_guard_m = LOD_SURFACE_GUARD_M
	_displacement_guard_fallback = false
	_displacement_guard_reason = ""
	if _displacement_runtime == null or not is_instance_valid(_displacement_runtime) \
			or not _displacement_runtime.has_method("displacement_envelope"):
		return _displacement_guard_m
	var value: Variant = _displacement_runtime.call("displacement_envelope")
	if not (value is Dictionary):
		return _displacement_guard_m
	var envelope: Dictionary = value as Dictionary
	if bool(envelope.get("bounds_known", false)):
		_displacement_guard_m = maxf(
			LOD_SURFACE_GUARD_M,
			maxf(float(envelope.get("total_max_abs_m", 0.0)), 0.0))
	else:
		# The custom AABB already declares this as the renderer's maximum displaced
		# extent. Reuse the same number for an unknown program instead of inventing a
		# smaller safety limit that could cull real geometry.
		_displacement_guard_m = GLOBAL_BOUNDS_M
		_displacement_guard_fallback = true
		_displacement_guard_reason = String(envelope.get("unknown_reason", "unbounded author graph"))
	return _displacement_guard_m


func _update_visible_cap(observer_radius: float, planet_radius: float) -> void:
	var safe_r: float = maxf(observer_radius, planet_radius + 0.01)
	var horizon_angle: float = acos(clampf(planet_radius / safe_r, -1.0, 1.0))
	var horizon_arc: float = horizon_angle * planet_radius
	var guard: float = _current_displacement_guard_m()

	# From a near-surface observer, relief h above the reference sphere can remain
	# visible roughly sqrt(2Rh+h^2) past the reference-sphere horizon. Adding that
	# reach to the existing 20 km production margin is conservative and monotonic.
	var relief_horizon_reach: float = sqrt(maxf(
		2.0 * planet_radius * guard + guard * guard, 0.0))
	_visible_cap_arc_m = minf(PI * 0.5 * planet_radius,
		horizon_arc + HORIZON_MARGIN_M + relief_horizon_reach)


func _update_screen_space_min_level() -> void:
	var camera: Camera3D = get_viewport().get_camera_3d()
	if camera == null or Planet.cfg == null:
		_active_min_level = 0
		_active_min_initialized = false
		return

	var viewport_size: Vector2 = get_viewport().get_visible_rect().size
	var viewport_h: float = maxf(viewport_size.y, 1.0)
	var aspect: float = viewport_size.x / viewport_h
	var vertical_fov: float = deg_to_rad(camera.fov)
	if camera.keep_aspect == Camera3D.KEEP_WIDTH:
		vertical_fov = 2.0 * atan(tan(vertical_fov * 0.5) / maxf(aspect, 1e-6))

	var origin := Vector3(float(Frames.origin.x), float(Frames.origin.y), float(Frames.origin.z))
	var planet_pos: Vector3 = camera.global_position + origin
	var observer_radius: float = planet_pos.length()
	if observer_radius <= 1.0:
		_active_min_level = 0
		_active_min_initialized = false
		return

	var observer_dir: Vector3 = planet_pos / observer_radius
	var local_macro_h: float = Planet.macro_height(observer_dir)
	var displacement_guard: float = _current_displacement_guard_m()
	_screen_space_surface_distance_m = maxf(
		observer_radius - (Planet.cfg.planet_radius + local_macro_h) - displacement_guard,
		0.0)
	_screen_space_metres_per_pixel = 2.0 * _screen_space_surface_distance_m \
		* tan(vertical_fov * 0.5) / viewport_h

	var max_fine_spacing: float = maxf(
		_base_spacing,
		_screen_space_metres_per_pixel * TARGET_FINE_VERTEX_PX)
	_screen_space_raw_level = clampf(
		log(max_fine_spacing / maxf(_base_spacing, 1e-6)) / log(2.0),
		0.0,
		float(MAX_LEVEL))

	if not _active_min_initialized:
		_active_min_level = clampi(int(floor(_screen_space_raw_level + 1e-6)), 0, MAX_LEVEL)
		_active_min_initialized = true
		return

	while _active_min_level < MAX_LEVEL \
			and _screen_space_raw_level >= float(_active_min_level + 1) + LOD_LEVEL_HYSTERESIS:
		_active_min_level += 1
	while _active_min_level > 0 \
			and _screen_space_raw_level < float(_active_min_level) - LOD_LEVEL_HYSTERESIS:
		_active_min_level -= 1


func gpu_stream_stats() -> Dictionary:
	var out: Dictionary = super.gpu_stream_stats()
	out["terrain_displacement_guard_m"] = _current_displacement_guard_m()
	out["terrain_displacement_guard_fallback"] = _displacement_guard_fallback
	out["terrain_displacement_guard_reason"] = _displacement_guard_reason
	out["terrain_cache_geomorph_snapshot_changes"] = _cache_geomorph_snapshot_changes
	if _displacement_runtime != null and is_instance_valid(_displacement_runtime) \
			and _displacement_runtime.has_method("displacement_envelope"):
		out["terrain_displacement_envelope"] = _displacement_runtime.call("displacement_envelope")
	if _terrain_cache_active != null and is_instance_valid(_terrain_cache_active) \
			and _terrain_cache_active.has_method("stats"):
		out["terrain_cache_geomorph"] = _terrain_cache_active.call("stats")
	return out
