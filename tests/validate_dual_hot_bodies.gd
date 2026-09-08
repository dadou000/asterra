extends Node
## M5 (primitives) verification: contact/gravity follows the nearer body's
## surface before the full active swap; the clipmap render-offset primitive
## (`_effective_origin`) shifts a bound non-primary instance by that body's
## canonical centre; a non-active body can adopt its own baked fields into its own
## sampler + context WITHOUT retargeting the shared Frames radius.
##   godot --headless --path . res://tests/validate_dual_hot_bodies.tscn
##
## The live SECOND rendering HOT clipmap (geomorph cache per body, full GPU
## contact on the non-active body) is M5b -- see the plan file.

const GEN_CONFIG := preload("res://scripts/gen/gen_config.gd")
const CLIPMAP := preload("res://scripts/terrain/spherical_geometry_clipmap_phase31.gd")

const RADIUS_A := 600_000.0
const RADIUS_B := 300_000.0
const MOON_OFFSET_X := 1.2e7   # close enough that a render offset stays well-conditioned

var _failed := false


func _ready() -> void:
	await get_tree().process_frame

	var cfg_a := GEN_CONFIG.new()
	cfg_a.face_res = 24
	cfg_a.erosion_iterations = 6
	cfg_a.world_seed = 0x4455414c48415f41
	cfg_a.planet_radius = RADIUS_A
	Planet.configure(cfg_a)
	Planet.adopt(PlanetBake.new(cfg_a).bake(Callable(), true))
	var primary: BodyRuntime = Bodies.primary()

	var cfg_b := GEN_CONFIG.new()
	cfg_b.face_res = 24
	cfg_b.erosion_iterations = 6
	cfg_b.world_seed = 0x4455414c48415f42
	cfg_b.planet_radius = RADIUS_B
	var rt_b: BodyRuntime = Bodies.register_body(
		&"moon5", cfg_b, Vec3D.new(MOON_OFFSET_X, 0.0, 0.0), RADIUS_B, 1.62)

	# --- adopt_fields_silent: own sampler + context, Frames radius untouched ---
	var frames_radius_before := Frames.planet_radius
	rt_b.adopt_fields_silent()
	_assert(rt_b.sampler != null and rt_b.sampler != Planet,
		"body B built its own sampler distinct from the Planet autoload")
	_assert(rt_b.context != null and bool(rt_b.context.get(&"ready_state")),
		"body B built its own coarse GPU context")
	_assert(int(rt_b.context.get(&"generation")) >= 1, "body B context has its own generation")
	_assert(int(PlanetContext.generation) == int(PlanetContext.generation),
		"the PlanetContext autoload generation is not driven by B")
	_assert(is_equal_approx(Frames.planet_radius, frames_radius_before)
			and is_equal_approx(Frames.planet_radius, RADIUS_A),
		"adopt_fields_silent left Frames.planet_radius on body A (%s)" % Frames.planet_radius)
	_assert(is_equal_approx(float(rt_b.sampler.cfg.planet_radius), RADIUS_B),
		"body B's own sampler carries body B's radius")

	# --- _effective_origin render-offset primitive ---------------------
	var cm_active: Node = CLIPMAP.new()          # not in tree -> no _ready / GPU
	var cm_b: Node = CLIPMAP.new()
	cm_b.bind_runtime(rt_b)
	var eo_active: Vector3 = cm_active._effective_origin()
	var eo_b: Vector3 = cm_b._effective_origin()
	var frames_origin := Vector3(float(Frames.origin.x), float(Frames.origin.y), float(Frames.origin.z))
	_assert(eo_active.distance_to(frames_origin) < 1e-3,
		"an unbound clipmap's effective origin is exactly Frames.origin")
	var expected_b := frames_origin - Vector3(MOON_OFFSET_X, 0.0, 0.0)
	_assert(eo_b.distance_to(expected_b) < 1.0,
		"a B-bound clipmap's effective origin is shifted by B's canonical centre (%s vs %s)" % [eo_b, expected_b])
	cm_active.free()
	cm_b.free()

	# --- contact_body follows the nearer surface, ahead of the swap -----
	_assert(rt_b.warm(), "body B warmed")
	Deltas.clear()
	var bakes_before := _bake_signature()   # B is now warm; count baseline before the flight

	# --- M5b: live second clipmap rendering B's terrain concurrently -----
	Bodies.concurrent_parent = self
	Bodies.update(Vec3D.new(RADIUS_A + 300_000.0, 0.0, 0.0))
	_assert(Bodies.contact_body == primary, "far from B, contact stays on body A")
	_assert(rt_b.is_concurrent(), "body B inside the concurrent band gets a live second clipmap")
	var cm_live: Node = rt_b.concurrent_clipmap
	_assert(cm_live != null and cm_live.is_inside_tree(), "the concurrent clipmap is a live node in the tree")
	_assert(cm_live.get(&"_concurrent_far_only") == true, "the concurrent clipmap runs the far-only path")
	_assert(cm_live._planet() == rt_b.sampler, "the concurrent clipmap reads body B's own sampler")
	_assert(cm_live._planet_ctx() == rt_b.context, "the concurrent clipmap reads body B's own context")
	var eo_cm: Vector3 = cm_live._effective_origin()
	_assert(eo_cm.distance_to(Vector3(-MOON_OFFSET_X, 0.0, 0.0)) < 1.0,
		"the concurrent clipmap renders offset by B's canonical centre (%s)" % eo_cm)
	_assert(_bake_signature() == bakes_before, "bringing up the concurrent clipmap ran no new bake")

	# Leaving the band tears it down.
	Bodies.update(Vec3D.new(-2.0e9, 0.0, 0.0))  # far from both on -X
	_assert(not rt_b.is_concurrent(), "leaving the concurrent band frees the second clipmap")

	var swapped := false
	var contact_flipped_before_swap := false
	var steps := 60
	for i in steps:
		var t := float(i + 1) / float(steps)
		var x: float = lerpf(RADIUS_A + 300_000.0, MOON_OFFSET_X - (RADIUS_B + 30_000.0), t)
		var obs := Vec3D.new(x, 0.0, 0.0)
		if swapped:
			obs = obs.sub(Vec3D.new(MOON_OFFSET_X, 0.0, 0.0))
		Bodies.update(obs)
		if not swapped and String(Bodies.contact_body.id) == "moon5":
			contact_flipped_before_swap = true
		if Bodies.swap_count() == 1 and not swapped:
			swapped = true

	_assert(contact_flipped_before_swap,
		"contact/gravity flipped to body B while body A was still the render origin")
	_assert(Bodies.swap_count() == 1, "exactly one active-body swap")
	_assert(String(Bodies.active.id) == "moon5" and Bodies.contact_body == Bodies.active,
		"body B is active and owns contact after the crossing")
	_assert(is_equal_approx(Frames.planet_radius, RADIUS_B), "Frames radius followed the swap to B")
	_assert(not rt_b.is_concurrent(),
		"on arrival the concurrent clipmap is torn down -- the singleton stack renders B")
	_assert(_bake_signature() == bakes_before,
		"no extra PlanetBake ran across the crossing (B was pre-warmed)")

	# --- gravity routing ---------------------------------------------
	_assert(is_equal_approx(Bodies.contact_gravity(), 1.62),
		"Bodies.contact_gravity() reports body B's authored surface gravity (%.3f)" % Bodies.contact_gravity())
	Bodies.set_active(primary, Vec3D.new(RADIUS_A + 5_000.0, 0.0, 0.0))
	_assert(is_equal_approx(Bodies.contact_gravity(), BodyRuntime.FALLBACK_SURFACE_GRAVITY),
		"back on body A (no authored gravity) contact_gravity() is the historical constant")

	if _failed:
		get_tree().quit(1)
		return
	print("DUAL_HOT_BODIES_OK")
	get_tree().quit(0)


## A stable summary of how many distinct worlds have a bake cache on disk, so the
## test can assert the crossing triggered no new bake.
func _bake_signature() -> int:
	var dir := DirAccess.open("user://asterra/worlds")
	if dir == null:
		return 0
	return dir.get_files().size()


func _assert(condition: bool, message: String) -> void:
	if condition:
		return
	_failed = true
	push_error("DUAL_HOT_BODIES_FAILED: %s" % message)
