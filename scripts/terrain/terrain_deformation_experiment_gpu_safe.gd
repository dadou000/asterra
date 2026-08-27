extends "res://scripts/terrain/terrain_deformation_experiment_gpu.gd"
## Stable asynchronous first-contact manifold for the deformation experiment.
##
## The previous safety wrapper could visibly oscillate because it re-positioned the
## sphere every physics substep onto whichever asynchronous fallback happened to be
## newest, then waited indefinitely for the exact rendered query. This version
## treats first contact like a real collision manifold:
##
## 1. Prefetch the exact rendered height and the strict same-location terrain height.
## 2. Freeze one pre-contact plane for the current drop.
## 3. Upgrade that plane to the exact rendered sample only while safely airborne.
## 4. Once crossed, lock the plane permanently and hand the incoming velocity to the
##    normal elasto-plastic solver on the next substep.
##
## No post-crossing asynchronous source switch is allowed, so query latency can no
## longer produce hovering, teleporting or rapid up/down chatter.

const EXACT_UPGRADE_MIN_CLEARANCE_M := 0.75
const EXACT_UPGRADE_MAX_DELTA_M := 1.50
const DEFAULT_RENDER_SURFACE_BIAS_M := 0.035

var _sphere_precontact_plane_m := 0.0
var _sphere_precontact_plane_valid := false
var _sphere_precontact_source := "none"
var _sphere_locked_source := "none"
var _sphere_exact_seen := false


func _place_sphere(drop_immediately: bool) -> void:
	super._place_sphere(drop_immediately)
	if not _world_ready:
		return
	_sphere_precontact_plane_m = 0.0
	_sphere_precontact_plane_valid = false
	_sphere_precontact_source = "none"
	_sphere_locked_source = "none"
	_sphere_exact_seen = false
	_prefetch_contact_sources()
	_refresh_precontact_plane(true)
	if _sphere_precontact_plane_valid:
		# Placement should be ten metres above the same plane that will be used for
		# continuous crossing, not ten metres above a broad aiming fallback.
		_sphere_altitude_msl = _sphere_precontact_plane_m + SPHERE_RADIUS_M + 10.0
		_update_visual_transforms()


func _prefetch_contact_sources() -> void:
	if _sphere_dir.length_squared() <= 1e-12:
		return
	RenderedTerrainContactQuery.request_height(_sphere_dir)
	TerrainHeightQuery.request_height(_sphere_dir)
	if TerrainDeformationGPU.ready_state and not TerrainDeformationGPU.failed:
		TerrainDeformationGPU.active_height_offset(_sphere_dir)


func _renderer_surface_bias() -> float:
	var terrain: Node = get_node_or_null("/root/GroundGeometryClipmap")
	if terrain == null or not terrain.has_method("rendered_contact_sample_params"):
		return DEFAULT_RENDER_SURFACE_BIAS_M
	var value: Variant = terrain.call("rendered_contact_sample_params")
	if not (value is Dictionary):
		return DEFAULT_RENDER_SURFACE_BIAS_M
	var params: Dictionary = value
	return float(params.get("surface_bias", DEFAULT_RENDER_SURFACE_BIAS_M))


func _strict_contact_ground_height() -> float:
	if Planet.cfg == null or _sphere_dir.length_squared() <= 1e-12:
		return NAN
	TerrainHeightQuery.request_height(_sphere_dir)
	if not TerrainHeightQuery.has_method("has_contact_height") \
			or not bool(TerrainHeightQuery.call("has_contact_height", _sphere_dir)):
		return NAN
	var base_value: Variant = TerrainHeightQuery.call(
		"contact_height_for_direction", _sphere_dir, NAN)
	var height_m: float = float(base_value)
	if not is_finite(height_m):
		return NAN

	# contact_height_for_direction already includes persistent Deltas. Add the live
	# GPU deformation only when the point-query cache is on the current field
	# generation, otherwise wait rather than combining states from different frames.
	if TerrainDeformationGPU.ready_state and not TerrainDeformationGPU.failed \
			and bool(TerrainDeformationGPU.get("_has_active_content")):
		if not TerrainDeformationQueryGPU.ready_state or TerrainDeformationQueryGPU.failed:
			return NAN
		TerrainDeformationGPU.active_height_offset(_sphere_dir)
		if not TerrainDeformationQueryGPU.has_method("has_fresh_sample") \
				or not bool(TerrainDeformationQueryGPU.call("has_fresh_sample", _sphere_dir)):
			return NAN
		height_m += TerrainDeformationGPU.active_height_offset(_sphere_dir)

	# The production clipmap vertex shader raises its visible surface by this bias.
	# Including it removes the small systematic hover that a pure height query would
	# otherwise have relative to the rendered triangles.
	return height_m + _renderer_surface_bias()


func _set_precontact_plane(height_m: float, source: String) -> void:
	_sphere_precontact_plane_m = height_m
	_sphere_precontact_plane_valid = true
	_sphere_precontact_source = source


func _refresh_precontact_plane(force_exact: bool = false) -> void:
	_prefetch_contact_sources()
	var exact_height_m: float = _rendered_ground_height()
	if is_finite(exact_height_m):
		_sphere_exact_seen = true
		if not _sphere_precontact_plane_valid:
			_set_precontact_plane(exact_height_m, "exact-render")
			return
		if _sphere_precontact_source == "exact-render":
			return
		var clearance_m: float = _sphere_altitude_msl - SPHERE_RADIUS_M \
			- _sphere_precontact_plane_m
		var delta_m: float = absf(exact_height_m - _sphere_precontact_plane_m)
		if force_exact or (clearance_m >= EXACT_UPGRADE_MIN_CLEARANCE_M \
				and delta_m <= EXACT_UPGRADE_MAX_DELTA_M):
			_set_precontact_plane(exact_height_m, "exact-render")
		return

	if _sphere_precontact_plane_valid:
		return
	var strict_height_m: float = _strict_contact_ground_height()
	if is_finite(strict_height_m):
		_set_precontact_plane(strict_height_m, "strict-contact")


func _lock_precontact_plane(incoming_velocity_mps: float) -> void:
	_sphere_reference_ground_m = _sphere_precontact_plane_m
	_sphere_contact_ground_m = _sphere_precontact_plane_m
	_sphere_total_center_sink_m = 0.0
	_sphere_reference_ready = true
	_sphere_contact_started = true
	_sphere_locked_source = _sphere_precontact_source
	_sphere_altitude_msl = _sphere_precontact_plane_m + SPHERE_RADIUS_M
	_sphere_velocity_mps = incoming_velocity_mps
	_sphere_last_contact.clear()


func _step_sphere(dt: float) -> void:
	if _sphere_contact_started:
		super._step_sphere(dt)
		return

	_refresh_precontact_plane(false)
	if not _sphere_precontact_plane_valid:
		# Normally the strict same-location GPU query resolves within a few frames.
		# Until then the sphere is allowed to free-fall, but no deformation command is
		# possible because no collision manifold exists yet.
		_integrate_freefall(dt)
		_sphere_last_contact.clear()
		return

	var bottom_now_m: float = _sphere_altitude_msl - SPHERE_RADIUS_M
	if bottom_now_m <= _sphere_precontact_plane_m:
		# The plane became available after the object had crossed it. Query latency is
		# not penetration: rewind to first touch while preserving the current incoming
		# velocity so impact energy is not lost.
		_lock_precontact_plane(_sphere_velocity_mps)
		return

	var next_velocity_mps: float = clampf(
		_sphere_velocity_mps - GRAVITY_MPS2 * dt, -180.0, 80.0)
	var next_altitude_m: float = _sphere_altitude_msl + next_velocity_mps * dt
	var next_bottom_m: float = next_altitude_m - SPHERE_RADIUS_M
	if next_bottom_m > _sphere_precontact_plane_m:
		_sphere_velocity_mps = next_velocity_mps
		_sphere_altitude_msl = next_altitude_m
		_sphere_last_contact.clear()
		return

	# Continuous crossing against one frozen world-space plane. From this point on
	# the exact/strict source is never switched for this impact.
	_lock_precontact_plane(next_velocity_mps)


func _update_hud() -> void:
	super._update_hud()
	if _hud_label == null or not _world_ready:
		return
	var query_stats: Dictionary = RenderedTerrainContactQuery.stats()
	var plane_clearance_text := "n/a"
	if _sphere_precontact_plane_valid:
		var plane_clearance_m: float = _sphere_altitude_msl - SPHERE_RADIUS_M \
			- _sphere_precontact_plane_m
		plane_clearance_text = "%+.3f m" % plane_clearance_m
	var authority: String = _sphere_locked_source if _sphere_contact_started \
		else _sphere_precontact_source
	_hud_label.text += (
		"\nContact manifold: %s | plane clearance %s | exact seen %s" % [
			authority, plane_clearance_text, "YES" if _sphere_exact_seen else "NO"] +
		"\nExact query: ready %s bind %s flight %s pending %d readbacks %d invalid %d serial %d" % [
			"Y" if bool(query_stats.get("ready", false)) else "N",
			"Y" if bool(query_stats.get("bindings_ready", false)) else "N",
			"Y" if bool(query_stats.get("in_flight", false)) else "N",
			int(query_stats.get("pending", 0)),
			int(query_stats.get("readbacks", 0)),
			int(query_stats.get("invalid_cache_results", 0)),
			int(query_stats.get("source_serial", 0))]
	)
