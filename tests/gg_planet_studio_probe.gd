extends Node
## Reproduces the user's report: in Planet Studio, select the gas giant and it
## renders as its tiny rocky core instead of the big cloud-top ball. Boots Main in
## planet_studio mode, makes colossus the resident authoring target via the host's
## own pool-swap path, and screenshots from far (should show the gas-giant sphere)
## and from inside the envelope (should show deck murk).
##   godot --path . res://tests/gg_planet_studio_probe.tscn   (WINDOWED)

var main: Node3D
var _failed := false


func _ready() -> void:
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	get_tree().set_meta("launch_mode", "planet_studio")
	OS.set_environment("ASTERRA_SYSTEM_SEED", "8571")
	OS.set_environment("ASTERRA_AUTHORING_RECOVERY_PATH",
		"user://world_authoring/tests/gg_ps_probe_%d.tres" % OS.get_process_id())
	main = load("res://scenes/Main.tscn").instantiate()
	add_child(main)

	var deadline := Time.get_ticks_msec() + 90000
	while (not main._started) and Time.get_ticks_msec() < deadline:
		await get_tree().process_frame
	if not main._started:
		return _fail("Main never started")
	# Let the live editor host open.
	for _i in 120:
		await get_tree().process_frame
	var host: Node = main.get_node_or_null("PlanetStudioRuntimeHost")
	if host == null or host.get("_authoring_session") == null:
		return _fail("Planet Studio host / session not up")

	DirAccess.make_dir_recursive_absolute("user://shots")
	main.player.set_mouse_captured(false)
	main.player.input_enabled = false

	var session: Object = host.get("_authoring_session")
	var col: Resource = session.staged_system.call("find_body", "colossus")
	if col == null:
		return _fail("no colossus in the staged system")
	var cloud_top: float = float(col.get(&"radius_m"))
	var core: float = float(col.call("surface_reference_radius_m"))
	print("colossus: cloud_top %.0f km  core %.0f km  is_gas_giant=%s"
		% [cloud_top / 1000.0, core / 1000.0, col.call("is_gas_giant")])

	# Select colossus as the authoring target -- exactly what clicking it does.
	session.select_body("colossus")
	for _i in 150:   # allow the deferred preview flush + a cold core bake
		await get_tree().process_frame
		if String(Bodies.active.id) == "colossus":
			break
	if String(Bodies.active.id) != "colossus":
		return _fail("colossus is not the resident body after selecting it")
	# It must STAY colossus: an earlier bug had a later preview flush swap it back.
	for _i in 90:
		await get_tree().process_frame
	_assert(String(Bodies.active.id) == "colossus",
		"colossus stays the resident body across later preview flushes (was %s)" % Bodies.active.id)
	_assert(String(host.get("_detailed_runtime_body_id")) == "colossus",
		"the host keeps colossus as the detailed runtime")

	# --- Far: ~40,000 km above the cloud tops. Should be the tan gas-giant ball,
	#     NOT a tiny rock; the far-LOD sphere must be visible. --------------
	var far_bright := await _shot("gg_ps_far", cloud_top + 40_000_000.0, col)
	# The host's celestial preview must NOT treat colossus as the hidden "detailed"
	# body -- hiding its cloud-top sphere (as it does for a normal detailed body)
	# would leave only the tiny rocky core on screen, which is the bug being fixed.
	var host_prev: Node = host.get("_celestial_preview")
	_assert(host_prev != null
			and String(host_prev.call("detailed_body_id")) != "colossus",
		"the preview does not hide the gas giant as its 'detailed' body (id=%s)"
			% (host_prev.call("detailed_body_id") if host_prev != null else "<none>"))
	_assert(far_bright > 0.02,
		"the gas giant is visibly on screen from 40,000 km out (mean %.4f)" % far_bright)

	# --- Inside the envelope: deck murk fills the view. ------------------
	var in_bright := await _shot("gg_ps_inside", cloud_top - 8_000_000.0, col)
	_assert(in_bright > 0.03,
		"inside the envelope the deck haze fills the view (mean %.4f)" % in_bright)

	var shell: Node = main.get_node_or_null("GasGiantShell_colossus")
	_assert(shell != null, "the GasGiantShell exists for colossus")

	if _failed:
		get_tree().quit(1)
		return
	print("GG_PS_PROBE_OK  (far %.4f, inside %.4f)" % [far_bright, in_bright])
	get_tree().quit(0)


func _shot(name: String, radius: float, body: Resource) -> float:
	var p: AsterraPlayer = main.player
	var sun: Vector3 = Frames.helion_dir.normalized()
	if not sun.is_normalized():
		sun = Vector3(1, 0, 0)
	var d := (sun + Vector3(0.2, 0.4, 0.15)).normalized()
	var center: Vec3D = Vec3D.new()   # colossus is the active body -> at the origin
	var wp: Vec3D = center.add(Vec3D.from_v3(d).mul(radius))
	p.world_pos = wp
	Frames.rebase(wp)
	p.pitch = -0.4
	p.vertical_speed = 0.0
	p._sync_transform()
	main._prev_player_world = wp
	for _i in 18:
		p.world_pos = wp
		p.vertical_speed = 0.0
		p._sync_transform()
		await get_tree().process_frame
	await RenderingServer.frame_post_draw
	var img := get_viewport().get_texture().get_image()
	img.save_png("user://shots/%s.png" % name)
	var sum := 0.0
	var n := 0
	for y in range(0, img.get_height(), 8):
		for x in range(0, img.get_width(), 8):
			var c := img.get_pixel(x, y)
			sum += (c.r + c.g + c.b) / 3.0
			n += 1
	var mean := sum / float(maxi(n, 1))
	print("  %s: r=%.0f km  mean %.4f  %d fps"
		% [name, radius / 1000.0, mean, Engine.get_frames_per_second()])
	return mean


func _assert(c: bool, m: String) -> void:
	if c:
		return
	_failed = true
	push_error("GG_PS_PROBE_FAILED: %s" % m)


func _fail(m: String) -> void:
	push_error("GG_PS_PROBE_FAILED: %s" % m)
	get_tree().quit(1)
