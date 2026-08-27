extends "res://scripts/terrain/terrain_deformation_experiment.gd"
## GPU-stage experiment override.
##
## The sphere submits its real spherical-cap footprint and uses a contact
## projection step so rigid-body penetration cannot run ahead of the terrain's
## plastic deformation. GPU height queries are asynchronous, therefore a tiny
## speculative center-sink accumulator bridges the one/two-frame query delay.

var _sphere_last_observed_ground_m: float = INF
var _sphere_unseen_gpu_sink_m := 0.0


func _place_sphere(drop_immediately: bool) -> void:
	super._place_sphere(drop_immediately)
	if not _world_ready:
		return
	_sphere_last_observed_ground_m = TerrainContactSampler.height(_sphere_dir)
	_sphere_unseen_gpu_sink_m = 0.0


func _step_sphere(dt: float) -> void:
	# The rendered GPU field can advance before its tiny asynchronous point query
	# reaches the CPU. Consume the speculative sink as soon as that queried surface
	# catches up, otherwise use it immediately for contact geometry.
	var observed_ground_m: float = TerrainContactSampler.height(_sphere_dir)
	if is_finite(_sphere_last_observed_ground_m):
		var observed_drop_m: float = maxf(
			_sphere_last_observed_ground_m - observed_ground_m, 0.0)
		_sphere_unseen_gpu_sink_m = maxf(
			_sphere_unseen_gpu_sink_m - observed_drop_m, 0.0)
	_sphere_last_observed_ground_m = observed_ground_m
	var physics_ground_m: float = observed_ground_m - _sphere_unseen_gpu_sink_m

	var bottom_altitude_m: float = _sphere_altitude_msl - SPHERE_RADIUS_M
	var penetration_m: float = physics_ground_m - bottom_altitude_m
	var acceleration_mps2: float = -GRAVITY_MPS2
	var velocity_before_contact_mps: float = _sphere_velocity_mps
	var plastic_center_step_m := 0.0
	var effective_restitution := 0.0
	_sphere_last_contact.clear()

	if penetration_m > 0.0:
		var capped_penetration_m: float = minf(penetration_m, SPHERE_RADIUS_M)
		var contact_radius_sq: float = 2.0 * SPHERE_RADIUS_M * capped_penetration_m
		contact_radius_sq -= capped_penetration_m * capped_penetration_m
		var contact_radius_m: float = sqrt(maxf(contact_radius_sq, 0.01))
		contact_radius_m = clampf(contact_radius_m, 0.12, SPHERE_RADIUS_M)

		var inward_speed_mps: float = maxf(-_sphere_velocity_mps, 0.0)
		var stopping_distance_m: float = maxf(
			0.18, minf(SPHERE_RADIUS_M, penetration_m + 0.22))
		var impact_force_n: float = _sphere_mass_kg * inward_speed_mps * inward_speed_mps
		impact_force_n /= 2.0 * stopping_distance_m
		var load_n: float = _sphere_mass_kg * GRAVITY_MPS2 + impact_force_n

		_sphere_last_contact = TerrainDeformation.apply_contact(
			_sphere_dir, contact_radius_m, load_n, penetration_m,
			_sphere_velocity_mps, 0.0, dt, 0.0, material_id,
			TerrainDeformation.FOOTPRINT_SPHERE, SPHERE_RADIUS_M)

		# Yield/bearing strength is a capacity, not a force source. Once capacity is
		# greater than the demanded contact load it should support that load, not
		# continue accelerating the body upward like an over-stiff penalty spring.
		var raw_support_force_n: float = float(
			_sphere_last_contact.get("support_force_n", 0.0))
		var support_force_n: float = minf(raw_support_force_n, load_n)
		_sphere_last_contact["support_capacity_force_n"] = raw_support_force_n
		_sphere_last_contact["support_force_n"] = support_force_n
		acceleration_mps2 += support_force_n / maxf(_sphere_mass_kg, 1.0)
		acceleration_mps2 = clampf(acceleration_mps2, -180.0, 260.0)

		plastic_center_step_m = maxf(
			float(_sphere_last_contact.get("deformed_m", 0.0)), 0.0)
		if bool(_sphere_last_contact.get("gpu_active", false)):
			_sphere_unseen_gpu_sink_m += plastic_center_step_m

		# Plastic work destroys rebound energy. Only the fraction of overlap which
		# was *not* plastically yielded is allowed to contribute restitution.
		var plastic_fraction: float = clampf(
			plastic_center_step_m / maxf(penetration_m, 1e-5), 0.0, 1.0)
		effective_restitution = float(
			_sphere_last_contact.get("restitution", 0.0)) * (1.0 - plastic_fraction)
		_sphere_last_contact["effective_restitution"] = effective_restitution

	_sphere_velocity_mps += acceleration_mps2 * dt
	_sphere_velocity_mps = clampf(_sphere_velocity_mps, -180.0, 80.0)
	_sphere_altitude_msl += _sphere_velocity_mps * dt

	if penetration_m > 0.0:
		# Position-level contact projection: the sphere can descend only as fast as
		# the material actually yields. This prevents the rigid object from tunneling
		# several decimetres/metres into a terrain field which has only yielded a few
		# centimetres during the same physics step.
		var predicted_ground_after_m: float = (
			observed_ground_m - _sphere_unseen_gpu_sink_m)
		var minimum_center_altitude_m: float = predicted_ground_after_m + SPHERE_RADIUS_M
		if _sphere_altitude_msl < minimum_center_altitude_m:
			_sphere_altitude_msl = minimum_center_altitude_m
			var terrain_retreat_speed_mps: float = -plastic_center_step_m / maxf(dt, 1e-6)
			if _sphere_velocity_mps < terrain_retreat_speed_mps:
				_sphere_velocity_mps = terrain_retreat_speed_mps

		# Explicitly enforce the material restitution ceiling when a contact impulse
		# would otherwise reverse the velocity and create the visible pogo bounce.
		if velocity_before_contact_mps < 0.0 and _sphere_velocity_mps > 0.0:
			var rebound_limit_mps: float = -velocity_before_contact_mps * effective_restitution
			_sphere_velocity_mps = minf(_sphere_velocity_mps, rebound_limit_mps)

		if absf(_sphere_velocity_mps) < 0.015:
			var support_n: float = float(_sphere_last_contact.get("support_force_n", 0.0))
			var weight_n: float = _sphere_mass_kg * GRAVITY_MPS2
			if support_n >= weight_n * 0.98:
				_sphere_velocity_mps = 0.0
