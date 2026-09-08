extends Node
## M4 integration: a generated system boots the game's resident-body path (home
## planet bakes + adopts) and every other solid body is registered with the
## Bodies pool so seamless travel (M3) can reach it.
##   godot --headless --path . res://tests/validate_generated_system_launch.tscn

const GENERATOR := preload("res://scripts/world_authoring/celestial_system_generator.gd")
const GEN_CONFIG := preload("res://scripts/gen/gen_config.gd")
const BODY_SCRIPT := preload("res://scripts/world_authoring/model/celestial_body_definition.gd")

var _failed := false


func _ready() -> void:
	await get_tree().process_frame

	var baseline := GEN_CONFIG.new()
	baseline.system_seed = 4242
	baseline.face_res = 24
	baseline.erosion_iterations = 6

	# --- Resolve the system + home planet exactly as main._resolve_home_config ---
	var system: CelestialSystemDefinition = GENERATOR.generate(4242, baseline.axial_tilt_deg)
	var home_cfg: Resource = GENERATOR.gen_config_for(system, GENERATOR.HOME_BODY_ID, baseline)

	Planet.configure(home_cfg)
	var fields := PlanetBake.new(home_cfg).bake(Callable(), true)
	_assert(fields != null, "home planet baked")
	Planet.adopt(fields)
	_assert(Planet.ready_state, "home planet is resident (Planet.ready_state)")

	var primary: BodyRuntime = Bodies.primary()
	_assert(primary != null and String(primary.id) == "asterra", "primary BodyRuntime is the home planet")
	_assert(is_equal_approx(Frames.planet_radius, float(home_cfg.get(&"planet_radius"))),
		"Frames radius is the home planet's")

	# --- Register the rest of the system (main._activate_system) --------
	var count: int = GENERATOR.populate_pool(system, baseline, Bodies, Frames, 0.0)
	var solid_others := 0
	for body_value: Variant in system.bodies:
		var body: Resource = body_value as Resource
		var bt: int = int(body.get(&"body_type"))
		if bt != BODY_SCRIPT.BodyType.STAR and String(body.get(&"body_id")) != "asterra":
			solid_others += 1
	_assert(count == solid_others, "populate_pool registered every non-home solid body (%d of %d)" % [count, solid_others])
	_assert(Bodies.slots.size() == 1 + count,
		"the pool holds the home body + %d others (got %d slots)" % [count, Bodies.slots.size()])
	_assert(count >= 2, "a seeded system offers at least two other worlds to fly to (got %d)" % count)

	# Every registered slot has a gen_config, a system centre, and a Frames frame.
	for rt: BodyRuntime in Bodies.slots:
		if rt.is_primary:
			continue
		_assert(rt.gen_config != null, "registered body %s carries a bake config" % rt.id)
		_assert(rt.radius_m > 1.0, "registered body %s has a radius" % rt.id)
		_assert(Frames.has_body_frame(rt.id), "Frames knows body %s's sub-frame" % rt.id)
		var canon: Vec3D = Frames.body_center_canonical(rt.id)
		_assert(canon.length() > 1.0e6, "body %s is offset from the home planet in the canonical frame (%s m)" % [rt.id, String.num_scientific(canon.length())])

	_assert(Frames.has_body_frame(&"asterra"), "Frames knows the home planet's own system centre")
	_assert(String(Frames.active_body_id) == "asterra", "Frames.active_body_id is the home planet")

	# A near body warms without error (cached bake load or a quick CPU bake).
	var nearest: BodyRuntime = null
	var nearest_d := 1.0e30
	for rt: BodyRuntime in Bodies.slots:
		if rt.is_primary:
			continue
		var d: float = Frames.body_center_canonical(rt.id).length()
		if d < nearest_d:
			nearest_d = d
			nearest = rt
	_assert(nearest != null and nearest.warm(), "the nearest other world warms cleanly")
	_assert(nearest.state == BodyRuntime.State.WARM, "the warmed body reports WARM")

	if _failed:
		get_tree().quit(1)
		return
	print("GENERATED_SYSTEM_LAUNCH_OK")
	get_tree().quit(0)


func _assert(condition: bool, message: String) -> void:
	if condition:
		return
	_failed = true
	push_error("GENERATED_SYSTEM_LAUNCH_FAILED: %s" % message)
