extends Node
## GG1: the gas giant's SOLID CORE is the walkable / collidable / gravitating
## surface. The pool warms + bakes it at core_radius_m; while it is resident the
## radius datum, Planet config and gravity all key off the core, while radius_m
## (cloud tops) is preserved for far-LOD / orbits.
##   godot --headless --path . res://tests/validate_gas_giant_core.tscn

const GENERATOR := preload("res://scripts/world_authoring/celestial_system_generator.gd")
const GEN_CONFIG := preload("res://scripts/gen/gen_config.gd")

const SEED := 8571

var _failed := false
var _frames := 0


func _process(_dt: float) -> void:
	_frames += 1
	if _frames > 3000:
		push_error("GAS_GIANT_CORE_FAILED: timed out")
		get_tree().quit(1)


func _ready() -> void:
	await get_tree().process_frame

	var system: CelestialSystemDefinition = GENERATOR.generate(SEED)
	var colossus: Resource = system.find_body("colossus")
	var core_r: float = float(colossus.get(&"core_radius_m"))
	var cloud_top: float = float(colossus.get(&"radius_m"))
	var gm: float = float(colossus.get(&"gravitational_parameter_m3_s2"))
	_assert(core_r > 0.0 and core_r < cloud_top, "colossus has a solid core inside the cloud tops")

	# Gravity at the core surface: only the enclosed mass pulls, so g falls
	# ~linearly toward the centre -- g_core ~= g_cloud_top * core/cloud_top.
	var g_cloud: float = gm / (cloud_top * cloud_top)
	var g_core: float = g_cloud * (core_r / cloud_top)
	_assert(is_equal_approx(float(colossus.get(&"surface_gravity_m_s2")), g_core),
		"CelestialBody.surface_gravity_m_s2 is the linear-interior core value (%.2f m/s^2, cloud tops %.2f)"
			% [g_core, g_cloud])
	_assert(g_core > 1.0 and g_core < 40.0,
		"the core is a walkable gravity (%.1f m/s^2), not a crushing GM/core^2" % g_core)

	# --- Boot home, register the system (mirrors main._activate_system) ----
	var baseline := GEN_CONFIG.new()
	baseline.system_seed = SEED
	baseline.face_res = 16
	baseline.erosion_iterations = 6
	var home_cfg: Resource = GENERATOR.gen_config_for(system, GENERATOR.HOME_BODY_ID, baseline)
	Planet.configure(home_cfg)
	Planet.adopt(PlanetBake.new(home_cfg).bake(Callable(), true))
	Bodies.primary()
	GENERATOR.populate_pool(system, baseline, Bodies, Frames, 0.0)

	var slot: BodyRuntime = Bodies.slot(&"colossus")
	_assert(slot != null, "colossus has a pool slot")
	_assert(is_equal_approx(slot.surface_gravity(), g_core),
		"the pool slot reports core-surface gravity (%.2f m/s^2)" % slot.surface_gravity())

	# --- Warm: bake the CORE ---------------------------------------------
	_assert(slot.warm(), "the gas giant core warmed (baked or cache-loaded)")
	_assert(slot.fields != null and slot.state == BodyRuntime.State.WARM, "the core bake is resident")
	_assert(is_equal_approx(float(slot.gen_config.get(&"planet_radius")), core_r),
		"the bake config radius is the core (%.0f km)" % (core_r / 1000.0))
	_assert(is_equal_approx(slot.radius_m, cloud_top),
		"the slot keeps radius_m == the cloud tops for far-LOD / orbits")
	_assert(is_equal_approx(slot.surface_radius(), core_r), "the slot surface datum is the core")

	# The baked core is ROCK, not a 97 %-ocean world: most cells are land.
	var fields: Object = PlanetBake.new(slot.gen_config).bake(Callable(), true)
	_assert(fields != null, "the core PlanetFields loaded")
	var grid: Object = fields.get("grid")
	var elev: PackedFloat32Array = fields.get("elev")
	var land := 0
	var total: int = int(grid.get("cell_count"))
	for c in total:
		if elev[c] > 0.0:
			land += 1
	var land_frac := float(land) / float(maxi(total, 1))
	_assert(land_frac > 0.55,
		"the gas giant core bakes as mostly land / rock (land fraction %.2f)" % land_frac)

	# --- Activate: the core becomes the resident surface ----------------
	Bodies.load_active(&"colossus", Vec3D.new(0.0, core_r + 50_000.0, 0.0))
	_assert(String(Bodies.active.id) == "colossus", "colossus is the resident body")
	_assert(Planet.ready_state, "the resident Planet is ready")
	_assert(is_equal_approx(float(Planet.cfg.planet_radius), core_r),
		"the resident Planet config radius is the core")
	_assert(is_equal_approx(Frames.planet_radius, core_r),
		"Frames.planet_radius (altitude / collision datum) is the core")
	_assert(is_equal_approx(Frames.body_radius(&"colossus"), cloud_top),
		"Frames still knows the cloud-top reference radius (far-LOD / approach)")
	_assert(is_equal_approx(Bodies.contact_gravity(), g_core),
		"contact gravity on the resident gas giant is core-surface gravity")

	# A spawn direction exists and would place the player on the core surface.
	var spawn_dir: Vector3 = grid.call("cell_dir", 0)
	_assert(spawn_dir.is_normalized() or spawn_dir.length() > 0.9,
		"a spawn direction is available on the core")
	var spawn_r: float = float(Planet.cfg.planet_radius) + maxf(elev[0], 0.0) + 60.0
	_assert(spawn_r > core_r and spawn_r < core_r + 20_000.0,
		"a spawn would land just above the core surface (r = %.0f km)" % (spawn_r / 1000.0))

	# far-LOD / orbit radius for colossus is untouched (still the cloud tops).
	_assert(is_equal_approx(float(system.find_body("colossus").get(&"radius_m")), cloud_top)
			and cloud_top > core_r,
		"CelestialBody.radius_m stays the cloud tops for the far-LOD sphere")

	if _failed:
		get_tree().quit(1)
		return
	print("GAS_GIANT_CORE_OK  (core %.0f km, g_core %.1f m/s^2, land %.0f%%)"
		% [core_r / 1000.0, g_core, land_frac * 100.0])
	get_tree().quit(0)


func _assert(condition: bool, message: String) -> void:
	if condition:
		return
	_failed = true
	push_error("GAS_GIANT_CORE_FAILED: %s" % message)
