extends Node
## Double-precision barycentric celestial mechanics for the Helion system.
##
## Translation is true Newtonian N-body gravity in SI units. Asterra starts on
## the locked Helion orbit and both bodies move around their common barycentre.
## Body spin is handled separately as rigid rotation because spin angular
## momentum is not part of the point-mass orbital integrator.

signal state_updated(simulation_seconds: float)

const G := 6.67430e-11
const AU_M := 149597870700.0
const SOLAR_MASS_KG := 1.98847e30
const SOLAR_RADIUS_M := 695700000.0
const SOLAR_LUMINOSITY_W := 3.828e26

## --- Locked Helion parameters -------------------------------------------------
const HELION_MASS_KG := 0.97 * SOLAR_MASS_KG
const HELION_RADIUS_M := 0.96 * SOLAR_RADIUS_M
const HELION_EFFECTIVE_TEMPERATURE_K := 5600.0
const HELION_LUMINOSITY_W := 0.8165622236300686 * SOLAR_LUMINOSITY_W

## --- Locked Asterra parameters ------------------------------------------------
const ASTERRA_RADIUS_M := 3500000.0
const ASTERRA_SURFACE_GRAVITY_MPS2 := 9.80665
const ASTERRA_MASS_KG := ASTERRA_SURFACE_GRAVITY_MPS2 * ASTERRA_RADIUS_M * ASTERRA_RADIUS_M / G
const ASTERRA_ROTATION_PERIOD_S := 11.5 * 3600.0
const ASTERRA_AXIAL_TILT_DEG := 26.0
const ASTERRA_SURFACE_PRESSURE_PA := 110000.0

## Orbit chosen so Helion delivers 1420 W/m^2 at the semi-major axis.
const ASTERRA_MEAN_IRRADIANCE_W_M2 := 1420.0
const ASTERRA_SEMI_MAJOR_AXIS_M := 132352302970.23317
const ASTERRA_ECCENTRICITY := 0.035

## Velocity-Verlet remains extremely accurate at this step for the current
## ~309-Earth-day orbit while leaving ample headroom for future moons.
const MAX_INTEGRATION_STEP_S := 300.0
const MAX_TIME_SCALE := 1000000.0

class Body:
	var body_name: String
	var mass_kg: float
	var radius_m: float
	var position_m: Vec3D
	var velocity_mps: Vec3D

	func _init(
		p_name: String,
		p_mass_kg: float,
		p_radius_m: float,
		p_position_m: Vec3D,
		p_velocity_mps: Vec3D
	) -> void:
		body_name = p_name
		mass_kg = p_mass_kg
		radius_m = p_radius_m
		position_m = p_position_m
		velocity_mps = p_velocity_mps

var time_scale: float = 1.0
var simulation_seconds: float = 0.0
var asterra_spin_phase_rad: float = 0.0

var _bodies: Array = []
var _helion_index := -1
var _asterra_index := -1

func _ready() -> void:
	process_priority = -200
	reset_system()

func _process(delta: float) -> void:
	if time_scale == 0.0 or delta <= 0.0:
		return
	advance_seconds(delta * time_scale)

func reset_system() -> void:
	_bodies.clear()
	simulation_seconds = 0.0
	asterra_spin_phase_rad = 0.0

	# Start at perihelion and at an equinox. The orbital plane is XZ with angular
	# momentum +Y. The barycentre is exactly at the origin and total momentum is
	# exactly zero, so Helion is not treated as an immovable background prop.
	var total_mass := HELION_MASS_KG + ASTERRA_MASS_KG
	var separation := ASTERRA_SEMI_MAJOR_AXIS_M * (1.0 - ASTERRA_ECCENTRICITY)
	var relative_speed := sqrt(
		G * total_mass * (1.0 + ASTERRA_ECCENTRICITY)
		/ (ASTERRA_SEMI_MAJOR_AXIS_M * (1.0 - ASTERRA_ECCENTRICITY))
	)

	var helion_fraction := ASTERRA_MASS_KG / total_mass
	var asterra_fraction := HELION_MASS_KG / total_mass
	var relative_position := Vec3D.new(separation, 0.0, 0.0)
	var relative_velocity := Vec3D.new(0.0, 0.0, -relative_speed)

	var helion := Body.new(
		"Helion",
		HELION_MASS_KG,
		HELION_RADIUS_M,
		relative_position.mul(-helion_fraction),
		relative_velocity.mul(-helion_fraction)
	)
	var asterra := Body.new(
		"Asterra",
		ASTERRA_MASS_KG,
		ASTERRA_RADIUS_M,
		relative_position.mul(asterra_fraction),
		relative_velocity.mul(asterra_fraction)
	)

	_helion_index = 0
	_asterra_index = 1
	_bodies.append(helion)
	_bodies.append(asterra)
	state_updated.emit(simulation_seconds)

func set_time_scale(value: float) -> void:
	time_scale = clampf(value, -MAX_TIME_SCALE, MAX_TIME_SCALE)

func advance_seconds(sim_seconds: float) -> void:
	if sim_seconds == 0.0 or _bodies.size() < 2:
		return

	var steps := maxi(1, int(ceil(abs(sim_seconds) / MAX_INTEGRATION_STEP_S)))
	var dt := sim_seconds / float(steps)
	for _step in range(steps):
		_velocity_verlet_step(dt)
		simulation_seconds += dt
		asterra_spin_phase_rad = fposmod(
			asterra_spin_phase_rad + TAU * dt / ASTERRA_ROTATION_PERIOD_S,
			TAU
		)
	state_updated.emit(simulation_seconds)

func _velocity_verlet_step(dt: float) -> void:
	var acceleration_0 := _compute_accelerations()
	var half_dt2 := 0.5 * dt * dt

	for i in range(_bodies.size()):
		var body: Body = _bodies[i]
		var a0: Vec3D = acceleration_0[i]
		body.position_m = body.position_m.add_scaled(body.velocity_mps, dt).add_scaled(a0, half_dt2)

	var acceleration_1 := _compute_accelerations()
	var half_dt := 0.5 * dt
	for i in range(_bodies.size()):
		var body: Body = _bodies[i]
		var a0: Vec3D = acceleration_0[i]
		var a1: Vec3D = acceleration_1[i]
		body.velocity_mps = body.velocity_mps.add_scaled(a0.add(a1), half_dt)

func _compute_accelerations() -> Array:
	var accelerations: Array = []
	for _i in range(_bodies.size()):
		accelerations.append(Vec3D.new())

	for i in range(_bodies.size()):
		var bi: Body = _bodies[i]
		for j in range(i + 1, _bodies.size()):
			var bj: Body = _bodies[j]
			var r := bj.position_m.sub(bi.position_m)
			var distance_sq := r.length_sq()
			if distance_sq <= 1.0:
				continue
			var inv_distance := 1.0 / sqrt(distance_sq)
			var inv_distance_cubed := inv_distance * inv_distance * inv_distance
			var pair_direction := r.mul(G * inv_distance_cubed)
			accelerations[i] = (accelerations[i] as Vec3D).add_scaled(pair_direction, bj.mass_kg)
			accelerations[j] = (accelerations[j] as Vec3D).add_scaled(pair_direction, -bi.mass_kg)
	return accelerations

func body_count() -> int:
	return _bodies.size()

func body_state(body_name: String) -> Dictionary:
	for body_variant in _bodies:
		var body: Body = body_variant
		if body.body_name == body_name:
			return {
				"name": body.body_name,
				"mass_kg": body.mass_kg,
				"radius_m": body.radius_m,
				"position_m": body.position_m.dup(),
				"velocity_mps": body.velocity_mps.dup(),
			}
	return {}

func asterra_helion_distance_m() -> float:
	if _helion_index < 0 or _asterra_index < 0:
		return ASTERRA_SEMI_MAJOR_AXIS_M
	var helion: Body = _bodies[_helion_index]
	var asterra: Body = _bodies[_asterra_index]
	return asterra.position_m.distance_to(helion.position_m)

func asterra_irradiance_w_m2() -> float:
	var distance_m := maxf(asterra_helion_distance_m(), 1.0)
	return HELION_LUMINOSITY_W / (4.0 * PI * distance_m * distance_m)

func asterra_irradiance_ratio() -> float:
	return asterra_irradiance_w_m2() / ASTERRA_MEAN_IRRADIANCE_W_M2

func helion_angular_diameter_deg() -> float:
	var distance_m := maxf(asterra_helion_distance_m(), HELION_RADIUS_M + 1.0)
	return rad_to_deg(2.0 * atan(HELION_RADIUS_M / distance_m))

func orbital_period_seconds() -> float:
	return TAU * sqrt(
		ASTERRA_SEMI_MAJOR_AXIS_M * ASTERRA_SEMI_MAJOR_AXIS_M * ASTERRA_SEMI_MAJOR_AXIS_M
		/ (G * (HELION_MASS_KG + ASTERRA_MASS_KG))
	)

func orbital_period_asterra_days() -> float:
	return orbital_period_seconds() / ASTERRA_ROTATION_PERIOD_S

func sun_dir_body() -> Vector3:
	if _helion_index < 0 or _asterra_index < 0:
		return Vector3(-1.0, 0.0, 0.0)

	var helion: Body = _bodies[_helion_index]
	var asterra: Body = _bodies[_asterra_index]
	var sun_inertial := helion.position_m.sub(asterra.position_m).normalized()

	# Body-fixed Y is the spin axis. At t=0 the tilt is toward +Z and Helion lies
	# on the equator, so the initial state is an equinox. X/Z rotate rigidly around
	# that axis with the locked 11.5-hour sidereal period.
	var tilt := deg_to_rad(ASTERRA_AXIAL_TILT_DEG)
	var north_axis := Vec3D.new(0.0, cos(tilt), sin(tilt))
	var x0 := Vec3D.new(1.0, 0.0, 0.0)
	var z0 := x0.cross(north_axis).normalized()
	var c := cos(asterra_spin_phase_rad)
	var s := sin(asterra_spin_phase_rad)
	var x_axis := x0.mul(c).add_scaled(z0, -s)
	var z_axis := x0.mul(s).add_scaled(z0, c)

	return Vector3(
		float(sun_inertial.dot(x_axis)),
		float(sun_inertial.dot(north_axis)),
		float(sun_inertial.dot(z_axis))
	).normalized()

func helion_declination_deg() -> float:
	return rad_to_deg(asin(clampf(sun_dir_body().y, -1.0, 1.0)))

func total_momentum_kg_mps() -> Vec3D:
	var total := Vec3D.new()
	for body_variant in _bodies:
		var body: Body = body_variant
		total = total.add_scaled(body.velocity_mps, body.mass_kg)
	return total

func total_energy_j() -> float:
	var kinetic := 0.0
	var potential := 0.0
	for i in range(_bodies.size()):
		var bi: Body = _bodies[i]
		kinetic += 0.5 * bi.mass_kg * bi.velocity_mps.length_sq()
		for j in range(i + 1, _bodies.size()):
			var bj: Body = _bodies[j]
			var distance_m := maxf(bi.position_m.distance_to(bj.position_m), 1.0)
			potential -= G * bi.mass_kg * bj.mass_kg / distance_m
	return kinetic + potential
