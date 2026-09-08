extends Node
## Planet Studio routes its single resident detailed terrain stack through the
## Bodies pool: switching the active authoring target to another primary
## terrestrial body bakes THAT body (and only that body), cache-first, so a
## return to any body baked earlier is instant and no other planet is ever baked.
##   godot --headless --path . res://tests/validate_planet_studio_pool_swap.tscn

const GENERATOR := preload("res://scripts/world_authoring/celestial_system_generator.gd")
const GEN_CONFIG := preload("res://scripts/gen/gen_config.gd")
const GENERATION_PROFILE := preload("res://scripts/world_authoring/model/generation_authoring_profile.gd")
const BODY_SCRIPT := preload("res://scripts/world_authoring/model/celestial_body_definition.gd")
const RUNTIME_HOST := preload("res://scripts/world_authoring/world_authoring_runtime_host_phase25.gd")

const SEED := 12345
const TARGET_A := &"rime"       # ice spine body
const TARGET_B := &"meridian"   # second terran spine body
const BAKE_DIR := "user://asterra/worlds"

var _failed := false
var _frames := 0


func _process(_dt: float) -> void:
	# Fail-safe: if _ready() aborts on an error before it can quit, do not spin.
	_frames += 1
	if _frames > 1800:
		push_error("PLANET_STUDIO_POOL_SWAP_FAILED: timed out before completion")
		get_tree().quit(1)


func _ready() -> void:
	await get_tree().process_frame

	var baseline := GEN_CONFIG.new()
	baseline.system_seed = SEED
	baseline.face_res = 16
	baseline.erosion_iterations = 6

	# --- Boot the home planet exactly as main._resolve_home_config / _activate_system
	var system: CelestialSystemDefinition = GENERATOR.generate(SEED, baseline.axial_tilt_deg)
	var home_cfg: Resource = GENERATOR.gen_config_for(system, GENERATOR.HOME_BODY_ID, baseline)
	Planet.configure(home_cfg)
	Planet.adopt(PlanetBake.new(home_cfg).bake(Callable(), true))
	var primary: BodyRuntime = Bodies.primary()
	_assert(String(primary.id) == "asterra" and primary.is_primary,
		"primary BodyRuntime is the home planet")

	var registered: int = GENERATOR.populate_pool(system, baseline, Bodies, Frames, 0.0)
	_assert(registered >= 3, "the seeded system registered several other worlds (%d)" % registered)

	# Nothing but the home body is resident straight after populate_pool -- registering
	# a body must never bake it.
	var baked_at_start := 0
	for rt: BodyRuntime in Bodies.slots:
		if rt.is_primary:
			continue
		if rt.fields != null or rt.state != BodyRuntime.State.FAR:
			baked_at_start += 1
	_assert(baked_at_start == 0,
		"populate_pool baked no bodies -- every non-home slot is FAR with no fields (%d baked)" % baked_at_start)

	var rt_a: BodyRuntime = Bodies.slot(TARGET_A)
	var rt_b: BodyRuntime = Bodies.slot(TARGET_B)
	_assert(rt_a != null and rt_b != null, "both swap targets have pool slots")

	# --- PARITY: the pool slot config == what the standalone game bakes for it ----
	# populate_pool (Planet Studio + the game share it) registers each slot with
	# gen_config_for(system, id, baseline); a plain switch must not perturb it, so
	# the .bake file the studio produces is the one the game loads.
	for target: StringName in [TARGET_A, TARGET_B]:
		var game_cfg: Resource = GENERATOR.gen_config_for(system, String(target), baseline)
		var slot_cfg: Resource = Bodies.slot(target).gen_config
		_assert(slot_cfg != null and slot_cfg.call("cache_key") == game_cfg.call("cache_key"),
			"%s pool slot bakes the same PlanetFields as the game (cache key parity)" % target)
		_assert(PlanetBake.new(slot_cfg).cache_path() == PlanetBake.new(game_cfg).cache_path(),
			"%s resolves the same on-disk bake path in the studio and the game" % target)
		_assert(int(slot_cfg.get(&"world_seed")) == int(game_cfg.get(&"world_seed")),
			"%s per-body world_seed matches the game" % target)

	# --- Switch the authoring target to body A: it becomes the ONLY resident body --
	var files_before_a := _bake_files()
	Bodies.load_active(TARGET_A, Vec3D.new(0.0, rt_a.radius_m * 4.0, 0.0))
	_assert(String(Bodies.active.id) == String(TARGET_A), "body A is the active resident body")
	_assert(is_equal_approx(Frames.planet_radius, rt_a.radius_m),
		"Frames radius datum followed body A")
	_assert(is_equal_approx(float(Planet.cfg.planet_radius), rt_a.radius_m),
		"the resident Planet config is body A's")
	_assert(rt_a.fields != null, "body A now has resident fields")

	# Only body A got baked -- every other non-home body is still FAR / no fields.
	var other_baked := []
	for rt: BodyRuntime in Bodies.slots:
		if rt.is_primary or rt == rt_a:
			continue
		if rt.fields != null or rt.state != BodyRuntime.State.FAR:
			other_baked.append(String(rt.id))
	_assert(other_baked.is_empty(),
		"switching to body A baked no other planet (unexpected: %s)" % [other_baked])

	var files_after_a := _bake_files()
	_assert(files_after_a.size() >= files_before_a.size(),
		"body A's first bake wrote (at most) its own cache file")
	var a_new_files := files_after_a.size() - files_before_a.size()
	_assert(a_new_files <= 1, "body A's first activation wrote at most one new bake file (%d)" % a_new_files)

	# --- Return to the home planet, then back to body A: the revisit is instant ----
	Bodies.load_active(&"asterra", Vec3D.new(0.0, primary.radius_m * 4.0, 0.0))
	_assert(String(Bodies.active.id) == "asterra", "back on the home planet")
	_assert(is_equal_approx(Frames.planet_radius, primary.radius_m), "Frames radius back to home")
	_assert(rt_a.fields != null, "body A keeps its baked fields while parked (instant return)")

	var files_before_revisit := _bake_files()
	Bodies.load_active(TARGET_A, Vec3D.new(0.0, rt_a.radius_m * 4.0, 0.0))
	_assert(String(Bodies.active.id) == String(TARGET_A), "body A resident again")
	var files_after_revisit := _bake_files()
	_assert(files_after_revisit.size() == files_before_revisit.size(),
		"returning to an already-baked body wrote NO new bake file (%d -> %d)"
			% [files_before_revisit.size(), files_after_revisit.size()])

	# --- Switch to body B: A is released to a cached FAR slot, B bakes once --------
	Bodies.load_active(TARGET_B, Vec3D.new(0.0, rt_b.radius_m * 4.0, 0.0))
	_assert(String(Bodies.active.id) == String(TARGET_B), "body B is the resident body")
	_assert(rt_a.state == BodyRuntime.State.FAR, "body A dropped back to FAR")
	_assert(rt_a.fields != null, "body A's bake is retained for an instant return")
	_assert(rt_b.fields != null, "body B has resident fields")

	# Bodies never touched (e.g. the gas giant, inner rocks) were never baked.
	var never_baked_ok := true
	for rt: BodyRuntime in Bodies.slots:
		if rt.is_primary or rt == rt_a or rt == rt_b:
			continue
		if rt.fields != null:
			never_baked_ok = false
	_assert(never_baked_ok, "no planet outside the visited set was ever baked")

	# --- The host guard now allows a primary terrestrial swap while asterra lives --
	var gap: Resource = GENERATION_PROFILE.new()
	gap.set(&"system_seed", SEED)
	gap.set(&"face_res", 16)
	var session := WorldAuthoringSession.new()
	session.bootstrap_from_generated_system(gap)
	_assert(session.staged_system != null, "session built the staged archetype system")

	var host: Node = RUNTIME_HOST.new()
	add_child(host)
	host.set(&"_authoring_session", session)
	host.set(&"_detailed_runtime_body_id", "asterra")

	var rime_body: Resource = session.staged_system.call("find_body", "rime")
	var colossus_body: Resource = session.staged_system.call("find_body", "colossus")
	var helion_body: Resource = session.staged_system.call("find_body", "helion")
	_assert(bool(host.call(&"_pool_hosts_body", rime_body)),
		"the host sees a pool slot for a spine body")
	_assert(bool(host.call(&"_body_can_own_detailed_runtime", rime_body)),
		"a primary terrestrial body CAN take the detailed runtime over from asterra now")
	_assert(bool(host.call(&"_body_can_own_detailed_runtime", colossus_body)),
		"the gas giant is a primary body and can own the detailed runtime")
	_assert(not bool(host.call(&"_body_can_own_detailed_runtime", helion_body)),
		"a STAR can never own the detailed runtime")

	# A moon (parented to a planet, not the root star) stays on the lightweight preview.
	var a_moon: Resource = null
	for body_value: Variant in session.staged_system.get(&"bodies"):
		var b: Resource = body_value as Resource
		if int(b.get(&"body_type")) == BODY_SCRIPT.BodyType.STAR:
			continue
		var parent_id: String = String(b.get(&"parent_body_id"))
		var parent: Resource = session.staged_system.call("find_body", parent_id)
		if parent != null and int(parent.get(&"body_type")) != BODY_SCRIPT.BodyType.STAR:
			a_moon = b
			break
	if a_moon != null:
		_assert(not bool(host.call(&"_body_can_own_detailed_runtime", a_moon)),
			"a moon (%s) cannot own the single resident detailed runtime" % a_moon.get(&"body_id"))

	# --- The host helper drives a plain switch WITHOUT perturbing bake parity -----
	Bodies.load_active(&"asterra", Vec3D.new(0.0, primary.radius_m * 4.0, 0.0))
	var rime_key_before: String = Bodies.slot(TARGET_A).gen_config.call("cache_key")
	var files_before_helper := _bake_files()
	var handled: bool = host.call(&"_adopt_pool_detailed_runtime", rime_body, false) == true
	_assert(handled, "the host helper claimed the switch to rime")
	_assert(String(Bodies.active.id) == "rime", "the helper made rime the resident body")
	_assert(String(host.get(&"_detailed_runtime_body_id")) == "rime",
		"the host now owns the detailed runtime on rime")
	_assert(Bodies.slot(TARGET_A).gen_config.call("cache_key") == rime_key_before,
		"a plain switch left rime's bake config untouched (still game-identical)")
	_assert(_bake_files().size() == files_before_helper.size(),
		"the plain switch through the host helper baked nothing new")
	var game_rime: Resource = GENERATOR.gen_config_for(system, "rime", baseline)
	_assert(Bodies.slot(TARGET_A).gen_config.call("cache_key") == game_rime.call("cache_key"),
		"rime still bakes byte-for-byte what the standalone game bakes")

	host.queue_free()

	if _failed:
		get_tree().quit(1)
		return
	print("PLANET_STUDIO_POOL_SWAP_OK")
	get_tree().quit(0)


func _bake_files() -> PackedStringArray:
	var out := PackedStringArray()
	var dir := DirAccess.open(BAKE_DIR)
	if dir == null:
		return out
	for f: String in dir.get_files():
		if f.ends_with(".bake"):
			out.append(f)
	out.sort()
	return out


func _assert(condition: bool, message: String) -> void:
	if condition:
		return
	_failed = true
	push_error("PLANET_STUDIO_POOL_SWAP_FAILED: %s" % message)
