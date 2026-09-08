extends Node
## GG0: GasGiantModel gives a coherent vertical envelope and the generator +
## Bodies pool + Frames route the gas giant's SOLID CORE (rho ~= 1000 kg/m^3) as
## the bake / collision / altitude datum while `radius_m` stays the cloud tops.
##   godot --headless --path . res://tests/validate_gas_giant_model.tscn

const GENERATOR := preload("res://scripts/world_authoring/celestial_system_generator.gd")
const GAS_GIANT_MODEL := preload("res://scripts/gen/gas_giant_model.gd")
const GEN_CONFIG := preload("res://scripts/gen/gen_config.gd")

const SEED := 8571

var _failed := false
var _frames := 0


func _process(_dt: float) -> void:
	_frames += 1
	if _frames > 1800:
		push_error("GAS_GIANT_MODEL_FAILED: timed out")
		get_tree().quit(1)


func _ready() -> void:
	await get_tree().process_frame

	# --- Pure model ---------------------------------------------------------
	var m: GasGiantModel = GAS_GIANT_MODEL.from_body(68_000_000.0, 8.6e15, 12345)
	_assert(m.core_radius_m < m.deadly_radius_m
			and m.deadly_radius_m < m.one_bar_radius_m
			and m.one_bar_radius_m < m.cloud_top_radius_m,
		"envelope radii strictly increase: core < deadly < 1bar < cloud_top (%s)" % m.describe())
	_assert(m.core_radius_m > m.cloud_top_radius_m * 0.03
			and m.core_radius_m < m.cloud_top_radius_m * 0.30,
		"the core sits in a sane band below the cloud tops (%.3f R)"
			% (m.core_radius_m / m.cloud_top_radius_m))
	_assert(is_equal_approx(m.density_at(m.core_radius_m), GAS_GIANT_MODEL.WATER_DENSITY)
			or absf(m.density_at(m.core_radius_m) - GAS_GIANT_MODEL.WATER_DENSITY) < 1.0,
		"density at the core surface is liquid-water density (%.2f kg/m^3)"
			% m.density_at(m.core_radius_m))

	# Density rises monotonically from the cloud tops inward to the core.
	var prev_rho: float = -1.0
	var steps := 40
	for i in steps + 1:
		var r: float = lerpf(m.cloud_top_radius_m, m.core_radius_m, float(i) / float(steps))
		var rho: float = m.density_at(r)
		_assert(rho >= prev_rho - 1.0e-6, "density is monotonic inward (step %d: %.4f vs %.4f)" % [i, rho, prev_rho])
		prev_rho = rho
	_assert(m.density_at(m.cloud_top_radius_m) < 1.0, "the cloud tops are near-vacuum thin (%.3f kg/m^3)" % m.density_at(m.cloud_top_radius_m))

	# Pressure is finite + increasing inward; temperature warms inward.
	_assert(m.pressure_at(m.core_radius_m) > m.pressure_at(m.cloud_top_radius_m),
		"pressure rises toward the core")
	_assert(m.temperature_at(m.core_radius_m) > m.temperature_at(m.cloud_top_radius_m),
		"temperature warms toward the core")
	_assert(m.shell_thickness_m() > 1.0 and is_finite(m.scale_height_at(m.core_radius_m)),
		"shell thickness + scale height are finite")

	# --- Generator wiring ------------------------------------------------
	var system: CelestialSystemDefinition = GENERATOR.generate(SEED)
	var colossus: Resource = system.find_body("colossus")
	_assert(colossus != null and String(colossus.get(&"archetype")) == "gas_giant",
		"the spine gas giant 'colossus' exists")
	var core_r: float = float(colossus.get(&"core_radius_m"))
	var cloud_top: float = float(colossus.get(&"radius_m"))
	_assert(core_r > 0.0 and core_r < cloud_top,
		"colossus carries a solid core radius below its cloud tops (core %.0f km < %.0f km)"
			% [core_r / 1000.0, cloud_top / 1000.0])
	_assert(core_r > cloud_top * 0.03 and core_r < cloud_top * 0.30,
		"colossus core radius is in a sane band (%.3f R)" % (core_r / cloud_top))
	_assert(float(colossus.get(&"one_bar_radius_m")) > core_r
			and float(colossus.get(&"one_bar_radius_m")) < cloud_top,
		"colossus 1-bar radius sits inside the shell")
	_assert(bool(colossus.call("is_gas_giant"))
			and is_equal_approx(float(colossus.call("surface_reference_radius_m")), core_r),
		"CelestialBodyDefinition.surface_reference_radius_m() returns the core for a gas giant")

	# The BAKE config for the gas giant is at the CORE radius, not the cloud tops.
	var baseline := GEN_CONFIG.new()
	baseline.system_seed = SEED
	var gg_cfg: Resource = GENERATOR.gen_config_for(system, "colossus", baseline)
	_assert(is_equal_approx(float(gg_cfg.get(&"planet_radius")), core_r),
		"gen_config_for('colossus').planet_radius == the core radius (%.0f km)" % (core_r / 1000.0))
	# A non-gas body still bakes at its own radius.
	var rime: Resource = system.find_body("rime")
	var rime_cfg: Resource = GENERATOR.gen_config_for(system, "rime", baseline)
	_assert(is_equal_approx(float(rime_cfg.get(&"planet_radius")), float(rime.get(&"radius_m"))),
		"a non-gas body's bake radius is unchanged (rime)")
	_assert(is_equal_approx(float(rime.call("surface_reference_radius_m")), float(rime.get(&"radius_m"))),
		"surface_reference_radius_m() == radius_m for a non-gas body")

	# --- Bodies pool + Frames route the core as the resident datum -------
	# Boot the home planet, register the system (mirrors main._activate_system).
	baseline.face_res = 16
	baseline.erosion_iterations = 6
	var home_cfg: Resource = GENERATOR.gen_config_for(system, GENERATOR.HOME_BODY_ID, baseline)
	Planet.configure(home_cfg)
	Planet.adopt(PlanetBake.new(home_cfg).bake(Callable(), true))
	Bodies.primary()
	GENERATOR.populate_pool(system, baseline, Bodies, Frames, 0.0)

	var slot: BodyRuntime = Bodies.slot(&"colossus")
	_assert(slot != null, "colossus has a pool slot")
	_assert(is_equal_approx(slot.radius_m, cloud_top),
		"the pool slot keeps radius_m == the cloud tops (reference)")
	_assert(is_equal_approx(slot.surface_radius(), core_r),
		"the pool slot's surface datum == the core radius")
	_assert(is_equal_approx(Frames.body_radius(&"colossus"), cloud_top)
			and is_equal_approx(Frames.body_surface_radius(&"colossus"), core_r),
		"Frames keeps the reference radius AND the core surface datum for colossus")

	# Make colossus the resident body: Frames.planet_radius must follow the CORE,
	# so world_altitude over the gas giant is measured from its solid surface.
	Bodies.load_active(&"colossus", Vec3D.new(0.0, core_r * 1.5, 0.0))
	_assert(String(Bodies.active.id) == "colossus", "colossus is the resident body")
	_assert(is_equal_approx(Frames.planet_radius, core_r),
		"Frames.planet_radius follows the gas giant's CORE while resident (%.0f km)"
			% (Frames.planet_radius / 1000.0))
	_assert(is_equal_approx(float(Planet.cfg.planet_radius), core_r),
		"the resident Planet config is the core radius")
	var surface_pt := Vec3D.new(core_r, 0.0, 0.0)
	_assert(absf(Frames.world_altitude(surface_pt)) < 1.0,
		"Frames.world_altitude at the core surface reads ~0 (%.1f m)" % Frames.world_altitude(surface_pt))
	var cloud_pt := Vec3D.new(cloud_top, 0.0, 0.0)
	_assert(Frames.world_altitude(cloud_pt) > (cloud_top - core_r) * 0.99,
		"altitude at the cloud tops is the full shell thickness above the core")

	if _failed:
		get_tree().quit(1)
		return
	print("GAS_GIANT_MODEL_OK  (colossus core %.0f km of %.0f km, shell %.0f km)"
		% [core_r / 1000.0, cloud_top / 1000.0, (cloud_top - core_r) / 1000.0])
	get_tree().quit(0)


func _assert(condition: bool, message: String) -> void:
	if condition:
		return
	_failed = true
	push_error("GAS_GIANT_MODEL_FAILED: %s" % message)
