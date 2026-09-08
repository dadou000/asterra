extends Node
## M6 verification: cruise from a moon of planet A to the surface of planet B
## (~7e10 m away). The velocity ray warms B early so its bake finishes mid-cruise;
## the slot budget holds at <= 2 HOT + 1 WARM; the single deep-space re-origin
## happens once, near the midpoint; no null active body at any step.
##   godot --headless --path . res://tests/validate_interplanetary_cruise.tscn

const GEN_CONFIG := preload("res://scripts/gen/gen_config.gd")

const AU := 1.495978707e11
const RADIUS_A := 700_000.0
const RADIUS_B := 500_000.0
const RADIUS_M := 220_000.0
const A_X := AU
const M_X := AU + 5_000_000.0      # moon 5e6 m out from planet A
const B_X := AU + 7.0e10           # planet B ~7e10 m from A

var _failed := false


func _ready() -> void:
	await get_tree().process_frame

	# --- Planet A: the resident body, orbiting the star at 1 AU ---------
	var cfg_a := GEN_CONFIG.new()
	cfg_a.face_res = 24
	cfg_a.erosion_iterations = 6
	cfg_a.world_seed = 0x435241_49534541
	cfg_a.planet_radius = RADIUS_A
	Planet.configure(cfg_a)
	Planet.adopt(PlanetBake.new(cfg_a).bake(Callable(), true))
	var pa: BodyRuntime = Bodies.primary()
	Frames.set_body_frame(&"planetA", Vec3D.new(A_X, 0.0, 0.0), RADIUS_A)
	Frames.active_body_id = &"planetA"
	pa.center_system = Vec3D.new(A_X, 0.0, 0.0)

	var cfg_m := GEN_CONFIG.new()
	cfg_m.face_res = 20
	cfg_m.erosion_iterations = 5
	cfg_m.world_seed = 0x4d4f4f4e_49534541
	cfg_m.planet_radius = RADIUS_M
	var mm: BodyRuntime = Bodies.register_body(
		&"moonM", cfg_m, Vec3D.new(M_X, 0.0, 0.0), RADIUS_M, 1.4)

	var cfg_b := GEN_CONFIG.new()
	cfg_b.face_res = 24
	cfg_b.erosion_iterations = 6
	cfg_b.world_seed = 0x504c414e_45542042
	cfg_b.planet_radius = RADIUS_B
	var pb: BodyRuntime = Bodies.register_body(
		&"planetB", cfg_b, Vec3D.new(B_X, 0.0, 0.0), RADIUS_B, 8.7)

	# --- Start on the moon (M3/M5 near-swap) ---------------------------
	var start_canon_about_a := Vec3D.new(M_X - A_X + RADIUS_M + 300_000.0, 0.0, 0.0)
	Bodies.set_active(mm, start_canon_about_a)
	_assert(String(Bodies.active.id) == "moonM", "player begins on moon M")
	_assert(Bodies.swap_count() == 1, "one swap so far (onto the moon)")
	_assert(is_equal_approx(Frames.planet_radius, RADIUS_M), "Frames radius is the moon's")

	# --- Cruise M -> B ------------------------------------------------
	# All positions now canonical about moon M. B's centre about M:
	var b_about_m: Vec3D = Frames.body_center_canonical(&"planetB")
	_assert(b_about_m.length() > 6.0e10 and b_about_m.length() < 8.0e10,
		"planet B is ~7e10 m from the moon in the canonical frame (%s)" % String.num_scientific(b_about_m.length()))

	var start := Vec3D.new(RADIUS_M + 300_000.0, 0.0, 0.0)
	var dest := Vec3D.new(b_about_m.x - (RADIUS_B + 40_000.0), 0.0, 0.0)
	var steps := 120
	var dt := 1.0

	var max_hot := 0
	var max_warm := 0
	var saw_null := false
	var b_warm_step := -1
	var b_warm_dist := 0.0
	var b_below_atmo_step := -1
	var prev := start
	var swap_step := -1

	for i in steps:
		var t := float(i + 1) / float(steps)
		var pos := Vec3D.new(lerpf(start.x, dest.x, t), 0.0, 0.0)
		# After the swap the canonical frame is B-centred; re-express.
		if swap_step >= 0:
			pos = Vec3D.new(lerpf(start.x, dest.x, t) - b_about_m.x, 0.0, 0.0)
		var vel := pos.sub(prev).mul(1.0 / dt)
		prev = pos
		Bodies.update(pos, vel)

		if Bodies.active == null or Planet.cfg == null or Bodies.active.fields == null:
			saw_null = true

		var hot := 0
		var warm := 0
		for rt: BodyRuntime in Bodies.slots:
			if rt == Bodies.active or rt.is_concurrent():
				hot += 1
			elif rt.state == BodyRuntime.State.WARM:
				warm += 1
		max_hot = maxi(max_hot, hot)
		max_warm = maxi(max_warm, warm)

		if b_warm_step < 0 and pb.state >= BodyRuntime.State.WARM:
			b_warm_step = i
			b_warm_dist = Frames.world_altitude_over(pos, &"planetB")

		if Bodies.swap_count() == 2 and swap_step < 0:
			swap_step = i

		if b_below_atmo_step < 0 and Frames.world_altitude_over(pos, &"planetB") < 60_000.0:
			b_below_atmo_step = i
			_assert(pb.fields != null and bool(pb.context.get(&"ready_state")),
				"planet B's terrain was resident BEFORE the player entered its atmosphere")

	_assert(not saw_null, "no cruise step had a null active body / Planet.cfg / fields")
	_assert(max_hot <= 2, "at most 2 HOT bodies at any step (got %d)" % max_hot)
	_assert(max_warm <= Bodies.MAX_WARM_SLOTS, "at most %d WARM body at any step (got %d)" % [Bodies.MAX_WARM_SLOTS, max_warm])
	_assert(Bodies.swap_count() == 2, "exactly one deep-space re-origin during the cruise (swap_count %d)" % Bodies.swap_count())
	_assert(swap_step > steps / 5 and swap_step < steps * 4 / 5,
		"the re-origin happened around the midpoint, not at either end (step %d/%d)" % [swap_step, steps])
	_assert(b_warm_step >= 0 and b_warm_dist > 1.0e9,
		"planet B was warmed by the velocity ray while still >1e9 m away (%s m at step %d)" % [String.num_scientific(b_warm_dist), b_warm_step])
	_assert(b_below_atmo_step >= 0, "the player did reach planet B's atmosphere")
	_assert(String(Bodies.active.id) == "planetB", "planet B is active at journey's end")
	_assert(is_equal_approx(Frames.planet_radius, RADIUS_B), "Frames radius is planet B's")
	_assert(String(Frames.active_body_id) == "planetB", "Frames.active_body_id tracked the re-origin")

	var final_alt := Frames.world_altitude(prev)
	_assert(is_finite(final_alt) and final_alt > 0.0 and final_alt < 200_000.0,
		"the player ends at a small finite altitude over planet B (%.0f m)" % final_alt)

	if _failed:
		get_tree().quit(1)
		return
	print("INTERPLANETARY_CRUISE_OK")
	get_tree().quit(0)


func _assert(condition: bool, message: String) -> void:
	if condition:
		return
	_failed = true
	push_error("INTERPLANETARY_CRUISE_FAILED: %s" % message)
