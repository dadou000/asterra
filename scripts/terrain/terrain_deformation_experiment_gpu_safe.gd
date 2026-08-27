extends "res://scripts/terrain/terrain_deformation_experiment_gpu.gd"
## Stable physical first-contact manifold for the deformation experiment.
##
## Camera-dependent rendered LOD is no longer a physics authority. Every drop gets
## one world-space physical plane from the urgent centimetre-grade TerrainHeightQuery
## plus persistent and live deformation. That plane is frozen before contact and is
## never switched after crossing. A coarse plane exists only as a no-tunnelling hold
## while the GPU result is pending and can never deform terrain.

const DEFAULT_RENDER_SURFACE_BIAS_M := 0.035

var _sphere_physical_plane_m := 0.0
var _sphere_physical_plane_valid := false
var _sphere_safety_plane_m := 0.0
var _sphere_safety_plane_valid := false
var _sphere_safety_hold := false
var _sphere_saved_velocity_mps := 0.0
var _sphere_locked_source := "none"

var _saved_microrelief_enabled := true
var _microrelief_override_active := false


func _toggle() -> void:
	var was_active: bool = active
	if not was_active:
		_enable_physical_surface_validation()
	super._toggle()
	if was_active and not active:
		_disable_physical_surface_validation()
	elif not was_active and not active:
		# Opening can fail outside Main; do not leave a render debug override behind.
		_disable_physical_surface_validation()


func _enable_physical_surface_validation() -> void:
	if _microrelief_override_active:
		return
	var terrain: Node = get_node_or_null("/root/GroundGeometryClipmap")
	if terrain != null and terrain.has_method("debug_microrelief_enabled") \
			and terrain.has_method("set_debug_microrelief_enabled"):
		_saved_microrelief_enabled = bool(terrain.call("debug_microrelief_enabled"))
		terrain.call("set_debug_microrelief_enabled", false)
	_microrelief_override_active = true


func _disable_physical_surface_validation() -> void:
	var terrain: Node = get_node_or_null("/root/GroundGeometryClipmap")
	if terrain != null:
		if terrain.has_method("set_contact_overlay"):
			terrain.call("set_contact_overlay", _sphere_dir, false)
		if _microrelief_override_active and terrain.has_method("set_debug_microrelief_enabled"):
			terrain.call("set_debug_microrelief_enabled", _saved_microrelief_enabled)
	_microrelief_override_active = false


func _place_sphere(drop_immediately: bool) -> void:
	super._place_sphere(drop_immediately)
	if not _world_ready:
		return
	_sphere_physical_plane_m = 0.0
	_sphere_physical_plane_valid = false
	_sphere_safety_plane_m = _coarse_safety_ground_height()
	_sphere_safety_plane_valid = is_finite(_sphere_safety_plane_m)
	_sphere_safety_hold = false
	_sphere_saved_velocity_mps = 0.0
	_sphere_locked_source = "none"
	_pin_contact_overlay()
	_request_physical_contact_height()
	var strict_height_m: float = _strict_contact_ground_height()
	if is_finite(strict_height_m):
		_set_physical_plane(strict_height_m)
		_sphere_altitude_msl = strict_height_m + SPHERE_RADIUS_M + 10.0
	elif _sphere_safety_plane_valid:
		_sphere_altitude_msl = _sphere_safety_plane_m + SPHERE_RADIUS_M + 10.0
	_update_visual_transforms()


func _pin_contact_overlay() -> void:
	var terrain: Node = get_node_or_null("/root/GroundGeometryClipmap")
	if terrain != null and terrain.has_method("set_contact_overlay"):
		terrain.call("set_contact_overlay", _sphere_dir, active)


func _renderer_surface_bias() -> float:
	var terrain: Node = get_node_or_null("/root/GroundGeometryClipmap")
	if terrain == null or not terrain.has_method("rendered_contact_sample_params"):
		return DEFAULT_RENDER_SURFACE_BIAS_M
	var value: Variant = terrain.call("rendered_contact_sample_params")
	if not (value is Dictionary):
		return DEFAULT_RENDER_SURFACE_BIAS_M
	var params: Dictionary = value
	return float(params.get("surface_bias", DEFAULT_RENDER_SURFACE_BIAS_M))


func _coarse_safety_ground_height() -> float:
	if Planet.cfg == null or _sphere_dir.length_squared() <= 1e-12:
		return NAN
	return TerrainContactSampler.coarse_height(_sphere_dir) + _renderer_surface_bias()


func _request_physical_contact_height() -> void:
	if _sphere_dir.length_squared() <= 1e-12:
		return
	if TerrainHeightQuery.has_method("request_contact_height"):
		TerrainHeightQuery.call("request_contact_height", _sphere_dir)
	else:
		TerrainHeightQuery.request_height(_sphere_dir)
	if TerrainDeformationGPU.ready_state and not TerrainDeformationGPU.failed:
		TerrainDeformationGPU.active_height_offset(_sphere_dir)


func _strict_contact_ground_height() -> float:
	if Planet.cfg == null or _sphere_dir.length_squared() <= 1e-12:
		return NAN
	_request_physical_contact_height()
	if not TerrainHeightQuery.has_method("has_contact_height") \
			or not bool(TerrainHeightQuery.call("has_contact_height", _sphere_dir)):
		return NAN
	var base_value: Variant = TerrainHeightQuery.call(
		"contact_height_for_direction", _sphere_dir, NAN)
	var height_m: float = float(base_value)
	if not is_finite(height_m):
		return NAN

	# contact_height_for_direction already contains persistent Deltas. If a live
	# deformation tile exists, require the point-query cache to be on its current
	# generation before combining the two states.
	if TerrainDeformationGPU.ready_state and not TerrainDeformationGPU.failed \
			and bool(TerrainDeformationGPU.get("_has_active_content")):
		if not TerrainDeformationQueryGPU.ready_state or TerrainDeformationQueryGPU.failed:
			return NAN
		TerrainDeformationGPU.active_height_offset(_sphere_dir)
		if not TerrainDeformationQueryGPU.has_method("has_fresh_sample") \
				or not bool(TerrainDeformationQueryGPU.call("has_fresh_sample", _sphere_dir)):
			return NAN
		height_m += TerrainDeformationGPU.active_height_offset(_sphere_dir)
	return height_m + _renderer_surface_bias()


func _set_physical_plane(height_m: float) -> void:
	if _sphere_physical_plane_valid:
		return
	_sphere_physical_plane_m = height_m
	_sphere_physical_plane_valid = true


func _lock_physical_contact(incoming_velocity_mps: float) -> void:
	if not _sphere_physical_plane_valid:
		return
	_sphere_reference_ground_m = _sphere_physical_plane_m
	_sphere_contact_ground_m = _sphere_physical_plane_m
	_sphere_total_center_sink_m = 0.0
	_sphere_reference_ready = true
	_sphere_contact_started = true
	_sphere_locked_source = "physical-l0"
	_sphere_altitude_msl = _sphere_physical_plane_m + SPHERE_RADIUS_M
	_sphere_velocity_mps = incoming_velocity_mps
	_sphere_safety_hold = false
	_sphere_saved_velocity_mps = 0.0
	_sphere_last_contact.clear()


func _step_sphere(dt: float) -> void:
	if _sphere_contact_started:
		super._step_sphere(dt)
		return

	# Acquire the strict physical plane exactly once. It is never refreshed after it
	# becomes valid, so asynchronous query timing cannot move the collision manifold.
	if not _sphere_physical_plane_valid:
		var strict_height_m: float = _strict_contact_ground_height()
		if is_finite(strict_height_m):
			_set_physical_plane(strict_height_m)
			if _sphere_safety_hold:
				# Resume from the frozen safety position with the velocity that existed at
				# the moment the object would have crossed it. If the physical plane is
				# lower, the sphere simply continues falling toward it; if it is higher,
				# the crossing test below performs one correction to first touch.
				_sphere_velocity_mps = _sphere_saved_velocity_mps
				_sphere_saved_velocity_mps = 0.0
				_sphere_safety_hold = false

	if _sphere_physical_plane_valid:
		var bottom_now_m: float = _sphere_altitude_msl - SPHERE_RADIUS_M
		if bottom_now_m <= _sphere_physical_plane_m:
			_lock_physical_contact(_sphere_velocity_mps)
			return
		var next_velocity_mps: float = clampf(
			_sphere_velocity_mps - GRAVITY_MPS2 * dt, -180.0, 80.0)
		var next_altitude_m: float = _sphere_altitude_msl + next_velocity_mps * dt
		var next_bottom_m: float = next_altitude_m - SPHERE_RADIUS_M
		if next_bottom_m <= _sphere_physical_plane_m:
			_lock_physical_contact(next_velocity_mps)
			return
		_sphere_velocity_mps = next_velocity_mps
		_sphere_altitude_msl = next_altitude_m
		_sphere_last_contact.clear()
		return

	# Strict terrain is still pending. A frozen coarse plane is only an emergency
	# anti-tunnelling barrier; it never calls TerrainDeformation.apply_contact().
	if not _sphere_safety_plane_valid:
		_sphere_safety_plane_m = _coarse_safety_ground_height()
		_sphere_safety_plane_valid = is_finite(_sphere_safety_plane_m)
	if not _sphere_safety_plane_valid:
		_integrate_freefall(dt)
		_sphere_last_contact.clear()
		return
	if _sphere_safety_hold:
		_sphere_altitude_msl = _sphere_safety_plane_m + SPHERE_RADIUS_M
		_sphere_velocity_mps = 0.0
		_sphere_last_contact.clear()
		return

	var safety_next_velocity_mps: float = clampf(
		_sphere_velocity_mps - GRAVITY_MPS2 * dt, -180.0, 80.0)
	var safety_next_altitude_m: float = _sphere_altitude_msl + safety_next_velocity_mps * dt
	if safety_next_altitude_m - SPHERE_RADIUS_M > _sphere_safety_plane_m:
		_sphere_velocity_mps = safety_next_velocity_mps
		_sphere_altitude_msl = safety_next_altitude_m
		_sphere_last_contact.clear()
		return
	_sphere_saved_velocity_mps = safety_next_velocity_mps
	_sphere_altitude_msl = _sphere_safety_plane_m + SPHERE_RADIUS_M
	_sphere_velocity_mps = 0.0
	_sphere_safety_hold = true
	_sphere_last_contact.clear()


func _update_hud() -> void:
	if _hud_label == null:
		return
	if not _world_ready:
		_hud_label.text = "TERRAIN DEFORMATION EXPERIMENT\nWaiting for the generated world...\nF10: close experiment"
		return
	var reference_plane_m := NAN
	var manifold := "waiting physical GPU sample"
	if _sphere_contact_started:
		reference_plane_m = _sphere_contact_ground_m
		manifold = _sphere_locked_source
	elif _sphere_physical_plane_valid:
		reference_plane_m = _sphere_physical_plane_m
		manifold = "physical-l0 ready"
	elif _sphere_safety_hold:
		reference_plane_m = _sphere_safety_plane_m
		manifold = "SAFETY HOLD (no deformation)"
	elif _sphere_safety_plane_valid:
		reference_plane_m = _sphere_safety_plane_m
		manifold = "waiting physical; safety armed"
	var clearance_text := "clearance n/a"
	if is_finite(reference_plane_m):
		var clearance_m: float = _sphere_altitude_msl - SPHERE_RADIUS_M - reference_plane_m
		clearance_text = "clearance %+7.3f m" % clearance_m
	var bearing_ratio := 0.0
	var sink_rate := 0.0
	if not _sphere_last_contact.is_empty():
		bearing_ratio = float(_sphere_last_contact.get("bearing_ratio", 0.0))
		sink_rate = float(_sphere_last_contact.get("sink_rate_mps", 0.0))
	var state_stats: Dictionary = TerrainDeformation.stats()
	var query_stats: Dictionary = TerrainHeightQuery.stats()
	var edit_stats: Dictionary = {}
	if TerrainEditDeltaGPU.has_method("stats"):
		var edit_value: Variant = TerrainEditDeltaGPU.call("stats")
		if edit_value is Dictionary:
			edit_stats = edit_value
	var clip_stats: Dictionary = {}
	var clipmap: Node = get_node_or_null("/root/GroundGeometryClipmap")
	if clipmap != null and clipmap.has_method("gpu_stream_stats"):
		var clip_value: Variant = clipmap.call("gpu_stream_stats")
		if clip_value is Dictionary:
			clip_stats = clip_value
	_hud_label.text = (
		"TERRAIN DEFORMATION EXPERIMENT\n" +
		"Material [1-4]: %s\n" % TerrainDeformation.material_name(material_id) +
		"Tungsten sphere: %.1f t | %s | v %+7.2f m/s | bearing %.2fx | sink %.3f m/s\n" % [
			_sphere_mass_kg / 1000.0, clearance_text, _sphere_velocity_mps,
			bearing_ratio, sink_rate] +
		"Contact manifold: %s | locked %s\n" % [
			manifold, "YES" if _sphere_contact_started else "NO"] +
		"Physical query: ready %s | pending %d | in flight %d | cached %d\n" % [
			"Y" if bool(query_stats.get("ready", false)) else "N",
			int(query_stats.get("pending", 0)), int(query_stats.get("in_flight", 0)),
			int(query_stats.get("cached_samples", 0))] +
		"Travel: edit recenter %d/%d px | cache buffers %d | spare alloc/reuse %d/%d\n" % [
			int(edit_stats.get("last_recenter_sampled_pixels", 0)),
			int(edit_stats.get("full_window_pixels", 262144)),
			int(clip_stats.get("terrain_cache_resident_buffers", 1)),
			int(clip_stats.get("terrain_cache_staging_allocations", 0)),
			int(clip_stats.get("terrain_cache_staging_reuses", 0))] +
		"Bucket reaction: %.0f kN | moved this step %.4f m3 | state tiles %d\n\n" % [
			_bucket_total_reaction_n / 1000.0, _bucket_last_moved_volume_m3,
			int(state_stats.get("state_tiles", 0))] +
		"R place + DROP sphere at aim | P pause/resume | Backspace reset suspended\n" +
		"Bucket: I/K forward/back  J/L left/right  U/O up/down  Z/X pitch  Shift fast  B reset\n" +
		"1 topsoil  2 wet clay  3 gravel  4 rock | Delete clears terrain edits | F10 close"
	)
