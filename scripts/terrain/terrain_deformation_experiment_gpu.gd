extends "res://scripts/terrain/terrain_deformation_experiment.gd"
## GPU-stage experiment override.
##
## The sphere keeps a tiny immediate CPU-side contact datum while the detailed
## deformation remains GPU resident. The datum is created only after a strict
## contact-grade terrain sample exists at the sphere direction; broad nearby cache
## samples are never allowed to start a contact.
##
## Sphere footprint geometry is based on total embed depth below the original
## contact plane, not instantaneous overlap against already-yielded soil.

var _sphere_reference_ground_m := 0.0
var _sphere_contact_ground_m := 0.0
var _sphere_total_center_sink_m := 0.0
var _sphere_reference_ready := false
var _sphere_contact_started := false


func _place_sphere(drop_immediately: bool) -> void:
	super._place_sphere(drop_immediately)
	if not _world_ready:
		return
	_sphere_reference_ground_m = 0.0
	_sphere_contact_ground_m = 0.0
	_sphere_total_center_sink_m = 0.0
	_sphere_reference_ready = false
	_sphere_contact_started = false
	TerrainHeightQuery.request_height(_sphere_dir)
	var strict_ground_m: float = _strict_ground_height()
	if is_finite(strict_ground_m):
		_sphere_reference_ground_m = strict_ground_m
		_sphere_contact_ground_m = strict_ground_m
		_sphere_reference_ready = true


func _strict_ground_height() -> float:
	# Pristine terrain must come from a sample within 45 cm of the requested point.
	# The ordinary TerrainContactSampler.height() intentionally accepts much farther
	# cached samples as a temporary fallback, which is unsuitable for first contact.
	TerrainHeightQuery.request_height(_sphere_dir)
	if not TerrainHeightQuery.has_method("has_contact_height") \
			or not bool(TerrainHeightQuery.call("has_contact_height", _sphere_dir)):
		return NAN
	var base_value: Variant = TerrainHeightQuery.call(
		"contact_height_for_direction", _sphere_dir, NAN)
	var base_height_m: float = float(base_value)
	if not is_finite(base_height_m):
		return NAN

	# If a live GPU deformation field already exists at this point, require its
	# point-query cache to be current too. Otherwise a previous crater could again
	# make the rigid contact plane disagree with the visible terrain.
	if TerrainDeformationGPU.ready_state and not TerrainDeformationGPU.failed \
			and bool(TerrainDeformationGPU.get("_has_active_content")):
		if TerrainDeformationQueryGPU.ready_state and not TerrainDeformationQueryGPU.failed:
			TerrainDeformationGPU.active_height_offset(_sphere_dir)
			if not TerrainDeformationQueryGPU.has_method("has_fresh_sample") \
					or not bool(TerrainDeformationQueryGPU.call("has_fresh_sample", _sphere_dir)):
				return NAN
		elif not TerrainDeformationQueryGPU.failed:
			return NAN
		base_height_m += TerrainDeformationGPU.active_height_offset(_sphere_dir)
	return base_height_m


func _integrate_freefall(dt: float) -> void:
	_sphere_velocity_mps -= GRAVITY_MPS2 * dt
	_sphere_velocity_mps = clampf(_sphere_velocity_mps, -180.0, 80.0)
	_sphere_altitude_msl += _sphere_velocity_mps * dt


func _step_sphere(dt: float) -> void:
	# Before first contact, continuously refresh the exact surface plane. No terrain
	# deformation is submitted until the sphere has geometrically crossed that exact
	# plane. This makes an airborne sphere incapable of excavating terrain.
	if not _sphere_contact_started:
		var strict_ground_m: float = _strict_ground_height()
		if not is_finite(strict_ground_m):
			_integrate_freefall(dt)
			_sphere_last_contact.clear()
			return

		_sphere_reference_ground_m = strict_ground_m
		_sphere_contact_ground_m = strict_ground_m
		_sphere_total_center_sink_m = 0.0
		_sphere_reference_ready = true

		var bottom_now_m: float = _sphere_altitude_msl - SPHERE_RADIUS_M
		var clearance_now_m: float = bottom_now_m - strict_ground_m
		if clearance_now_m > 0.0:
			var next_velocity_mps: float = clampf(
				_sphere_velocity_mps - GRAVITY_MPS2 * dt, -180.0, 80.0)
			var next_altitude_m: float = _sphere_altitude_msl + next_velocity_mps * dt
			var next_bottom_m: float = next_altitude_m - SPHERE_RADIUS_M
			if next_bottom_m > strict_ground_m:
				_sphere_velocity_mps = next_velocity_mps
				_sphere_altitude_msl = next_altitude_m
				_sphere_last_contact.clear()
				return
			# Continuous crossing: land exactly on the sampled visible surface and keep
			# the incoming velocity for the following contact substep.
			_sphere_velocity_mps = next_velocity_mps
			_sphere_altitude_msl = strict_ground_m + SPHERE_RADIUS_M
			_sphere_contact_started = true
			_sphere_last_contact.clear()
			return

		# A strict sample arrived after the sphere was already marginally below it.
		# Clamp to first touch rather than converting that query latency into soil work.
		_sphere_altitude_msl = strict_ground_m + SPHERE_RADIUS_M
		_sphere_contact_started = true
		_sphere_last_contact.clear()
		return

	if not _sphere_reference_ready:
		_integrate_freefall(dt)
		return

	var bottom_altitude_m: float = _sphere_altitude_msl - SPHERE_RADIUS_M
	var overlap_m: float = _sphere_contact_ground_m - bottom_altitude_m
	var embed_depth_m: float = maxf(_sphere_reference_ground_m - bottom_altitude_m, 0.0)
	var acceleration_mps2: float = -GRAVITY_MPS2
	var velocity_before_contact_mps: float = _sphere_velocity_mps
	var plastic_center_step_m := 0.0
	var effective_restitution := 0.0
	_sphere_last_contact.clear()

	if overlap_m > 0.0:
		var geometry_depth_m: float = minf(embed_depth_m, SPHERE_RADIUS_M)
		var contact_radius_m := SPHERE_RADIUS_M
		if geometry_depth_m < SPHERE_RADIUS_M:
			var contact_radius_sq: float = 2.0 * SPHERE_RADIUS_M * geometry_depth_m
			contact_radius_sq -= geometry_depth_m * geometry_depth_m
			contact_radius_m = sqrt(maxf(contact_radius_sq, 0.01))
		contact_radius_m = clampf(contact_radius_m, 0.12, SPHERE_RADIUS_M)

		var inward_speed_mps: float = maxf(-_sphere_velocity_mps, 0.0)
		var stopping_distance_m: float = maxf(
			0.18, minf(SPHERE_RADIUS_M, overlap_m + 0.22))
		var impact_force_n: float = _sphere_mass_kg * inward_speed_mps * inward_speed_mps
		impact_force_n /= 2.0 * stopping_distance_m
		var load_n: float = _sphere_mass_kg * GRAVITY_MPS2 + impact_force_n

		_sphere_last_contact = TerrainDeformation.apply_contact(
			_sphere_dir, contact_radius_m, load_n, maxf(embed_depth_m, 1e-5),
			_sphere_velocity_mps, 0.0, dt, 0.0, material_id,
			TerrainDeformation.FOOTPRINT_SPHERE, SPHERE_RADIUS_M)
		_sphere_last_contact["contact_overlap_m"] = overlap_m
		_sphere_last_contact["embed_depth_m"] = embed_depth_m
		_sphere_last_contact["contact_radius_m"] = contact_radius_m

		var support_force_n: float = float(_sphere_last_contact.get("support_force_n", 0.0))
		acceleration_mps2 += support_force_n / maxf(_sphere_mass_kg, 1.0)
		acceleration_mps2 = clampf(acceleration_mps2, -180.0, 260.0)

		plastic_center_step_m = maxf(
			float(_sphere_last_contact.get("deformed_m", 0.0)), 0.0)
		if plastic_center_step_m > 0.0:
			_sphere_total_center_sink_m += plastic_center_step_m
			_sphere_contact_ground_m = _sphere_reference_ground_m - _sphere_total_center_sink_m

		var plastic_fraction: float = clampf(
			plastic_center_step_m / maxf(overlap_m, 1e-5), 0.0, 1.0)
		effective_restitution = float(
			_sphere_last_contact.get("restitution", 0.0)) * (1.0 - plastic_fraction)
		_sphere_last_contact["effective_restitution"] = effective_restitution
		_sphere_last_contact["contact_ground_m"] = _sphere_contact_ground_m
		_sphere_last_contact["total_center_sink_m"] = _sphere_total_center_sink_m

	_sphere_velocity_mps += acceleration_mps2 * dt
	_sphere_velocity_mps = clampf(_sphere_velocity_mps, -180.0, 80.0)
	_sphere_altitude_msl += _sphere_velocity_mps * dt

	if overlap_m > 0.0:
		var minimum_center_altitude_m: float = _sphere_contact_ground_m + SPHERE_RADIUS_M
		var projected := false
		if _sphere_altitude_msl < minimum_center_altitude_m:
			_sphere_altitude_msl = minimum_center_altitude_m
			projected = true

		var terrain_retreat_speed_mps: float = -plastic_center_step_m / maxf(dt, 1e-6)
		if projected:
			if effective_restitution <= 0.03:
				_sphere_velocity_mps = terrain_retreat_speed_mps
			elif velocity_before_contact_mps < 0.0 and _sphere_velocity_mps > 0.0:
				var rebound_limit_mps: float = -velocity_before_contact_mps * effective_restitution
				_sphere_velocity_mps = minf(_sphere_velocity_mps, rebound_limit_mps)
			elif _sphere_velocity_mps < terrain_retreat_speed_mps:
				_sphere_velocity_mps = terrain_retreat_speed_mps
		elif velocity_before_contact_mps < 0.0 and _sphere_velocity_mps > 0.0:
			var rebound_limit_mps: float = -velocity_before_contact_mps * effective_restitution
			_sphere_velocity_mps = minf(_sphere_velocity_mps, rebound_limit_mps)

		if absf(_sphere_velocity_mps) < 0.015:
			var support_n: float = float(_sphere_last_contact.get("support_force_n", 0.0))
			var weight_n: float = _sphere_mass_kg * GRAVITY_MPS2
			if support_n >= weight_n * 0.98 and plastic_center_step_m < 1e-5:
				_sphere_velocity_mps = 0.0
