extends Node
## M4 verification: CelestialSystemGenerator produces a deterministic, valid
## multi-planet system and per-body bake configs that are distinct and
## cache-stable.
##   godot --headless --path . res://tests/validate_system_generator.tscn

const GENERATOR := preload("res://scripts/world_authoring/celestial_system_generator.gd")
const GEN_CONFIG := preload("res://scripts/gen/gen_config.gd")
const BODY_SCRIPT := preload("res://scripts/world_authoring/model/celestial_body_definition.gd")
const ORBIT_MATH := preload("res://scripts/world_authoring/model/orbit_math.gd")

var _failed := false


func _ready() -> void:
	await get_tree().process_frame

	var sys_a: CelestialSystemDefinition = GENERATOR.generate(12345)
	var sys_b: CelestialSystemDefinition = GENERATOR.generate(12345)

	# --- Determinism -----------------------------------------------------
	_assert(sys_a.bodies.size() == sys_b.bodies.size(),
		"same seed -> same body count (%d vs %d)" % [sys_a.bodies.size(), sys_b.bodies.size()])
	var mismatched := 0
	for i in sys_a.bodies.size():
		var ba: Resource = sys_a.bodies[i]
		var bb: Resource = sys_b.bodies[i]
		if String(ba.get(&"body_id")) != String(bb.get(&"body_id")):
			mismatched += 1
			continue
		if not is_equal_approx(float(ba.get(&"radius_m")), float(bb.get(&"radius_m"))):
			mismatched += 1
		var oa: Resource = ba.get(&"orbit")
		var ob: Resource = bb.get(&"orbit")
		if oa != null and ob != null:
			if not is_equal_approx(float(oa.get(&"semi_major_axis_m")), float(ob.get(&"semi_major_axis_m"))) \
					or not is_equal_approx(float(oa.get(&"mean_anomaly_at_epoch_deg")), float(ob.get(&"mean_anomaly_at_epoch_deg"))):
				mismatched += 1
	_assert(mismatched == 0, "two generations of seed 12345 are byte-identical (%d mismatches)" % mismatched)

	var sys_other: CelestialSystemDefinition = GENERATOR.generate(999)
	var ember_a: float = float((sys_a.find_body("ember").get(&"orbit") as Resource).get(&"mean_anomaly_at_epoch_deg"))
	var ember_o: float = float((sys_other.find_body("ember").get(&"orbit") as Resource).get(&"mean_anomaly_at_epoch_deg"))
	_assert(sys_other.bodies.size() != sys_a.bodies.size()
			or not is_equal_approx(ember_a, ember_o),
		"a different seed varies the system (orbit phases / body count)")

	# --- Structure -----------------------------------------------------
	var root_stars := 0
	var companion_stars := 0
	var planets := 0
	var archetypes := {}
	var root_star_id := ""
	for body_value: Variant in sys_a.bodies:
		var body: Resource = body_value as Resource
		var bt: int = int(body.get(&"body_type"))
		if bt == BODY_SCRIPT.BodyType.STAR:
			if String(body.get(&"parent_body_id")).is_empty():
				root_stars += 1
				root_star_id = String(body.get(&"body_id"))
			else:
				companion_stars += 1
		elif bt == BODY_SCRIPT.BodyType.PLANET:
			planets += 1
			archetypes[String(body.get(&"archetype"))] = true
	_assert(root_stars == 1, "exactly one root star (got %d)" % root_stars)
	_assert(root_star_id == "helion", "root star id is 'helion'")
	_assert(planets >= 5, "the archetype spine gives at least five planets (got %d)" % planets)
	_assert(companion_stars >= 1, "the system has a distant stellar companion (the pulsar)")

	var pulsar: Resource = sys_a.find_body("lighthouse")
	_assert(pulsar != null and int(pulsar.get(&"body_type")) == BODY_SCRIPT.BodyType.STAR
			and String(pulsar.get(&"archetype")) == "pulsar"
			and String(pulsar.get(&"parent_body_id")) == "helion",
		"the pulsar 'lighthouse' is a STAR archetype 'pulsar' orbiting Helion")

	for want: String in ["hot_rock", "terran", "ice", "gas_giant"]:
		_assert(archetypes.has(want), "the spine includes a %s planet" % want)

	var home: Resource = sys_a.find_body("asterra")
	_assert(home != null and int(home.get(&"body_type")) == BODY_SCRIPT.BodyType.PLANET,
		"the home planet 'asterra' exists and is a PLANET")
	_assert(String(home.get(&"archetype")) == "terran", "the home planet is terran")
	_assert(String(home.get(&"parent_body_id")) == "helion", "the home planet orbits the star")

	# Inner rocks really are closer to Helion than the outer ice/giant.
	var cinder: Resource = sys_a.find_body("cinder")
	var colossus: Resource = sys_a.find_body("colossus")
	_assert(cinder != null and colossus != null
			and float((cinder.get(&"orbit") as Resource).get(&"semi_major_axis_m"))
				< float((colossus.get(&"orbit") as Resource).get(&"semi_major_axis_m")),
		"the hot rock 'cinder' orbits inside the gas giant 'colossus'")
	_assert(float(colossus.get(&"radius_m")) > 2.0e7, "the gas giant is a giant (%.2e m radius)" % float(colossus.get(&"radius_m")))

	# Every non-star body resolves its parent, and no orbit is degenerate.
	for body_value: Variant in sys_a.bodies:
		var body: Resource = body_value as Resource
		if int(body.get(&"body_type")) == BODY_SCRIPT.BodyType.STAR:
			continue
		var parent_id: String = String(body.get(&"parent_body_id"))
		_assert(not parent_id.is_empty() and sys_a.find_body(parent_id) != null,
			"body %s resolves its parent %s" % [body.get(&"body_id"), parent_id])
		var orbit: Resource = body.get(&"orbit")
		_assert(orbit != null and float(orbit.get(&"semi_major_axis_m")) > 0.0,
			"body %s has a non-degenerate orbit" % body.get(&"body_id"))

	# --- system_position is finite for every body -------------------
	for body_value: Variant in sys_a.bodies:
		var body: Resource = body_value as Resource
		var pos: Vec3D = ORBIT_MATH.system_position(sys_a, String(body.get(&"body_id")), 0.0)
		_assert(is_finite(pos.x) and is_finite(pos.y) and is_finite(pos.z),
			"system_position(%s) is finite" % body.get(&"body_id"))
	var star_pos: Vec3D = ORBIT_MATH.system_position(sys_a, "helion", 0.0)
	_assert(star_pos.length() < 1.0, "the root star sits at the system origin")
	var home_pos: Vec3D = ORBIT_MATH.system_position(sys_a, "asterra", 0.0)
	_assert(home_pos.length() > 1.4e11 and home_pos.length() < 1.6e11,
		"the home planet is ~1 AU from the star (%s m)" % String.num_scientific(home_pos.length()))

	# --- Per-body bake configs -------------------------------------
	var baseline := GEN_CONFIG.new()
	baseline.system_seed = 12345
	baseline.face_res = 48

	var home_cfg: Resource = GENERATOR.gen_config_for(sys_a, "asterra", baseline)
	_assert(int(home_cfg.get(&"world_seed")) == int(baseline.world_seed),
		"the home planet reuses the baseline world_seed (existing bake)")
	_assert(int(home_cfg.get(&"system_seed")) == 0, "a per-body config never re-triggers generation")

	var paths := {}
	for body_value: Variant in sys_a.bodies:
		var body: Resource = body_value as Resource
		if int(body.get(&"body_type")) == BODY_SCRIPT.BodyType.STAR:
			continue
		var body_id: String = String(body.get(&"body_id"))
		var cfg: Resource = GENERATOR.gen_config_for(sys_a, body_id, baseline)
		_assert(int(cfg.get(&"system_seed")) == 0, "%s config has system_seed 0" % body_id)
		# A gas giant bakes its solid core, not the cloud tops; every other body
		# bakes at its own radius.
		var want_radius: float = float(body.call("surface_reference_radius_m"))
		_assert(is_equal_approx(float(cfg.get(&"planet_radius")), want_radius),
			"%s config radius matches its surface reference radius" % body_id)
		var path: String = PlanetBake.new(cfg).cache_path()
		_assert(not paths.has(path), "bake cache_path for %s is unique (%s)" % [body_id, path])
		paths[path] = body_id

	if _failed:
		get_tree().quit(1)
		return
	print("SYSTEM_GENERATOR_OK")
	get_tree().quit(0)


func _assert(condition: bool, message: String) -> void:
	if condition:
		return
	_failed = true
	push_error("SYSTEM_GENERATOR_FAILED: %s" % message)
