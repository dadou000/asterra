extends Node
## M8 verification: a save round-trips which body the player was on and every
## visited body's terrain edits; a v2 (single-planet) save still loads.
##   godot --headless --path . res://tests/validate_multibody_save.tscn

const GEN_CONFIG := preload("res://scripts/gen/gen_config.gd")
const SAVE_GAME := preload("res://scripts/persist/save_game.gd")

const RADIUS_A := 640_000.0
const RADIUS_B := 380_000.0
const SAVE_NAME := "mb_test"
const LEGACY_SAVE := "mb_legacy"

var _failed := false


func _ready() -> void:
	await get_tree().process_frame

	var cfg_a := GEN_CONFIG.new()
	cfg_a.face_res = 24
	cfg_a.erosion_iterations = 6
	cfg_a.world_seed = 0x4d42535f5f5f4141
	cfg_a.planet_radius = RADIUS_A
	cfg_a.system_seed = 77
	Planet.configure(cfg_a)
	Planet.adopt(PlanetBake.new(cfg_a).bake(Callable(), true))
	var pa: BodyRuntime = Bodies.primary()

	var cfg_b := GEN_CONFIG.new()
	cfg_b.face_res = 24
	cfg_b.erosion_iterations = 6
	cfg_b.world_seed = 0x4d42535f5f5f4242
	cfg_b.planet_radius = RADIUS_B
	var pb: BodyRuntime = Bodies.register_body(
		&"bodyB", cfg_b, Vec3D.new(9_000_000.0, 0.0, 0.0), RADIUS_B, 3.1)
	_assert(pb.warm(), "body B warmed")

	# --- Edit A, swap to B, edit B ------------------------------------
	Deltas.clear()
	var lat_a: Array = Deltas.dir_to_lattice(Vector3(1, 0, 0))
	Deltas.add_offset(int(lat_a[0]), int(lat_a[1]), int(lat_a[2]), -4.0, -60.0, 60.0)
	_assert(Deltas.edited_tile_count() == 1, "body A has one edited tile")

	var b_canon: Vec3D = Frames.body_center_canonical(&"bodyB")
	Bodies.set_active(pb, b_canon.add(Vec3D.new(RADIUS_B + 5_000.0, 0.0, 0.0)))
	_assert(String(Bodies.active.id) == "bodyB", "player swapped to body B")
	_assert(Deltas.edited_tile_count() == 0, "the shared store holds body B's (empty) edits after the swap")
	_assert(not pa.delta_blob().is_empty(), "body A's edits were captured on its BodyRuntime")

	var lat_b: Array = Deltas.dir_to_lattice(Vector3(0, 1, 0))
	Deltas.add_offset(int(lat_b[0]), int(lat_b[1]), int(lat_b[2]), 6.5, -60.0, 60.0)
	Deltas.add_offset(int(lat_b[0]), int(lat_b[1]) + 200, int(lat_b[2]), 2.0, -60.0, 60.0)
	var b_tiles := Deltas.edited_tile_count()
	_assert(b_tiles >= 1, "body B has edited tiles (%d)" % b_tiles)

	# --- Save while on body B ---------------------------------------
	var player_state := {"x": 400_000.0, "y": 0.0, "z": 0.0, "yaw": 0.3, "pitch": -0.1,
		"mode": 0, "carry": [], "sim_time_s": 1234.0, "active_body_id": "bodyB"}
	var body_deltas := {}
	for rt: BodyRuntime in Bodies.slots:
		if rt == Bodies.active:
			body_deltas[String(rt.id)] = Deltas.serialize()
		elif not rt.delta_blob().is_empty():
			body_deltas[String(rt.id)] = rt.delta_blob()
	_assert(body_deltas.has("asterra") and body_deltas.has("bodyB"),
		"both visited bodies contribute a delta blob to the save")
	var err := SAVE_GAME.save(SAVE_NAME, cfg_b, player_state, null, 42.0, 77, "bodyB", body_deltas)
	_assert(err == OK, "multi-body save wrote (err %d)" % err)

	# --- Wipe live state, go back to A, then LOAD -------------------
	Deltas.clear()
	pa.set(&"_delta_blob", {})
	pb.set(&"_delta_blob", {})
	Bodies.set_active(pa, b_canon.mul(-1.0).add(Vec3D.new(RADIUS_A + 5_000.0, 0.0, 0.0)))
	Deltas.clear()
	_assert(String(Bodies.active.id) == "asterra" and Deltas.edited_tile_count() == 0,
		"state wiped: back on A with no deltas")

	var data := SAVE_GAME.load_into(SAVE_NAME, cfg_b, null)
	_assert(not data.is_empty(), "save loaded")
	_assert(int(data.get("version", 0)) == SAVE_GAME.VERSION, "save is v%d" % SAVE_GAME.VERSION)
	_assert(String(data.get("active_body_id", "")) == "bodyB", "load payload names body B as active")
	_assert(int(data.get("system_seed", -1)) == 77, "system seed round-tripped")

	SAVE_GAME.restore_multibody(data, Bodies, Frames)
	_assert(String(Bodies.active.id) == "bodyB", "restore made body B active")
	_assert(is_equal_approx(Frames.planet_radius, RADIUS_B), "Frames radius is body B's")
	_assert(String(Frames.active_body_id) == "bodyB", "Frames.active_body_id restored")
	_assert(Deltas.edited_tile_count() == b_tiles,
		"body B's edited tiles restored (%d vs %d)" % [Deltas.edited_tile_count(), b_tiles])
	var a_blob: Dictionary = Bodies.slot(&"asterra").delta_blob()
	_assert(not a_blob.is_empty()
			and (a_blob.get("keys", PackedInt64Array()) as PackedInt64Array).size() == 1,
		"body A's one edited tile is stashed on its BodyRuntime for the next visit")

	var alt := Frames.world_altitude(Vec3D.new(400_000.0, 0.0, 0.0))
	_assert(is_finite(alt), "player position over body B resolves to a finite altitude (%.0f m)" % alt)

	# Returning to A replays its edit.
	Bodies.set_active(Bodies.slot(&"asterra"), Vec3D.new(RADIUS_A + 5_000.0, 0.0, 0.0))
	_assert(Deltas.edited_tile_count() == 1, "flying back to body A replays its saved edit")

	# --- Legacy v2 save still loads --------------------------------
	Deltas.clear()
	Deltas.add_offset(int(lat_a[0]), int(lat_a[1]), int(lat_a[2]), -1.0, -60.0, 60.0)
	var v2_ok := _write_legacy_v2(LEGACY_SAVE, cfg_a)
	_assert(v2_ok, "legacy v2 save written")
	Deltas.clear()
	var v2 := SAVE_GAME.load_into(LEGACY_SAVE, cfg_a, null)
	_assert(not v2.is_empty(), "v2 save loads")
	_assert(String(v2.get("active_body_id", "")) == SAVE_GAME.DEFAULT_BODY_ID,
		"a v2 save defaults active_body_id to '%s'" % SAVE_GAME.DEFAULT_BODY_ID)
	_assert((v2.get("body_deltas", {}) as Dictionary).has(SAVE_GAME.DEFAULT_BODY_ID),
		"a v2 save is normalised to a single-body body_deltas map")
	_assert(Deltas.edited_tile_count() == 1, "v2 deltas restored")

	if _failed:
		get_tree().quit(1)
		return
	print("MULTIBODY_SAVE_OK")
	get_tree().quit(0)


## Hand-write a v2-shape payload (pre-M8) to prove forward compatibility.
func _write_legacy_v2(name: String, cfg: Resource) -> bool:
	DirAccess.make_dir_recursive_absolute(SAVE_GAME.DIR)
	var data := {
		"version": 2,
		"saved_at": "legacy",
		"elapsed": 1.0,
		"world": {"seed": cfg.world_seed, "radius": cfg.planet_radius,
			"face_res": cfg.face_res, "cache_key": cfg.cache_key()},
		"player": {"x": 1.0, "y": 2.0, "z": 3.0, "yaw": 0.0, "pitch": 0.0, "mode": 0, "carry": []},
		"deltas": Deltas.serialize(),
		"piles": [],
	}
	var f := FileAccess.open_compressed(
		SAVE_GAME.path_for(name), FileAccess.WRITE, FileAccess.COMPRESSION_ZSTD)
	if f == null:
		return false
	f.store_var(data, true)
	f.close()
	return true


func _assert(condition: bool, message: String) -> void:
	if condition:
		return
	_failed = true
	push_error("MULTIBODY_SAVE_FAILED: %s" % message)
