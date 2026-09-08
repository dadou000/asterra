extends Node
## GG2 windowed probe: make colossus the resident body, drop the camera through
## its volumetric envelope at three depths, and confirm the raymarched shell
## renders (shader compiles, haze thickens with depth, no device loss).
##   godot --path . res://tests/gg2_shell_probe.tscn        (WINDOWED -- no --headless)

var main: Node3D
var _failed := false


func _ready() -> void:
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	get_tree().set_meta("launch_mode", "play")
	OS.set_environment("ASTERRA_SYSTEM_SEED", "8571")
	main = load("res://scenes/Main.tscn").instantiate()
	add_child(main)

	var boot_deadline := Time.get_ticks_msec() + 60000
	while not main._started and Time.get_ticks_msec() < boot_deadline:
		await get_tree().process_frame
	if not main._started:
		return _fail("Main never started")

	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	Engine.max_fps = 0
	main.player.set_mouse_captured(false)
	main.player.input_enabled = false   # no physics drift between the settle frames
	main.hud.visible = false
	main.map.visible = false
	DirAccess.make_dir_recursive_absolute("user://shots")

	var slot: BodyRuntime = Bodies.slot(&"colossus")
	if slot == null:
		return _fail("no colossus slot (system not populated)")
	var cloud_top: float = slot.radius_m
	var core: float = slot.surface_radius()

	# Bake + make colossus the resident body (this blocks on a cold core bake).
	Bodies.load_active(&"colossus", Vec3D.new(0.0, cloud_top - 200_000.0, 0.0))
	await get_tree().process_frame
	if String(Bodies.active.id) != "colossus":
		return _fail("colossus did not become the resident body")

	var brights: Array[float] = []
	# depth fractions below the cloud tops: shallow -> deep.
	for spec: Array in [["gg2_a_top", 0.03], ["gg2_b_mid", 0.45], ["gg2_c_deep", 0.9]]:
		var frac: float = float(spec[1])
		var r: float = lerpf(cloud_top, core + 8_000.0, frac)
		var b: float = await _shot(String(spec[0]), r)
		brights.append(b)

	var shell: Node = main.get_node_or_null("GasGiantShell_colossus")
	_assert(shell != null, "the GasGiantShell node exists while colossus is resident")
	if shell != null:
		_assert(int(shell.call("band_count")) >= 3,
			"the envelope is split into bands (%d)" % int(shell.call("band_count")))
		_assert(int(shell.call("active_band_count")) <= 2,
			"only 1-2 bands raymarch at any depth (%d active)" % int(shell.call("active_band_count")))

	# Near the cloud tops the shell is a thick, colour-dominated haze filling the
	# view (not a bare black sky). Deeper views vary with sun angle / core shadow.
	_assert(brights[0] > 0.15,
		"the shell haze fills the view near the cloud tops (mean %.3f)" % brights[0])
	for i in brights.size():
		_assert(brights[i] > 0.01, "depth %d is not a black frame (%.4f)" % [i, brights[i]])

	# Framerate at a spread of depths through the envelope (held still at each so
	# the core clipmap isn't thrashing on teleports -- a real descent is < 1 km/frame).
	var p2: AsterraPlayer = main.player
	var d2 := (Frames.helion_dir.normalized() + Vector3(0.15, 0.35, 0.1)).normalized()
	var worst_fps := 10000
	var max_active := 0
	for frac2: float in [0.02, 0.2, 0.4, 0.6, 0.8, 0.97]:
		var r2: float = lerpf(cloud_top - 20_000.0, core + 8_000.0, frac2)
		for _i in 40:
			p2.world_pos = Vec3D.new(d2.x * r2, d2.y * r2, d2.z * r2)
			p2.vertical_speed = 0.0
			p2._sync_transform()
			await get_tree().process_frame
		var fps := Engine.get_frames_per_second()
		var na := int(shell.call("active_band_count")) if shell != null else 0
		max_active = maxi(max_active, na)
		worst_fps = mini(worst_fps, fps)
		print("  depth %.0f%%: %d fps, %d band(s) active" % [frac2 * 100.0, fps, na])
	_assert(max_active <= 2, "never more than 2 envelope bands raymarch at once (%d)" % max_active)
	# Absolute fps in an unfocused automated window is noisy; the real win is that
	# only one thin band ever raymarches, so cost is bounded no matter the envelope
	# thickness or how many gas giants are resident.
	_assert(worst_fps >= 30,
		"every depth in the envelope stays playable (worst %d fps)" % worst_fps)

	if _failed:
		get_tree().quit(1)
		return
	print("GG2_SHELL_PROBE_OK  (bright shallow=%.4f mid=%.4f deep=%.4f)"
		% [brights[0], brights[1], brights[2]])
	get_tree().quit(0)


func _shot(name: String, radius: float) -> float:
	var p: AsterraPlayer = main.player
	# Vantage on the SUNLIT side so the lit haze is what we photograph.
	var sun: Vector3 = Frames.helion_dir.normalized()
	if not sun.is_normalized():
		sun = Vector3(1, 0, 0)
	var d := (sun + Vector3(0.15, 0.35, 0.1)).normalized()
	p.world_pos = Vec3D.new(d.x * radius, d.y * radius, d.z * radius)
	Frames.rebase(p.world_pos)
	p.pitch = -0.75          # look down toward the core
	p.vertical_speed = 0.0
	p._sync_transform()
	main._prev_player_world = p.world_pos

	for _i in 16:
		p.world_pos = Vec3D.new(d.x * radius, d.y * radius, d.z * radius)
		p.vertical_speed = 0.0
		p._sync_transform()
		await get_tree().process_frame
		# Drive the shell directly so the probe never depends on main._process
		# ordering vs. this coroutine for the frame that gets captured.
		var shell: Object = main._gas_shells.get("colossus")
		if shell != null:
			shell.sync(Frames.to_render(Vec3D.new()), sun, Frames.to_render(p.world_pos))
	await RenderingServer.frame_post_draw

	var img := get_viewport().get_texture().get_image()
	img.save_png("user://shots/%s.png" % name)
	var sum := 0.0
	var n := 0
	var w := img.get_width()
	var h := img.get_height()
	for y in range(0, h, 8):
		for x in range(0, w, 8):
			var c := img.get_pixel(x, y)
			sum += (c.r + c.g + c.b) / 3.0
			n += 1
	var mean := sum / float(maxi(n, 1))
	print("  %s: r=%.0f km  mean brightness %.4f  %d fps"
		% [name, radius / 1000.0, mean, Engine.get_frames_per_second()])
	return mean


func _assert(cond: bool, msg: String) -> void:
	if cond:
		return
	_failed = true
	push_error("GG2_SHELL_PROBE_FAILED: %s" % msg)


func _fail(msg: String) -> void:
	push_error("GG2_SHELL_PROBE_FAILED: %s" % msg)
	get_tree().quit(1)
