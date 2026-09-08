extends Node
## GG4: AtmosphereMedium gives a gas giant real drag (a fall reaches terminal
## velocity), buoyancy (a light object floats inside the envelope, above the
## crush boundary), and a lethal floor -- and is a no-op on every other body.
##   godot --headless --path . res://tests/validate_atmosphere_medium.tscn

const GENERATOR := preload("res://scripts/world_authoring/celestial_system_generator.gd")
const GEN_CONFIG := preload("res://scripts/gen/gen_config.gd")
const ATMOSPHERE_MEDIUM := preload("res://scripts/physics/atmosphere_medium.gd")

const SEED := 8571

var _failed := false
var _frames := 0


func _process(_dt: float) -> void:
	_frames += 1
	if _frames > 2400:
		push_error("ATMOSPHERE_MEDIUM_FAILED: timed out")
		get_tree().quit(1)


func _ready() -> void:
	await get_tree().process_frame

	var system: CelestialSystemDefinition = GENERATOR.generate(SEED, 21.4, 1.0, true)
	var baseline := GEN_CONFIG.new()
	baseline.system_seed = SEED
	baseline.face_res = 16
	baseline.erosion_iterations = 6
	var home_cfg: Resource = GENERATOR.gen_config_for(system, GENERATOR.HOME_BODY_ID, baseline)
	Planet.configure(home_cfg)
	Planet.adopt(PlanetBake.new(home_cfg).bake(Callable(), true))
	Bodies.primary()
	GENERATOR.populate_pool(system, baseline, Bodies, Frames, 0.0)

	var colossus: Resource = system.find_body("colossus")
	var cloud_top: float = float(colossus.get(&"radius_m"))
	var core: float = float(colossus.get(&"core_radius_m"))

	# --- Vacuum everywhere by default -------------------------------------
	var m := ATMOSPHERE_MEDIUM.new()
	m.refresh(Bodies, system)   # contact body is still the home planet
	_assert(not m.active(), "no atmosphere medium on the home planet")
	_assert(is_equal_approx(m.speed_factor(-5000.0), 1.0)
			and is_equal_approx(m.drag_decel(100.0, -5000.0), 0.0)
			and is_equal_approx(m.buoyancy_accel(-5000.0, 0.05), 0.0),
		"vacuum medium is a no-op (speed x1, no drag, no buoyancy)")

	# A non-gas body is also vacuum.
	Bodies.load_active(&"rime", Vec3D.new(0.0, float(system.find_body("rime").get(&"radius_m")) * 1.5, 0.0))
	m.refresh(Bodies, system)
	_assert(not m.active(), "no atmosphere medium on the ice world 'rime'")

	# --- Inside the gas giant -------------------------------------------
	Bodies.load_active(&"colossus", Vec3D.new(0.0, core * 1.4, 0.0))
	m.refresh(Bodies, system)
	_assert(m.active(), "the gas giant has an atmosphere medium")
	_assert(is_equal_approx(m.reference_radius_m(), cloud_top),
		"the medium's altitude datum is the cloud tops")

	# Density rises as you descend; drag with it.
	var rho_top := m.density_at(0.0)
	var rho_mid := m.density_at(-(cloud_top - core) * 0.5)
	var rho_deep := m.density_at(-(cloud_top - core) * 0.9)
	_assert(rho_top < rho_mid and rho_mid < rho_deep,
		"gas density rises inward (%.3f -> %.1f -> %.1f kg/m^3)" % [rho_top, rho_mid, rho_deep])
	_assert(m.drag_decel(50.0, -(cloud_top - core) * 0.6) > m.drag_decel(50.0, -1000.0),
		"drag is stronger deeper in the envelope")
	_assert(m.drag_decel(80.0, -5.0e6) > 3.0 * m.drag_decel(40.0, -5.0e6),
		"drag is quadratic in speed")
	_assert(is_equal_approx(m.speed_factor(1.0e6), 1.0) and m.speed_factor(-6.0e6) < 0.5,
		"the FLY speed factor is 1 above the tops and throttles hard in the murk")

	# --- Terminal velocity: integrate a free fall --------------------
	var g: float = float(Bodies.slot(&"colossus").surface_gravity())
	var vs: float = 0.0
	var alt: float = -2000.0                     ## just below the cloud tops
	var dt: float = 1.0 / 30.0
	var vmax: float = 0.0
	for step in 6000:
		vs -= g * dt
		vs += m.buoyancy_accel(alt, ATMOSPHERE_MEDIUM.PERSON_BUOYANCY) * dt
		vs -= signf(vs) * m.drag_decel(absf(vs), alt) * dt
		if m.lethal(alt):
			vs = maxf(vs, 14.0)
		alt += vs * dt
		if step > 1500:
			vmax = maxf(vmax, absf(vs))
	_assert(is_finite(vs) and vmax > 1.0 and vmax < 400.0,
		"a free fall settles to a finite terminal velocity (~%.1f m/s), not runaway" % vmax)
	_assert(alt > m.lethal_altitude() - 5.0e6,
		"the fall is arrested near the crush boundary, not through the core")

	# --- Buoyant float: a balloon holds an altitude inside the envelope --
	var neutral := m.neutral_altitude(ATMOSPHERE_MEDIUM.BALLOON_BUOYANCY)
	_assert(neutral < 0.0 and neutral > m.lethal_altitude(),
		"a balloon floats inside the envelope, above the crush boundary (alt %.0f km)" % (neutral / 1000.0))
	_assert(absf(m.buoyancy_accel(neutral, ATMOSPHERE_MEDIUM.BALLOON_BUOYANCY) - g) < g * 0.15,
		"at the float altitude buoyancy ~= gravity")
	# A bare person is much less buoyant -> floats far deeper (or not at all).
	_assert(m.neutral_altitude(ATMOSPHERE_MEDIUM.PERSON_BUOYANCY) < neutral,
		"a bare person's float altitude is deeper than a balloon's")

	# --- Lethal boundary ------------------------------------------------
	_assert(not m.lethal(0.0) and not m.lethal(-1000.0),
		"the upper envelope is survivable")
	_assert(m.lethal(m.lethal_altitude() - 1000.0),
		"below the deadly radius is lethal")

	# --- GG5: the HUD / status readout --------------------------------
	var crush_km := m.lethal_altitude() / 1000.0
	var say_above := m.readout(Vec3D.new(0.0, cloud_top + 4.0e6, 0.0))
	var say_inside := m.readout(Vec3D.new(0.0, (core + cloud_top) * 0.5, 0.0))
	_assert(say_above.contains("cloud tops") and not say_above.contains("above core"),
		"readout above the tops is a plain altitude tagged '(cloud tops)': %s" % say_above)
	_assert(say_inside.contains("above core") and say_inside.contains("bar")
			and say_inside.contains("kg/m"),
		"readout inside the envelope adds depth / pressure / density: %s" % say_inside)
	# Vacuum (home planet) -> empty string, HUD falls back to its normal line.
	Bodies.load_active(&"rime", Vec3D.new(0.0, float(system.find_body("rime").get(&"radius_m")) * 1.5, 0.0))
	m.refresh(Bodies, system)
	_assert(m.readout(Vec3D.new(0.0, 1.0e7, 0.0)) == "",
		"the readout is empty on a non-gas body (HUD keeps its normal altitude line)")

	if _failed:
		get_tree().quit(1)
		return
	print("ATMOSPHERE_MEDIUM_OK  (terminal ~%.0f m/s, balloon floats at %.0f km, crush at %.0f km)"
		% [vmax, neutral / 1000.0, crush_km])
	get_tree().quit(0)


func _assert(condition: bool, message: String) -> void:
	if condition:
		return
	_failed = true
	push_error("ATMOSPHERE_MEDIUM_FAILED: %s" % message)
