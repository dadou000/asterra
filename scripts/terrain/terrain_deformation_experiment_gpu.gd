extends "res://scripts/terrain/terrain_deformation_experiment.gd"
## GPU-stage experiment override: the drop sphere submits its real spherical cap
## footprint instead of the generic radial depression kernel.


func _step_sphere(dt: float) -> void:
	var ground_height_m: float = TerrainContactSampler.height(_sphere_dir)
	var bottom_altitude_m: float = _sphere_altitude_msl - SPHERE_RADIUS_M
	var penetration_m: float = ground_height_m - bottom_altitude_m
	var acceleration_mps2: float = -GRAVITY_MPS2
	_sphere_last_contact.clear()
	if penetration_m > 0.0:
		var capped_penetration_m: float = minf(penetration_m, SPHERE_RADIUS_M)
		var contact_radius_sq: float = 2.0 * SPHERE_RADIUS_M * capped_penetration_m
		contact_radius_sq -= capped_penetration_m * capped_penetration_m
		var contact_radius_m: float = sqrt(maxf(contact_radius_sq, 0.01))
		contact_radius_m = clampf(contact_radius_m, 0.12, SPHERE_RADIUS_M)
		var inward_speed_mps: float = maxf(-_sphere_velocity_mps, 0.0)
		var stopping_distance_m: float = maxf(0.25, minf(SPHERE_RADIUS_M, penetration_m + 0.30))
		var impact_force_n: float = _sphere_mass_kg * inward_speed_mps * inward_speed_mps
		impact_force_n /= 2.0 * stopping_distance_m
		var load_n: float = _sphere_mass_kg * GRAVITY_MPS2 + impact_force_n
		_sphere_last_contact = TerrainDeformation.apply_contact(
			_sphere_dir, contact_radius_m, load_n, penetration_m,
			_sphere_velocity_mps, 0.0, dt, 0.0, material_id,
			TerrainDeformation.FOOTPRINT_SPHERE, SPHERE_RADIUS_M)
		var support_force_n: float = float(_sphere_last_contact.get("support_force_n", 0.0))
		acceleration_mps2 += support_force_n / maxf(_sphere_mass_kg, 1.0)
		acceleration_mps2 = clampf(acceleration_mps2, -180.0, 260.0)
	_sphere_velocity_mps += acceleration_mps2 * dt
	_sphere_velocity_mps = clampf(_sphere_velocity_mps, -180.0, 80.0)
	_sphere_altitude_msl += _sphere_velocity_mps * dt
	if penetration_m > 0.0 and absf(_sphere_velocity_mps) < 0.015:
		var support_n: float = float(_sphere_last_contact.get("support_force_n", 0.0))
		var weight_n: float = _sphere_mass_kg * GRAVITY_MPS2
		if support_n >= weight_n * 0.98:
			_sphere_velocity_mps = 0.0
