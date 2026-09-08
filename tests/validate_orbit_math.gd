extends SceneTree
## Headless invariants for OrbitMath (scripts/world_authoring/model/orbit_math.gd).
## Run: godot --headless --path . --script res://tests/validate_orbit_math.gd

const ORBIT_MATH := preload("res://scripts/world_authoring/model/orbit_math.gd")
const ORBIT_SCRIPT := preload("res://scripts/world_authoring/model/orbit_definition.gd")
const BODY_SCRIPT := preload("res://scripts/world_authoring/model/celestial_body_definition.gd")
const SYSTEM_SCRIPT := preload("res://scripts/world_authoring/model/celestial_system_definition.gd")

const AU := 149_597_870_700.0
const SOL_MU := 1.327_124_400_18e20


var _failed := false


func _init() -> void:
	_test_kepler_third_law()
	_test_epoch_shape()
	_test_time_advance_periodic()
	_test_eccentric_geometry()
	_test_system_position_depth_2()
	if _failed:
		quit(1)
		return
	print("ORBIT_MATH_OK")
	quit(0)


func _make_orbit(a: float, e: float, i_deg: float, lan_deg: float, argp_deg: float,
		mean_deg: float, epoch_s: float = 0.0) -> Resource:
	var orbit: Resource = ORBIT_SCRIPT.new()
	orbit.set(&"semi_major_axis_m", a)
	orbit.set(&"eccentricity", e)
	orbit.set(&"inclination_deg", i_deg)
	orbit.set(&"longitude_ascending_node_deg", lan_deg)
	orbit.set(&"argument_periapsis_deg", argp_deg)
	orbit.set(&"mean_anomaly_at_epoch_deg", mean_deg)
	orbit.set(&"epoch_s", epoch_s)
	return orbit


func _test_kepler_third_law() -> void:
	for a: float in [0.387 * AU, 1.0 * AU, 5.2 * AU]:
		var expected: float = TAU * sqrt((a * a * a) / SOL_MU)
		var got: float = ORBIT_MATH.period_s(a, SOL_MU)
		_assert(_rel(got, expected) < 1e-9, "period_s wrong for a=%s (got %s want %s)" % [a, got, expected])
	_assert(ORBIT_MATH.period_s(AU, 0.0) == 0.0, "period_s must be 0 with mu=0 (frozen orbit)")
	_assert(ORBIT_MATH.mean_motion(0.0, SOL_MU) == 0.0, "mean_motion must be 0 with a=0")


func _test_epoch_shape() -> void:
	# Circular, in-plane, zero anomaly -> periapsis on +X at distance a.
	var o: Resource = _make_orbit(AU, 0.0, 0.0, 0.0, 0.0, 0.0)
	var p: Vec3D = ORBIT_MATH.orbit_offset(o, 1.0, 1.0, 0.0, 0.0)
	_assert(_rel(p.x, AU) < 1e-9 and absf(p.y) < 1.0 and absf(p.z) < 1.0,
		"circular M=0 should sit on +X at a, got (%s, %s, %s)" % [p.x, p.y, p.z])

	# Quarter turn on a circle -> +Z at distance a, still in plane.
	var q: Vec3D = ORBIT_MATH.orbit_offset(_make_orbit(AU, 0.0, 0.0, 0.0, 0.0, 90.0), 1.0, 1.0, 0.0, 0.0)
	_assert(absf(q.x) < 1.0 and absf(q.y) < 1.0 and _rel(q.z, AU) < 1e-9,
		"circular M=90 should sit on +Z at a, got (%s, %s, %s)" % [q.x, q.y, q.z])

	# Inclination lifts the quarter-turn point out of the XZ plane.
	var r: Vec3D = ORBIT_MATH.orbit_offset(_make_orbit(AU, 0.0, 30.0, 0.0, 0.0, 90.0), 1.0, 1.0, 0.0, 0.0)
	_assert(r.y > AU * 0.1, "inclination 30 deg should lift M=90 point above the plane, y=%s" % r.y)


func _test_time_advance_periodic() -> void:
	var o: Resource = _make_orbit(AU, 0.0167, 12.0, 40.0, 25.0, 10.0)
	var period: float = ORBIT_MATH.period_s(AU, SOL_MU)
	var at_epoch: Vec3D = ORBIT_MATH.orbit_offset(o, 1.0, 1.0, SOL_MU, 0.0)
	var after_one: Vec3D = ORBIT_MATH.orbit_offset(o, 1.0, 1.0, SOL_MU, period)
	_assert(at_epoch.distance_to(after_one) / AU < 1e-6,
		"one full period must return to the epoch position (drift %s m)" % at_epoch.distance_to(after_one))

	# mu = 0 must behave exactly like the pre-time-term solver: t is ignored.
	var frozen_a: Vec3D = ORBIT_MATH.orbit_offset(o, 1.0, 1.0, 0.0, 0.0)
	var frozen_b: Vec3D = ORBIT_MATH.orbit_offset(o, 1.0, 1.0, 0.0, period * 0.37)
	_assert(frozen_a.distance_to(frozen_b) < 1e-6, "mu=0 orbit_offset must ignore t")

	# epoch_s offset: evaluating at t == epoch_s reproduces the M_epoch position.
	var shifted: Resource = _make_orbit(AU, 0.2, 0.0, 0.0, 0.0, 33.0, 5000.0)
	var base: Vec3D = ORBIT_MATH.orbit_offset(_make_orbit(AU, 0.2, 0.0, 0.0, 0.0, 33.0, 0.0), 1.0, 1.0, SOL_MU, 0.0)
	var same: Vec3D = ORBIT_MATH.orbit_offset(shifted, 1.0, 1.0, SOL_MU, 5000.0)
	_assert(base.distance_to(same) / AU < 1e-9, "orbit_offset at t == epoch_s must equal the M_epoch position")


func _test_eccentric_geometry() -> void:
	# Perihelion (M=0) and aphelion (M=180) distances for an eccentric orbit.
	for e: float in [0.0, 0.1, 0.4, 0.9]:
		var peri: Vec3D = ORBIT_MATH.orbit_offset(_make_orbit(AU, e, 0.0, 0.0, 0.0, 0.0), 1.0, 1.0, 0.0, 0.0)
		var apo: Vec3D = ORBIT_MATH.orbit_offset(_make_orbit(AU, e, 0.0, 0.0, 0.0, 180.0), 1.0, 1.0, 0.0, 0.0)
		_assert(_rel(peri.length(), AU * (1.0 - e)) < 1e-6,
			"perihelion distance wrong for e=%s (got %s)" % [e, peri.length()])
		_assert(_rel(apo.length(), AU * (1.0 + e)) < 1e-6,
			"aphelion distance wrong for e=%s (got %s)" % [e, apo.length()])
	# Newton solve stays finite / converged at high eccentricity.
	var extreme: Vec3D = ORBIT_MATH.orbit_offset(_make_orbit(AU, 0.95, 0.0, 0.0, 0.0, 47.0), 1.0, 1.0, 0.0, 0.0)
	_assert(is_finite(extreme.x) and is_finite(extreme.y) and is_finite(extreme.z),
		"orbit_offset produced non-finite output at e=0.95")


func _test_system_position_depth_2() -> void:
	var star: Resource = _mk_body("star", "", BODY_SCRIPT.BodyType.STAR, 6.9e8, SOL_MU)
	var planet: Resource = _mk_body("planet", "star", BODY_SCRIPT.BodyType.PLANET, 6.4e6, 3.986e14)
	planet.set(&"orbit", _make_orbit(AU, 0.0167, 5.0, 10.0, 20.0, 30.0))
	var moon: Resource = _mk_body("moon", "planet", BODY_SCRIPT.BodyType.MOON, 1.7e6, 0.0)
	moon.set(&"orbit", _make_orbit(3.84e8, 0.05, 5.1, 0.0, 0.0, 60.0))

	var system: Resource = SYSTEM_SCRIPT.new()
	var arr: Array[Resource] = [star, planet, moon]
	system.set(&"bodies", arr)
	system.set(&"active_body_id", "planet")

	var t: float = 987654.0
	var star_pos: Vec3D = ORBIT_MATH.system_position(system, "star", t)
	_assert(star_pos.length() < 1.0, "root star must be at the system origin, got %s" % star_pos.length())

	var planet_pos: Vec3D = ORBIT_MATH.system_position(system, "planet", t)
	var planet_direct: Vec3D = ORBIT_MATH.orbit_offset(planet.get(&"orbit"),
		star.get(&"radius_m"), planet.get(&"radius_m"), SOL_MU, t)
	_assert(planet_pos.distance_to(planet_direct) / AU < 1e-9, "planet system_position != direct orbit_offset")

	var moon_pos: Vec3D = ORBIT_MATH.system_position(system, "moon", t)
	var moon_expected: Vec3D = planet_pos.add(ORBIT_MATH.orbit_offset(moon.get(&"orbit"),
		planet.get(&"radius_m"), moon.get(&"radius_m"), OrbitMath.body_mu(planet), t))
	_assert(moon_pos.distance_to(moon_expected) < 1.0,
		"moon system_position != planet_pos + moon orbit_offset (drift %s m)" % moon_pos.distance_to(moon_expected))
	_assert(moon_pos.distance_to(planet_pos) < 4.1e8,
		"moon must stay within its ~3.84e8 m (x1.05 aphelion) orbit of the planet")


func _mk_body(id: String, parent: String, type: int, radius_m: float, gm: float) -> Resource:
	var b: Resource = BODY_SCRIPT.new()
	b.set(&"body_id", id)
	b.set(&"display_name", id.capitalize())
	b.set(&"body_type", type)
	b.set(&"parent_body_id", parent)
	b.set(&"radius_m", radius_m)
	b.set(&"gravitational_parameter_m3_s2", gm)
	b.call("ensure_children")
	return b


func _rel(got: float, want: float) -> float:
	return absf(got - want) / maxf(absf(want), 1e-9)


func _assert(condition: bool, message: String) -> void:
	if condition:
		return
	_failed = true
	push_error("ORBIT_MATH_FAILED: %s" % message)
