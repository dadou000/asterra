extends "res://scripts/terrain/terrain_deformation_experiment.gd"
## GPU-stage experiment override.
##
## The sphere keeps a tiny immediate CPU-side contact datum while the detailed
## deformation remains GPU resident. This avoids feeding delayed GPU readback back
## into the same contact constraint that produced it. The datum only tracks the
## center-line plastic retreat; the GPU still owns the actual 2D crater profile.
##
## Sphere footprint geometry is based on total embed depth below the original
## contact plane, not instantaneous overlap against already-yielded soil. Otherwise
## the overlap stays near zero while the sphere follows the yielding surface and a
## deep, impossibly narrow needle hole is produced.

var _sphere_reference_ground_m := 0.0
var _sphere_contact_ground_m := 0.0
var _sphere_total_center_sink_m := 0.0


func _place_sphere(drop_immediately: bool) -> void:
	super._place_sphere(drop_immediately)
	if not _world_ready:
		return
	var h: float = TerrainContactSampler.height(_sphere_dir)
	_sphere_reference_ground_m = h
	_sphere_contact_ground_m = h
	_sphere_total_center_sink_m = 0.0


func _step_sphere(dt: float) -> void:
	# Contact uses the immediate local datum instead of an asynchronous GPU sample.
	# The GPU result remains authoritative visually; this scalar exists only to make
	# the current rigid contact causally consistent with the deformation command it
	# submitted during the same physics step.
	var bottom_altitude_m: float = _sphere_altitude_msl - SPHERE_RADIUS_M
	var overlap_m: float = _sphere_contact_ground_m - bottom_altitude_m
	var embed_depth_m: float = maxf(_sphere_reference_ground_m - bottom_altitude_m, 0.0)
	var acceleration_mps2: float = -GRAVITY_MPS2
	var velocity_before_contact_mps: float = _sphere_velocity_mps
	var plastic_center_step_m := 0.0
	var effective_restitution := 0.0
	_sphere_last_contact.clear()

	if overlap_m > 0.0:
		# The spherical contact patch grows with total embed depth relative to the
		# original far-field surface. Once the equator is below that surface the
		# heightfield cannot represent an overhang, so keep a full-radius bore rather
		# than letting the footprint shrink again.
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

		# For the spherical test use embed depth as the footprint depth. It also adds
		# confinement with burial depth to this first-order soil model. The actual
		# contact overlap is retained in the returned diagnostics below.
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

		# Plastic deformation consumes impact energy. Wet/loose ground should remain
		# in contact rather than acting like a penalty spring. Hard ground retains the
		# material restitution already defined by the terrain preset.
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
				# A yielding, highly damped surface keeps the body attached to the retreating
				# support surface. This removes the stop/fall/stop numerical chatter.
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
