extends Node
## Camera, visual diagnostics and reproducible capture commands for Planet Studio.
const CAPTURE_DIR := "user://world_authoring/captures"
var editor: Control
var _bright_restore: int = -1

func _exit_tree() -> void:
	if _bright_restore >= 0 and is_instance_valid(editor):
		editor.get_viewport().debug_draw = _bright_restore

func _number(value: Variant) -> bool:
	return (value is float or value is int) and is_finite(float(value))

func _vector(value: Variant) -> bool:
	return value is Array and value.size() == 3 and _number(value[0]) and _number(value[1]) and _number(value[2])

func _v(value: Array) -> Vector3:
	return Vector3(value[0], value[1], value[2])

func _a(value: Vector3) -> Array:
	return [value.x, value.y, value.z]

func _world(value: Array) -> Vec3D:
	return Vec3D.new(value[0], value[1], value[2])

func _body_id() -> String:
	if editor == null:
		return ""
	return String(editor.get("_session").get("staged_system").get("active_body_id"))

## `editor` is resolved fresh per dispatch by the boot autoload (see
## planet_studio_bridge.gd::_resolve_editor) and is null whenever Planet Studio
## is not currently open (main menu, or the live game before the player clicks
## into it) -- this autoload runs from boot specifically so other tools keep
## working through that window, so every editor-touching path here must stay
## null-safe rather than assume `editor` exists.
func _debug_viewport() -> Viewport:
	return editor.get_viewport() if editor != null else get_viewport()

func camera_state() -> Dictionary:
	var player: Node = editor.get("_player") if editor != null else null
	var camera: Camera3D = editor.get("_camera") if editor != null else null
	if player == null or camera == null:
		return {"error": "A live Planet Studio camera is required."}
	editor.call("_sync_interest_camera_transform")
	var p: Vec3D = player.get("world_pos")
	var frame: RefCounted = editor.get("_editor_frame")
	return {"position_m": [p.x, p.y, p.z], "north": _a(frame.get("north")), "up": _a(frame.get("up")),
		"yaw_rad": player.get("yaw"), "pitch_rad": player.get("pitch"), "roll_rad": editor.get("_editor_roll"),
		"fov_deg": camera.fov, "body_id": _body_id(), "forward": _a(-camera.global_basis.z),
		"right": _a(camera.global_basis.x), "near_m": camera.near, "far_m": camera.far}

func camera_command(args: Dictionary) -> Dictionary:
	var state := camera_state()
	if state.has("error"): return state
	var player: Node3D = editor.get("_player")
	var camera: Camera3D = editor.get("_camera")
	var action := String(args.get("action", "state"))
	match action:
		"state": return state
		"move":
			if not _vector(args.get("offset_m")): return {"error": "offset_m requires three finite metres."}
			var offset := _v(args.offset_m)
			var space := String(args.get("space", "camera"))
			if space not in ["camera", "world"]: return {"error": "space must be camera or world."}
			if space == "camera":
				offset = camera.global_basis.x * offset.x + camera.global_basis.y * offset.y - camera.global_basis.z * offset.z
			var p: Vec3D = player.get("world_pos")
			_set_position(p.add(Vec3D.from_v3(offset)))
		"rotate":
			if not _vector(args.get("degrees")): return {"error": "degrees requires [yaw,pitch,roll]."}
			player.set("yaw", float(player.get("yaw")) + deg_to_rad(float(args.degrees[0])))
			player.set("pitch", clampf(float(player.get("pitch")) + deg_to_rad(float(args.degrees[1])), -1.55, 1.55))
			editor.set("_editor_roll", float(editor.get("_editor_roll")) + deg_to_rad(float(args.degrees[2])))
		"zoom":
			if not _number(args.get("fov_deg")) or args.fov_deg < 1 or args.fov_deg > 150:
				return {"error": "fov_deg must be between 1 and 150."}
			camera.fov = float(args.fov_deg)
		"teleport", "slew":
			if not _vector(args.get("position_m")): return {"error": "position_m requires a canonical world position in metres."}
			var target := _clear_local_ground(_world(args.position_m))
			var start: Vec3D = player.get("world_pos")
			var duration: Variant = args.get("duration_s", 1.0)
			if not _number(duration) or duration < 0 or duration > 30: return {"error": "duration_s must be 0–30."}
			if action == "teleport" or duration == 0:
				_set_position(target, true)
			else:
				editor.set("_camera_command_active", true)
				var began := Time.get_ticks_msec()
				var fraction := 0.0
				while fraction < 1.0:
					fraction = minf(float(Time.get_ticks_msec() - began) / (float(duration) * 1000.0), 1.0)
					_set_position(start.add(target.sub(start).mul(smoothstep(0.0, 1.0, fraction))))
					await get_tree().process_frame
				_set_position(target, true)
				editor.set("_camera_command_active", false)
			# The observer jumped past every intermediate streaming step. Give the
			# spherical clipmap / height-page atlas a bounded window to reanchor and
			# upload the destination region before we report a pose that would
			# screenshot as black.
			await _await_terrain_ready()
		"pose":
			var pose: Variant = args.get("pose")
			if not pose is Dictionary: return {"error": "pose must be a camera state returned by this tool."}
			for key: String in ["position_m", "north", "up"]:
				if not _vector(pose.get(key)): return {"error": "Invalid pose vector: " + key}
			for key: String in ["yaw_rad", "pitch_rad", "roll_rad", "fov_deg"]:
				if not _number(pose.get(key)): return {"error": "Invalid pose number: " + key}
			if pose.get("body_id", "") != _body_id(): return {"error": "Select the capture's body before restoring its pose."}
			if pose.fov_deg < 1 or pose.fov_deg > 150 or absf(pose.pitch_rad) > 1.55:
				return {"error": "Pose FOV/pitch outside camera limits."}
			var up := _v(pose.up)
			var north := _v(pose.north)
			if absf(up.length() - 1.0) > 0.001 or absf(north.length() - 1.0) > 0.001 or absf(up.dot(north)) > 0.001:
				return {"error": "Pose frame must be orthonormal."}
			var frame: RefCounted = editor.get("_editor_frame")
			frame.set("up", up)
			frame.set("north", north)
			player.set("yaw", pose.yaw_rad)
			player.set("pitch", pose.pitch_rad)
			editor.set("_editor_roll", pose.roll_rad)
			camera.fov = pose.fov_deg
			_set_position(_world(pose.position_m))
		_: return {"error": "Unknown camera action."}
	editor.call("_sync_interest_camera_transform")
	editor.call("_sync_interest_camera_clip")
	return camera_state()

func _set_position(position: Vec3D, _settle: bool = false) -> void:
	var player: Node3D = editor.get("_player")
	player.set("world_pos", position)
	# maintain_origin only queues; Frames coalesces and commits one rebase on the
	# next physics tick even for a large jump. Do NOT force an immediate
	# Frames.rebase() here -- if the studio host is running a per-frame observer
	# correction (e.g. camera below local terrain) an immediate re-origin per call
	# turns into an origin storm.
	Frames.maintain_origin(Frames.to_render(position))
	editor.call("_sync_interest_camera_transform")
	player.emit_signal("moved", position)


## Raise a requested camera position so it never lands inside the planet. A
## sub-terrain observer sends the studio host into a per-frame "push the camera
## back above ground" correction that thrashes the origin and the clipmap
## (fps collapses, sectors 0/12). Uses the coarse broad-height query, plus a
## generous margin for relief the coarse query misses.
func _clear_local_ground(target: Vec3D) -> Vec3D:
	if target.length_sq() < 1.0:
		return target
	var dir: Vector3 = target.normalized().to_v3()
	var radius: float = Frames.planet_radius
	if Planet.ready_state and Planet.cfg != null:
		radius = float(Planet.cfg.get(&"planet_radius"))
	# coarse_height() is a SYNCHRONOUS macro-elevation query (Planet.terrain_height)
	# -- unlike height(), which is an async broad lookup that returns 0 / a stale
	# cached sample when nothing is resident yet (which is exactly the case right
	# after a long teleport). Fine relief rides on top of the macro value, so the
	# clearance margin also has to cover that.
	# TerrainContactSampler is a `class_name` with static methods, not an autoload,
	# so call it directly. coarse_height() self-guards when Planet is not ready.
	var ground_h: float = maxf(TerrainContactSampler.coarse_height(dir), 0.0)
	var min_len: float = radius + ground_h + GROUND_CLEARANCE_M
	if target.length() >= min_len:
		return target
	return Vec3D.from_v3(dir).mul(min_len)


const GROUND_CLEARANCE_M := 300.0


## Bounded wait until the spherical clipmap reports renderable coverage at the
## current observer, so a teleport/slew result is not returned while the
## destination is still a black frame. Never blocks longer than `timeout_s`.
func _await_terrain_ready(timeout_s: float = 5.0) -> void:
	var ground: Node = get_node_or_null("/root/GroundGeometryClipmap")
	if ground == null or not ground.has_method("gpu_stream_stats"):
		return
	var deadline: int = Time.get_ticks_msec() + int(timeout_s * 1000.0)
	while Time.get_ticks_msec() < deadline:
		# coverage_ready is exactly what the clipmap uses to decide _set_visible:
		# `_bound_orbit != null or GroundHeightPageAtlas.ready_for_shader()`. That is
		# the "will it draw anything" gate; do not also require resident detail
		# pages, which the horizon-limited shell renders fine without.
		if bool(ground.gpu_stream_stats().get("coverage_ready", false)):
			return
		await get_tree().process_frame

func time_state() -> Dictionary:
	return {"seconds": Frames.system_time_s, "rate": Frames.time_scale, "playing": Frames.playing,
		"year": floor(Frames.system_time_s / Frames.year_seconds()), "day": Frames.day_of_year(),
		"day_seconds": Frames.day_seconds, "year_days": Frames.year_days,
		"scope": "Celestial clock only; this does not reverse water, weather, physics or edit history."}

## seek/advance/date set Frames.system_time_s to an arbitrary absolute value --
## a discontinuity, not the continuous per-frame advance WeatherSystem expects.
## Without this, a large forward jump (e.g. a year-scale `date`) leaves its
## simulation accumulator with a backlog it can only drain a few steps per job,
## pinning the scheduler busy and collapsing the whole game toward ~1 fps with
## no recovery. See weather_system.gd::notify_time_jump / planning/PROBLEMS.md P-009.
func _notify_weather_time_jump() -> void:
	var weather: Node = get_node_or_null(^"/root/WeatherSystem")
	if weather != null and weather.has_method("notify_time_jump"):
		weather.call("notify_time_jump")


## WeatherSystem's own job-limit scaling (how many native step_global calls a
## scheduled job may drain) keys off its `simulation_speed` property, which
## only its own set_simulation_speed() normally updates. `rate` here writes
## Frames.time_scale directly and never touched it, so any MCP-driven warp used
## the slowest (1-step-per-job) drain budget regardless of how fast the clock
## was actually told to run -- see MAX_GLOBAL_SIM_BACKLOG_S in weather_system.gd
## for the hard cap that keeps that gap from hanging the game; this just gets
## legitimate moderate-to-high warp closer to full weather fidelity again.
## Poke the plain property directly rather than call set_simulation_speed(),
## which clamps to +-8192 and would overwrite Frames.time_scale right back down
## with that clamped value.
func _sync_weather_simulation_speed(magnitude: float) -> void:
	var weather: Node = get_node_or_null(^"/root/WeatherSystem")
	if weather != null:
		weather.set("simulation_speed", clampf(magnitude, 0.0, 8192.0))


func time_command(args: Dictionary) -> Dictionary:
	match String(args.get("action", "state")):
		"state": pass
		"pause": Frames.playing = false
		"play": Frames.playing = true
		"rate":
			if not _number(args.get("rate")) or absf(args.rate) > 1e7: return {"error": "rate must be finite and between -1e7 and 1e7."}
			Frames.time_scale = args.rate
			Frames.playing = bool(args.get("playing", true))
			_sync_weather_simulation_speed(absf(args.rate))
		"advance", "seek":
			if not _number(args.get("seconds")) or absf(args.seconds) > 1e14: return {"error": "seconds must be finite and within +/-1e14."}
			var target: float = Frames.system_time_s + float(args.seconds) if args.action == "advance" else float(args.seconds)
			if absf(target) > 1e14: return {"error": "Resulting time exceeds +/-1e14 seconds."}
			Frames.system_time_s = target
			Frames.playing = bool(args.get("playing", false))
			_notify_weather_time_jump()
		"date":
			for key: String in ["year", "day", "hour"]:
				if not _number(args.get(key)): return {"error": "date requires numeric year, zero-based day and hour."}
			if int(args.year) != args.year or int(args.day) != args.day or absf(args.year) > 100000 or args.day < 0 or args.hour < 0 or args.hour >= 24 or args.day + args.hour / 24.0 >= Frames.year_days:
				return {"error": "Date is outside the active body's calendar."}
			Frames.system_time_s = args.year * Frames.year_seconds() + (args.day + args.hour / 24.0) * Frames.day_seconds
			Frames.playing = bool(args.get("playing", false))
			_notify_weather_time_jump()
		_: return {"error": "Unknown time action."}
	return time_state()

func debug_menu() -> Node:
	return _find_class(get_tree().root, "DebugMenu")

func _find_class(node: Node, type_name: String) -> Node:
	if (type_name == "DebugMenu" and node is DebugMenu) or (type_name == "TerrainDebug" and node is TerrainDebug): return node
	for child: Node in node.get_children():
		var found := _find_class(child, type_name)
		if found != null: return found
	return null

## Boolean ASTERRA-DEBUG terrain flags reachable without the studio_ui/studio_control
## discovery dance -- each is a direct TerrainDebug setter (see terrain_debug.gd).
const TERRAIN_DEBUG_BOOL_KEYS: Dictionary = {
	"wireframe": "set_wireframe",
	"freeze_terrain": "set_freeze_terrain",
	"side_cut": "set_side_cut",
	"stable_displacement": "set_stable_displacement",
	"stable_ocean_displacement": "set_stable_ocean_displacement",
	"microrelief": "set_microrelief",
	"pbr_detail": "set_pbr_detail",
	"gpu_scatter": "set_gpu_scatter",
	"agl_height_cursor": "set_agl_height_cursor",
	"gpu_height_cursor": "set_gpu_height_cursor",
	"physics_height_cursor": "set_physics_height_cursor",
}
const TERRAIN_DEBUG_FLOAT_KEYS: Dictionary = {
	"sink_scale": "set_sink_scale",
	"aerial_strength": "set_aerial_strength",
}


func _terrain_debug_state(terrain: Node) -> Dictionary:
	if terrain == null:
		return {}
	var out: Dictionary = {}
	for key: String in TERRAIN_DEBUG_BOOL_KEYS:
		out[key] = terrain.get(key)
	for key: String in TERRAIN_DEBUG_FLOAT_KEYS:
		out[key] = terrain.get(key)
	return out


func view_state() -> Dictionary:
	var modes: Dictionary = {}
	for key: String in ClassDB.class_get_enum_constants("Viewport", "DebugDraw"):
		if key != "DEBUG_DRAW_MAX": modes[key.trim_prefix("DEBUG_DRAW_").to_lower()] = ClassDB.class_get_integer_constant("Viewport", key)
	var terrain := _find_class(get_tree().root, "TerrainDebug")
	return {"mode": int(_debug_viewport().debug_draw), "modes": modes, "full_bright": _bright_restore >= 0,
		"geomorph_modes": Array(TerrainDebug.GEOMORPH_MODE_NAMES), "geomorph_mode": terrain.get("geomorph_mode") if terrain != null else null,
		"render_toggles": RenderDebug.get("_state").duplicate(),
		"terrain_debug": _terrain_debug_state(terrain),
		"note": "Buffer availability depends on renderer and enabled effects. studio_ui scope=debug exposes all additional debug controls."}

func view_command(args: Dictionary) -> Dictionary:
	var state := view_state()
	match String(args.get("action", "state")):
		"state": return state
		"mode":
			var mode := String(args.get("mode", ""))
			if not state.modes.has(mode): return {"error": "Unknown mode; inspect modes from studio_view state."}
			var mode_terrain := _find_class(get_tree().root, "TerrainDebug")
			if mode == "wireframe":
				RenderingServer.set_debug_generate_wireframes(true)
				if mode_terrain != null: mode_terrain.call("set_wireframe", true)
			elif mode_terrain != null and bool(mode_terrain.get("wireframe")):
				# TerrainDebug.set_wireframe() independently re-asserts
				# viewport.debug_draw AND drives the "TERRAIN DEBUG WIREFRAME" HUD
				# status label. Switching modes here used to only overwrite
				# debug_draw, leaving TerrainDebug's own flag (and its label) stuck
				# on -- a screenshot taken after "mode: disabled" still read
				# TERRAIN DEBUG WIREFRAME even though nothing was actually
				# wireframe-rendered anymore.
				mode_terrain.call("set_wireframe", false)
			_bright_restore = -1
			_debug_viewport().debug_draw = int(state.modes[mode])
		"terrain_debug":
			var terrain := _find_class(get_tree().root, "TerrainDebug")
			if terrain == null: return {"error": "Terrain diagnostics are unavailable."}
			var key := String(args.get("key", ""))
			if key == "reset":
				terrain.call("reset_inspection")
			elif TERRAIN_DEBUG_BOOL_KEYS.has(key):
				if not args.get("enabled") is bool: return {"error": "enabled must be boolean for key '%s'." % key}
				terrain.call(TERRAIN_DEBUG_BOOL_KEYS[key], args.enabled)
			elif TERRAIN_DEBUG_FLOAT_KEYS.has(key):
				if not _number(args.get("value")): return {"error": "value must be a finite number for key '%s'." % key}
				terrain.call(TERRAIN_DEBUG_FLOAT_KEYS[key], float(args.value))
			else:
				return {"error": "Unknown terrain_debug key. Inspect studio_view state.terrain_debug for valid keys, or use key 'reset'."}
		"full_bright":
			if not args.get("enabled") is bool: return {"error": "enabled must be boolean."}
			if args.enabled:
				if _bright_restore < 0: _bright_restore = int(_debug_viewport().debug_draw)
				_debug_viewport().debug_draw = Viewport.DEBUG_DRAW_UNSHADED
			elif _bright_restore >= 0:
				_debug_viewport().debug_draw = _bright_restore
				_bright_restore = -1
		"debug_menu":
			var menu := debug_menu()
			if menu == null: return {"error": "Runtime debug menu is unavailable."}
			if not args.get("enabled") is bool: return {"error": "enabled must be boolean."}
			if menu.get("visible") != args.enabled: menu.call("toggle")
		"geomorph":
			var terrain := _find_class(get_tree().root, "TerrainDebug")
			if terrain == null: return {"error": "Terrain diagnostics are unavailable."}
			if not _number(args.get("index")) or int(args.index) != args.index or args.index < 0 or args.index >= TerrainDebug.GEOMORPH_MODE_NAMES.size(): return {"error": "Invalid geomorph index."}
			terrain.call("set_geomorph_mode", int(args.index))
		"render_toggle":
			var key := String(args.get("key", ""))
			if not state.render_toggles.has(key) or not args.get("enabled") is bool: return {"error": "Unknown render toggle or invalid enabled value."}
			RenderDebug.call("_set_many", {key: args.enabled})
		_: return {"error": "Unknown view action."}
	return view_state()

func _capture_path(id: String, extension: String) -> String:
	if id.is_empty() or id.length() > 160: return ""
	for c: String in id:
		if not c in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_": return ""
	return CAPTURE_DIR.path_join(id + extension)

func _read_capture(id: String) -> Dictionary:
	var path := _capture_path(id, ".json")
	if path.is_empty() or not FileAccess.file_exists(path): return {"error": "Unknown capture ID."}
	var data: Variant = JSON.parse_string(FileAccess.get_file_as_string(path))
	return data if data is Dictionary else {"error": "Invalid capture metadata."}

func captures(args: Dictionary) -> Dictionary:
	match String(args.get("action", "list")):
		"list":
			var rows: Array = []
			var dir := DirAccess.open(CAPTURE_DIR)
			if dir != null:
				var files := dir.get_files()
				files.sort()
				files.reverse()
				for file: String in files:
					if not file.ends_with(".json"): continue
					var row := _read_capture(file.get_basename())
					if not String(args.get("name", "")).is_empty() and row.get("name", "") != args.name: continue
					rows.append(row)
					if rows.size() >= 100: break
			return {"captures": rows, "directory": ProjectSettings.globalize_path(CAPTURE_DIR), "limit": 100}
		"read": return _read_capture(String(args.get("id", "")))
		"compare": return compare(String(args.get("id", "")), String(args.get("baseline", "")))
		_: return {"error": "Unknown capture action."}

func screenshot(args: Dictionary) -> Dictionary:
	if DisplayServer.get_name() == "headless": return {"error": "Screenshots require a rendered game window."}
	var name := String(args.get("name", ""))
	var intent := String(args.get("intent", ""))
	if (not name.is_empty() and (name.length() > 64 or intent.strip_edges().is_empty())) or intent.length() > 2000:
		return {"error": "Named captures require a name up to 64 characters and intent (1–2000 characters)."}
	# hide_editor is meaningful only once Planet Studio is open; before that
	# (main menu, or the live game with Planet Studio not opened yet) there is
	# nothing to hide, but the screenshot itself still works against whatever
	# viewport is currently live -- lets automation see the start menu too.
	var hidden := bool(args.get("hide_editor", false)) and editor != null
	var was_visible := editor.visible if editor != null else false
	if hidden: editor.hide()
	# Allow the selected camera/debug state to reach rendering before capture.
	await get_tree().process_frame
	await RenderingServer.frame_post_draw
	var picture := _debug_viewport().get_texture().get_image()
	if hidden: editor.visible = was_visible
	if picture == null or picture.is_empty(): return {"error": "Viewport image unavailable."}
	var metadata := {"name": name, "intent": intent, "utc": Time.get_datetime_string_from_system(true),
		"camera": camera_state(), "clock": time_state(), "view": view_state(), "body_id": _body_id(),
		"dirty": editor.get("_session").get("dirty") if editor != null else null,
		"width": picture.get_width(), "height": picture.get_height(),
		"hide_editor": hidden, "engine": Engine.get_version_info().get("string", ""), "tags": args.get("tags", [])}
	if not name.is_empty():
		var slug := ""
		for c: String in name.to_lower(): slug += c if c in "abcdefghijklmnopqrstuvwxyz0123456789-_" else "-"
		var id := "%s-%s-%d-%d" % [Time.get_datetime_string_from_system(true).replace(":", "-").replace("T", "-"), slug, OS.get_process_id(), Time.get_ticks_usec()]
		if DirAccess.make_dir_recursive_absolute(CAPTURE_DIR) != OK: return {"error": "Cannot create capture directory."}
		var png_path := _capture_path(id, ".png")
		var json_path := _capture_path(id, ".json")
		if picture.save_png(png_path) != OK: return {"error": "Cannot write screenshot."}
		metadata.id = id
		metadata.png_path = ProjectSettings.globalize_path(png_path)
		metadata.metadata_path = ProjectSettings.globalize_path(json_path)
		var output := FileAccess.open(json_path, FileAccess.WRITE)
		if output == null:
			DirAccess.remove_absolute(png_path)
			return {"error": "Cannot write capture metadata."}
		output.store_string(JSON.stringify(metadata, "\t"))
		output.close()
		if args.has("baseline"): metadata.comparison = compare(id, String(args.baseline))
	# Keep the stored PNG full-resolution; limit only the inline MCP preview.
	if picture.get_width() > 1600: picture.resize(1600, int(picture.get_height() * 1600.0 / picture.get_width()))
	return {"image": Marshalls.raw_to_base64(picture.save_png_to_buffer()), "mimeType": "image/png", "capture": metadata}

func compare(id: String, baseline: String) -> Dictionary:
	var a := _read_capture(id)
	var b := _read_capture(baseline)
	if a.has("error"): return a
	if b.has("error"): return b
	var current := Image.load_from_file(_capture_path(id, ".png"))
	var previous := Image.load_from_file(_capture_path(baseline, ".png"))
	if current == null or previous == null: return {"error": "Capture PNG is missing."}
	if current.get_size() != previous.get_size(): return {"error": "Capture sizes differ; compare at the same viewport resolution."}
	current.convert(Image.FORMAT_RGBA8)
	previous.convert(Image.FORMAT_RGBA8)
	var x := current.get_data()
	var y := previous.get_data()
	var diff := PackedByteArray()
	diff.resize(x.size())
	var absolute_error := 0.0
	var changed := 0
	for i: int in range(0, x.size(), 4):
		var pixel_max := 0
		for channel: int in 3:
			var delta := absi(int(x[i + channel]) - int(y[i + channel]))
			absolute_error += delta / 255.0
			pixel_max = maxi(pixel_max, delta)
			diff[i + channel] = delta
		diff[i + 3] = 255
		if pixel_max > 5: changed += 1
	var difference := Image.create_from_data(current.get_width(), current.get_height(), false, Image.FORMAT_RGBA8, diff)
	var diff_path := CAPTURE_DIR.path_join(id + "-diff-" + baseline.sha256_text().substr(0, 12) + ".png")
	if difference.save_png(diff_path) != OK: return {"error": "Cannot save comparison image."}
	var mismatches: Array = []
	for key: String in ["camera", "clock", "view", "width", "height", "body_id", "hide_editor"]:
		if JSON.stringify(a.get(key)) != JSON.stringify(b.get(key)): mismatches.append(key)
	var pixels := current.get_width() * current.get_height()
	var result := {"id": id, "baseline": baseline, "mean_absolute_rgb_error": absolute_error / (pixels * 3.0),
		"changed_pixel_fraction": float(changed) / pixels, "channel_threshold": 5.0 / 255.0,
		"diff_path": ProjectSettings.globalize_path(diff_path), "metadata_mismatches": mismatches,
		"note": "Visual difference is not automatically a regression. Animation, exposure and render jitter can change pixels."}
	if not a.has("comparisons"): a.comparisons = {}
	a.comparisons[baseline] = result
	var output := FileAccess.open(_capture_path(id, ".json"), FileAccess.WRITE)
	if output == null: return {"error": "Comparison generated but its metadata could not be saved."}
	output.store_string(JSON.stringify(a, "\t"))
	output.close()
	return result
