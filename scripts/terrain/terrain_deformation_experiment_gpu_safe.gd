extends "res://scripts/terrain/terrain_deformation_experiment_gpu.gd"
## No-tunnelling guard for asynchronous rendered-terrain contact acquisition.
##
## Exact rendered terrain remains the only surface allowed to START deformation.
## While that GPU sample is unavailable, a contact-grade terrain query (and, only
## as a last resort, the ordinary nearby fallback) acts as a temporary safety
## barrier. The sphere is held there without deforming anything and its incoming
## impact velocity is preserved. As soon as the exact rendered sample arrives the
## sphere is moved to that exact surface and the saved impact velocity is handed to
## the normal elasto-plastic solver on the following substep.

var _sphere_waiting_for_rendered_contact := false
var _sphere_saved_impact_velocity_mps := 0.0
var _sphere_safety_source := "none"


func _place_sphere(drop_immediately: bool) -> void:
	super._place_sphere(drop_immediately)
	_sphere_waiting_for_rendered_contact = false
	_sphere_saved_impact_velocity_mps = 0.0
	_sphere_safety_source = "none"


func _strict_safety_ground_height() -> float:
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

	# If an active deformation tile already exists here, do not mix a fresh
	# pristine sample with stale deformation state. Request the point and wait until
	# its field generation catches up.
	if TerrainDeformationGPU.ready_state and not TerrainDeformationGPU.failed \
			and bool(TerrainDeformationGPU.get("_has_active_content")):
		if TerrainDeformationQueryGPU.ready_state and not TerrainDeformationQueryGPU.failed:
			TerrainDeformationGPU.active_height_offset(_sphere_dir)
			if TerrainDeformationQueryGPU.has_method("has_fresh_sample") \
					and bool(TerrainDeformationQueryGPU.call("has_fresh_sample", _sphere_dir)):
				height_m += TerrainDeformationGPU.active_height_offset(_sphere_dir)
			else:
				return NAN
		elif not TerrainDeformationQueryGPU.failed:
			return NAN
	return height_m


func _temporary_safety_ground_height() -> float:
	var strict_height_m: float = _strict_safety_ground_height()
	if is_finite(strict_height_m):
		_sphere_safety_source = "strict"
		return strict_height_m

	# Emergency anti-tunnelling barrier only. This broad fallback is NEVER passed to
	# TerrainDeformation.apply_contact(). It merely prevents an asynchronous query
	# miss from allowing the object to disappear through the planet. Once an exact
	# rendered sample arrives, this temporary plane is discarded without doing soil
	# work.
	var fallback_height_m: float = TerrainContactSampler.height(_sphere_dir)
	if is_finite(fallback_height_m):
		_sphere_safety_source = "fallback"
		return fallback_height_m
	_sphere_safety_source = "none"
	return NAN


func _lock_exact_rendered_contact(rendered_ground_m: float) -> void:
	_sphere_reference_ground_m = rendered_ground_m
	_sphere_contact_ground_m = rendered_ground_m
	_sphere_total_center_sink_m = 0.0
	_sphere_reference_ready = true
	_sphere_contact_started = true
	_sphere_altitude_msl = rendered_ground_m + SPHERE_RADIUS_M
	if _sphere_waiting_for_rendered_contact:
		_sphere_velocity_mps = _sphere_saved_impact_velocity_mps
	_sphere_waiting_for_rendered_contact = false
	_sphere_saved_impact_velocity_mps = 0.0
	_sphere_safety_source = "exact"
	_sphere_last_contact.clear()


func _step_sphere(dt: float) -> void:
	if _sphere_contact_started:
		super._step_sphere(dt)
		return

	# Exact renderer-derived height always wins. If we had to stop on a temporary
	# safety plane, transfer the preserved incoming velocity to the exact surface,
	# but do not deform until the following substep.
	var rendered_ground_m: float = _rendered_ground_height()
	if is_finite(rendered_ground_m):
		if _sphere_waiting_for_rendered_contact:
			_lock_exact_rendered_contact(rendered_ground_m)
			return
		# Let the normal exact-rendered crossing code handle ordinary freefall.
		super._step_sphere(dt)
		return

	var safety_ground_m: float = _temporary_safety_ground_height()
	if not is_finite(safety_ground_m):
		# There is genuinely no terrain information at all yet. Continue freefall,
		# but the usual query requests above keep both exact and fallback paths hot.
		_integrate_freefall(dt)
		_sphere_last_contact.clear()
		return

	if _sphere_waiting_for_rendered_contact:
		# Update the temporary holding plane if the stricter fallback becomes ready,
		# but do not accumulate extra gravitational energy while waiting for the GPU.
		_sphere_altitude_msl = safety_ground_m + SPHERE_RADIUS_M
		_sphere_velocity_mps = 0.0
		_sphere_last_contact.clear()
		return

	var next_velocity_mps: float = clampf(
		_sphere_velocity_mps - GRAVITY_MPS2 * dt, -180.0, 80.0)
	var next_altitude_m: float = _sphere_altitude_msl + next_velocity_mps * dt
	var next_bottom_m: float = next_altitude_m - SPHERE_RADIUS_M
	if next_bottom_m > safety_ground_m:
		_sphere_velocity_mps = next_velocity_mps
		_sphere_altitude_msl = next_altitude_m
		_sphere_last_contact.clear()
		return

	# Crossing occurred before the exact rendered query completed. Never tunnel and
	# never convert query latency into deformation. Hold at the safety plane and save
	# the real incoming velocity for exact contact acquisition.
	_sphere_saved_impact_velocity_mps = next_velocity_mps
	_sphere_altitude_msl = safety_ground_m + SPHERE_RADIUS_M
	_sphere_velocity_mps = 0.0
	_sphere_waiting_for_rendered_contact = true
	_sphere_last_contact.clear()
