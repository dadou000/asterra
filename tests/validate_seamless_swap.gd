extends Node
## M3 verification: the resident detailed stack swaps from one body to another as
## the observer flies across, with no null frame and no modal load. Driven by a
## synthetic 2-body system (the game wires a real CelestialSystemDefinition into
## Bodies in M4).
##   godot --headless --path . res://tests/validate_seamless_swap.tscn

const GEN_CONFIG := preload("res://scripts/gen/gen_config.gd")

const RADIUS_A := 600_000.0
const RADIUS_B := 400_000.0
const MOON_OFFSET_X := 4.0e8   # B's centre in the system frame (A at the origin)

var _failed := false


func _ready() -> void:
	await get_tree().process_frame

	# --- Body A: the resident/primary body ---------------------------------
	var cfg_a := GEN_CONFIG.new()
	cfg_a.face_res = 24
	cfg_a.erosion_iterations = 6
	cfg_a.world_seed = 0x5357415041414141
	cfg_a.planet_radius = RADIUS_A
	Planet.configure(cfg_a)
	var fields_a := PlanetBake.new(cfg_a).bake(Callable(), true)
	Planet.adopt(fields_a)

	var primary: BodyRuntime = Bodies.primary()
	_assert(primary != null and primary.is_primary, "primary BodyRuntime exists")
	_assert(is_equal_approx(Frames.planet_radius, RADIUS_A), "Frames radius starts at body A")

	# --- Body B: a smaller body 4e8 m away --------------------------------
	var cfg_b := GEN_CONFIG.new()
	cfg_b.face_res = 24
	cfg_b.erosion_iterations = 6
	cfg_b.world_seed = 0x5357415042424242
	cfg_b.planet_radius = RADIUS_B
	var rt_b: BodyRuntime = Bodies.register_body(
		&"moonb", cfg_b, Vec3D.new(MOON_OFFSET_X, 0.0, 0.0), RADIUS_B, 1.6)
	_assert(rt_b != null and rt_b.state == BodyRuntime.State.FAR, "body B registered as FAR")
	_assert(Bodies.slots.size() == 2, "two bodies registered")
	_assert(Frames.has_body_frame(&"moonb"), "Frames knows body B's sub-frame")
	_assert(Frames.body_center_canonical(&"moonb").sub(Vec3D.new(MOON_OFFSET_X, 0, 0)).length() < 1.0,
		"B's canonical centre is its system centre minus A's (== A at origin)")

	# Warm B up front so the flight loop is not timing a cold CPU bake.
	_assert(rt_b.warm(), "body B warmed (cached bake load + coarse context)")
	_assert(rt_b.state == BodyRuntime.State.WARM, "body B is WARM after warm()")
	_assert(rt_b.context != null and bool(rt_b.context.get(&"ready_state")),
		"body B built its own coarse GPU context")
	_assert(int(rt_b.context.get(&"generation")) >= 1, "body B context advanced its own generation")

	# --- Pre-flight edit on body A ---------------------------------------
	Deltas.clear()
	var edit_dir := Vector3(1, 0, 0)
	var lat: Array = Deltas.dir_to_lattice(edit_dir)
	Deltas.add_offset(int(lat[0]), int(lat[1]), int(lat[2]), -7.0, -60.0, 60.0)
	_assert(Deltas.edited_tile_count() == 1, "body A has a pending terrain edit")

	# --- Fly from 200 km over A toward B --------------------------------
	var observer := Vec3D.new(RADIUS_A + 200_000.0, 0.0, 0.0)
	var target_x := MOON_OFFSET_X - (RADIUS_B + 40_000.0)
	var steps := 80
	var swapped_at := -1
	var saw_null := false
	for i in steps:
		var t := float(i + 1) / float(steps)
		var x: float = lerpf(RADIUS_A + 200_000.0, target_x, t)
		# Once swapped, the canonical frame is B-centred: re-express the observer.
		observer = Vec3D.new(x, 0.0, 0.0)
		if swapped_at >= 0:
			observer = observer.sub(Vec3D.new(MOON_OFFSET_X, 0.0, 0.0))
		Bodies.update(observer)
		if Bodies.active == null or Planet.cfg == null or Bodies.active.fields == null:
			saw_null = true
		if Bodies.swap_count() == 1 and swapped_at < 0:
			swapped_at = i

	_assert(not saw_null, "no frame during the crossing had a null active body / Planet.cfg")
	_assert(Bodies.swap_count() == 1, "exactly one swap occurred (got %d)" % Bodies.swap_count())
	_assert(swapped_at > 0 and swapped_at < steps - 1,
		"the swap happened mid-flight, not at either end (step %d/%d)" % [swapped_at, steps])
	_assert(String(Bodies.active.id) == "moonb", "body B is active after the crossing")
	_assert(Bodies.contact_body == Bodies.active, "contact follows the active body (M3)")
	_assert(is_equal_approx(Frames.planet_radius, RADIUS_B),
		"Frames radius is now body B's (%s vs %s)" % [Frames.planet_radius, RADIUS_B])
	_assert(String(Frames.active_body_id) == "moonb", "Frames.active_body_id tracked the swap")

	var final_alt := Frames.world_altitude(observer)
	_assert(is_finite(final_alt) and final_alt > 0.0 and final_alt < 200_000.0,
		"observer ends at a small finite positive altitude over body B (%.1f m)" % final_alt)

	# --- Body A's edits were captured; body B started clean --------------
	var blob: Dictionary = Bodies.slot(&"asterra").delta_blob()
	_assert(not blob.is_empty() and (blob.get("keys", PackedInt64Array()) as PackedInt64Array).size() > 0,
		"body A's terrain edit was serialised into its BodyRuntime on cold()")
	_assert(Deltas.edited_tile_count() == 0,
		"the shared Deltas store now holds body B's (empty) edits, not A's")

	# --- Return trip restores body A's edits ----------------------------
	Bodies.set_active(primary, observer.add(Vec3D.new(MOON_OFFSET_X, 0.0, 0.0)))
	_assert(String(Bodies.active.id) == "asterra", "swapped back to body A")
	_assert(is_equal_approx(Frames.planet_radius, RADIUS_A), "Frames radius back to body A")
	_assert(Deltas.edited_tile_count() == 1, "body A's terrain edit was replayed on return")

	if _failed:
		get_tree().quit(1)
		return
	print("SEAMLESS_SWAP_OK")
	get_tree().quit(0)


func _assert(condition: bool, message: String) -> void:
	if condition:
		return
	_failed = true
	push_error("SEAMLESS_SWAP_FAILED: %s" % message)
