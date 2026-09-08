extends Node
## 4K look probe for the gas-giant envelope. Boots the seeded minimal game, makes
## colossus the resident body, and saves 3840x2160 screenshots from a spread of
## vantages (orbit / just-inside / mid / deep) so the atmosphere composition, star
## occlusion and cloud volume can be eyeballed.
##   godot --path . res://tests/gg_look_4k.tscn        (WINDOWED -- no --headless)

var main: Node3D
var _shot_dir := "user://shots/gg4k"


func _ready() -> void:
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	DisplayServer.window_set_size(Vector2i(3840, 2160))
	get_tree().set_meta("launch_mode", "play")
	OS.set_environment("ASTERRA_SYSTEM_SEED", "8571")
	OS.set_environment("ASTERRA_MINIMAL_SYSTEM", "1")
	main = load("res://scenes/Main.tscn").instantiate()
	add_child(main)

	var boot_deadline := Time.get_ticks_msec() + 90000
	while not main._started and Time.get_ticks_msec() < boot_deadline:
		await get_tree().process_frame
	if not main._started:
		push_error("GG_LOOK_4K_FAILED: Main never started")
		get_tree().quit(1)
		return

	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	Engine.max_fps = 0
	main.player.set_mouse_captured(false)
	main.player.input_enabled = false
	main.hud.visible = false
	main.map.visible = false
	DirAccess.make_dir_recursive_absolute(_shot_dir)

	var slot: BodyRuntime = Bodies.slot(&"colossus")
	if slot == null:
		push_error("GG_LOOK_4K_FAILED: no colossus slot")
		get_tree().quit(1)
		return
	var cloud_top: float = slot.radius_m
	var core: float = slot.surface_radius()

	Bodies.load_active(&"colossus", Vec3D.new(0.0, cloud_top * 0.9, 0.0))
	await get_tree().process_frame
	if String(Bodies.active.id) != "colossus":
		push_error("GG_LOOK_4K_FAILED: colossus not resident")
		get_tree().quit(1)
		return

	# Let the core clipmap settle so a shot is not caught mid-build.
	var settle := Time.get_ticks_msec() + 45000
	while Time.get_ticks_msec() < settle:
		await get_tree().process_frame
		var st: Dictionary = main.terrain.stats()
		if int(st.get("in_flight", 1)) == 0 and int(st.get("chunks", 0)) > 0 \
				and Engine.get_frames_per_second() > 40:
			break

	for spec: Array in [
			["01_orbit_far", cloud_top * 1.9, "at"],
			["02_orbit_near", cloud_top * 1.12, "at"],
			["03_skim_tangent", cloud_top * 0.995, "tangent"],
			["04_upper_in", lerpf(cloud_top, core, 0.12), "tangent"],
			["05_upper_up", lerpf(cloud_top, core, 0.12), "out"],
			["06_mid", lerpf(cloud_top, core, 0.45), "tangent"],
			["07_deep_down", lerpf(cloud_top, core, 0.8), "in"]]:
		await _shot(String(spec[0]), float(spec[1]), String(spec[2]))

	print("GG_LOOK_4K_OK  (%s)" % ProjectSettings.globalize_path(_shot_dir))
	get_tree().quit(0)


func _shot(shot_name: String, radius: float, look: String) -> void:
	var p: AsterraPlayer = main.player
	var sun: Vector3 = Frames.helion_dir.normalized()
	if not sun.is_normalized():
		sun = Vector3(1, 0, 0)
	# Vantage on the sunlit side. "at"/"in"/"out" look at the body -> sit closer to
	# the sun line so the disc shows a fuller lit face; "tangent" stays off to the side.
	var off := Vector3(0.2, 0.5, 0.15) if look == "tangent" else Vector3(0.12, 0.28, 0.1)
	var sun_w := 1.0 if look == "tangent" else 2.4
	var d := (sun * sun_w + off).normalized()
	p.world_pos = Vec3D.new(d.x * radius, d.y * radius, d.z * radius)
	Frames.rebase(p.world_pos)
	# "at": look at the body centre (from orbit). "in": look inward/down toward the
	# core. "out": look radially outward toward the cloud tops. "tangent": along the
	# shell, pitched down a touch.
	match look:
		"at", "in":
			p.pitch = -1.15
			p.yaw = 0.0
		"out":
			p.pitch = 1.2
			p.yaw = 0.0
		_:
			p.pitch = -0.12
			p.yaw = 0.6
	p.vertical_speed = 0.0
	p._sync_transform()
	main._prev_player_world = p.world_pos

	for _i in 24:
		p.world_pos = Vec3D.new(d.x * radius, d.y * radius, d.z * radius)
		p.vertical_speed = 0.0
		p._sync_transform()
		await get_tree().process_frame
	await RenderingServer.frame_post_draw
	await get_tree().process_frame
	await RenderingServer.frame_post_draw

	var img := get_viewport().get_texture().get_image()
	var path := "%s/%s.png" % [_shot_dir, shot_name]
	img.save_png(path)
	var sm: ShaderMaterial = main.sky_mat
	print("    sky occ=%.0f obs=%.0f  world_r=%.0f  active=%s  shells=%s" % [
		float(sm.get_shader_parameter("u_occluder_radius")),
		float(sm.get_shader_parameter("u_observer_radius")),
		p.world_pos.length(), String(Bodies.active.id), str(main._gas_shells.keys())])
	var mean := 0.0
	var n := 0
	var whitish := 0        # near-white specks (stars leaking / firefly noise)
	for y in range(0, img.get_height(), 6):
		for x in range(0, img.get_width(), 6):
			var c := img.get_pixel(x, y)
			mean += (c.r + c.g + c.b) / 3.0
			n += 1
			if c.r > 0.75 and c.g > 0.75 and c.b > 0.75:
				whitish += 1
	print("  %s  r=%.0f km  mean %.3f  white %d/%d  %d fps" % [
		shot_name, radius / 1000.0, mean / float(maxi(n, 1)),
		whitish, n, Engine.get_frames_per_second()])
