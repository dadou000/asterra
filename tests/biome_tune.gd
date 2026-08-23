extends Node
## Live shader tuning harness: boots the planet once, then takes orders.
##
## Visual iteration on terrain is dominated by startup, not by the change being
## tested. A bake load, a terrain build and an orbit-elevation cache is roughly a
## minute before the first pixel, and the interesting edit is usually two lines
## of shader. This scene pays that cost once and then sits in a loop reading
## commands from a JSON file, so a shader edit to photograph is seconds.
##
##   godot --path . res://tests/BiomeTune.tscn -- --biome=temperate_forest
##
## Then, from anywhere, write user://tune/cmd.json:
##
##   {"seq": 1, "ops": [
##       {"op": "reload"},
##       {"op": "set", "name": "u_forest", "value": 0.0},
##       {"op": "shot", "name": "test", "alt": 100000, "pitch": -0.85}]}
##
## and wait for user://tune/ack.json to report the same seq. Shader source is
## re-read from disk on `reload`, `#include` and all, so the shader being tuned
## is whatever is on disk right now -- no restart, no reimport.
##
## Ops:
##   reload                              recompile every terrain shader from disk
##   set    name value                   shader uniform (float, or [x,y,z] colour)
##   env    name value                   Environment property
##   sun    elev azimuth                 solar position at the current site
##   site   biome                        move to the most representative cell
##   shot   name alt pitch [yaw] [settle] photograph
##   stage  n                            u_debug_stage
##   quit

const SETTLE_LIMIT_MS := 45000
const CMD_PATH := "user://tune/cmd.json"
const ACK_PATH := "user://tune/ack.json"
const SHOT_DIR := "user://tune/shots"

var main: Node3D
var _biome_name := "temperate_forest"
var _sun_elevation := 30.0
var _sun_azimuth := 135.0
var _site := Vector3.ZERO
var _last_alt := -1.0
var _seq := -1
var _busy := false
var _log: Array[String] = []
var _frames := 0
var _poll_wait := 0.0
var _res := Vector2i.ZERO
var _idle_quit_frames := 0

func _ready() -> void:
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--biome="):
			_biome_name = arg.trim_prefix("--biome=")
		elif arg.begins_with("--sun="):
			_sun_elevation = float(arg.trim_prefix("--sun="))
		elif arg.begins_with("--azimuth="):
			_sun_azimuth = float(arg.trim_prefix("--azimuth="))
		elif arg.begins_with("--res="):
			var wh := arg.trim_prefix("--res=").split("x", false)
			if wh.size() == 2:
				_res = Vector2i(int(wh[0]), int(wh[1]))

	if _res.x > 0:
		# Resize before the world exists, so nothing caches the old size. The
		# window is placed at the origin because a 4K window on a smaller desktop
		# is otherwise positioned partly off-screen, and Godot renders only what
		# it thinks is visible.
		DisplayServer.window_set_size(_res)
		DisplayServer.window_set_position(Vector2i(0, 0))
		get_window().size = _res
		get_window().content_scale_size = _res
		await get_tree().process_frame
		print("viewport %s" % str(get_viewport().get_visible_rect().size))

	main = load("res://scenes/Main.tscn").instantiate()
	add_child(main)
	while not main._started:
		await get_tree().process_frame
	main.player.set_mouse_captured(false)
	# Freeze the player outright. Without this it falls under gravity between
	# commands, and since altitude drives the quadtree the terrain quietly
	# restreams while the harness thinks nothing has changed -- which arrives in
	# the image as holes where chunks have not landed yet, and reads exactly like
	# a coastline defect.
	main.player.input_enabled = false
	main.player.vertical_speed = 0.0
	main.hud.visible = false
	main.map.visible = false
	DirAccess.make_dir_recursive_absolute("user://tune")
	DirAccess.make_dir_recursive_absolute(SHOT_DIR)

	_site = BiomeSites.find(_biome_name)
	if _site == Vector3.ZERO:
		print("no site for '%s'" % _biome_name)
		get_tree().quit(1)
		return
	_place_sun()
	_report_site()

	# The orbit elevation cache is what carries terrain shape at distance, and it
	# is built on a worker over roughly half a minute. Photographing before it
	# lands gives a smooth ball and a wasted iteration, so wait it out once here
	# rather than hoping each shot happens to be late enough.
	var deadline := Time.get_ticks_msec() + 120000
	while Time.get_ticks_msec() < deadline:
		if Planet.orbit_texture_face_res >= 700:
			break
		await get_tree().process_frame
	print("orbit texture face res %d" % Planet.orbit_texture_face_res)

	# Clear any stale order from a previous session before announcing readiness.
	_seq = _read_seq()
	_write_ack(_seq, true)
	print("TUNE READY")

func _process(dt: float) -> void:
	_frames += 1
	if _busy:
		return
	# Throttled. Polling every frame keeps a read handle on the command file
	# hundreds of times a second, which on Windows is enough to make the writer's
	# atomic rename fail more often than it succeeds.
	_poll_wait -= dt
	if _poll_wait > 0.0:
		return
	_poll_wait = 0.25
	var s := _read_seq()
	if s != _seq:
		_seq = s
		_run_commands()

# ------------------------------------------------------------------ command ---
func _read_seq() -> int:
	if not FileAccess.file_exists(CMD_PATH):
		return -1
	var txt := FileAccess.get_file_as_string(CMD_PATH)
	var parsed = JSON.parse_string(txt)
	if typeof(parsed) != TYPE_DICTIONARY:
		return -1
	return int(parsed.get("seq", -1))

func _run_commands() -> void:
	_busy = true
	_log.clear()
	var parsed = JSON.parse_string(FileAccess.get_file_as_string(CMD_PATH))
	var ops: Array = parsed.get("ops", []) if typeof(parsed) == TYPE_DICTIONARY else []
	for op in ops:
		if typeof(op) != TYPE_DICTIONARY:
			continue
		await _run_one(op)
	_write_ack(_seq, true)
	_busy = false

func _run_one(op: Dictionary) -> void:
	match String(op.get("op", "")):
		"reload":
			_reload_shaders()
		"set":
			_set_param(String(op.get("name", "")), op.get("value"))
		"env":
			_set_env(String(op.get("name", "")), op.get("value"))
		"stage":
			_set_param("u_debug_stage", int(op.get("n", 0)))
		"sun":
			_sun_elevation = float(op.get("elev", _sun_elevation))
			_sun_azimuth = float(op.get("azimuth", _sun_azimuth))
			_place_sun()
			_log.append("sun %.0f/%.0f" % [_sun_elevation, _sun_azimuth])
		"site":
			var want := String(op.get("biome", _biome_name))
			var d := BiomeSites.find(want)
			if d == Vector3.ZERO:
				_log.append("no site for '%s'" % want)
			else:
				_biome_name = want
				_site = d
				_place_sun()
				_last_alt = -1.0
				_report_site()
		"shot":
			await _shot(String(op.get("name", "shot")), float(op.get("alt", 300.0)),
				float(op.get("pitch", -0.34)), float(op.get("yaw", 0.0)),
				bool(op.get("settle", true)))
		"physics":
			main.player.input_enabled = bool(op.get("on", true))
			_log.append("player physics %s" % main.player.input_enabled)
		"field":
			_dump_field(String(op.get("name", "field")), float(op.get("span", 400000.0)),
				int(op.get("res", 512)))
		"tree":
			_report_tree()
		"viewport":
			var vp := get_viewport()
			var mode := String(op.get("mode", ""))
			if mode == "fsr2":
				vp.scaling_3d_mode = Viewport.SCALING_3D_MODE_FSR2
			elif mode == "bilinear":
				vp.scaling_3d_mode = Viewport.SCALING_3D_MODE_BILINEAR
			if op.has("scale"):
				vp.scaling_3d_scale = float(op["scale"])
			if op.has("sharpness"):
				vp.fsr_sharpness = float(op["sharpness"])
			if op.has("msaa"):
				vp.msaa_3d = int(op["msaa"]) as Viewport.MSAA
			_log.append("viewport mode %d scale %.2f sharp %.2f msaa %d"
				% [vp.scaling_3d_mode, vp.scaling_3d_scale, vp.fsr_sharpness,
				vp.msaa_3d])
		"sharpness":
			GraphicsQuality.configure_viewport(get_viewport(),
				AppSettings.graphics_quality)
			get_viewport().fsr_sharpness = float(op.get("value", 0.18))
			_log.append("fsr_sharpness %.2f" % get_viewport().fsr_sharpness)
		"stats":
			await _watch_stats(float(op.get("secs", 8.0)))
		"quit":
			_write_ack(_seq, true)
			get_tree().quit()
		_:
			_log.append("unknown op %s" % op)

## Dump what the planet actually contains around the site, as data rather than
## pixels.
##
## A defect at orbital range can live in the generator, in the baked cell colour,
## in the orbit texture or in the shader, and all four look identical through a
## camera. These panels answer "what is actually there?" directly: if the field
## has ridges and the render does not, the fault is downstream of the field, and
## if the baked albedo is already a pale wash then no amount of shading work will
## save it.
##
## Gnomonic projection about the site, so a great circle is a straight line: a
## coastline that looks straight here really is straight on the planet rather
## than being straightened by the projection.
func _dump_field(name: String, span: float, res: int) -> void:
	var up := _site.normalized()
	var ref := Vector3(0, 1, 0)
	if absf(up.dot(ref)) > 0.995:
		ref = Vector3(1, 0, 0)
	var east := ref.cross(up).normalized()
	var north := up.cross(east).normalized()
	var radius: float = Planet.cfg.planet_radius
	var t0 := Time.get_ticks_msec()
	var detail: TerrainDetail = Planet.make_detail()

	var dirs: Array[Vector3] = []
	dirs.resize(res * res)
	var h := PackedFloat32Array()
	h.resize(res * res)
	for y in res:
		var fy := (float(y) / float(res - 1) - 0.5) * 2.0 * span
		for x in res:
			var fx := (float(x) / float(res - 1) - 0.5) * 2.0 * span
			var d := (up + east * (fx / radius) + north * (-fy / radius)).normalized()
			dirs[y * res + x] = d
			h[y * res + x] = Planet.terrain_height(d, detail)

	var metres_per_px := 2.0 * span / float(res - 1)
	var panels := {
		"height": Image.create(res, res, false, Image.FORMAT_RGB8),
		"hillshade": Image.create(res, res, false, Image.FORMAT_RGB8),
		"biome": Image.create(res, res, false, Image.FORMAT_RGB8),
		"veg": Image.create(res, res, false, Image.FORMAT_RGB8),
		"soil": Image.create(res, res, false, Image.FORMAT_RGB8),
		"albedo": Image.create(res, res, false, Image.FORMAT_RGB8),
	}
	# Sun in the local tangent frame, matching where _place_sun put it.
	var sun_e := deg_to_rad(_sun_elevation)
	var sun_a := deg_to_rad(_sun_azimuth)
	var sun2 := Vector2(sin(sun_a), cos(sun_a)) * cos(sun_e)
	for y in res:
		for x in res:
			var i := y * res + x
			var d: Vector3 = dirs[i]
			panels["height"].set_pixel(x, y, _hypso(h[i]))
			# Central differences on the sampled grid, lit by the harness sun.
			var xl: int = maxi(x - 1, 0)
			var xr: int = mini(x + 1, res - 1)
			var yd: int = maxi(y - 1, 0)
			var yu: int = mini(y + 1, res - 1)
			var gx := (h[y * res + xr] - h[y * res + xl]) / (float(xr - xl) * metres_per_px)
			var gy := (h[yu * res + x] - h[yd * res + x]) / (float(yu - yd) * metres_per_px)
			# north is -y in image space
			var lambert := clampf((-gx * sun2.x + gy * sun2.y + sin(sun_e))
				/ sqrt(gx * gx + gy * gy + 1.0), 0.0, 1.0)
			panels["hillshade"].set_pixel(x, y, Color(lambert, lambert, lambert))
			var info: Dictionary = Planet.sample_info(d)
			panels["biome"].set_pixel(x, y, PlanetFields.BIOME_COLORS[int(info["biome"])])
			var v := clampf(float(info["vegetation"]), 0.0, 1.0)
			panels["veg"].set_pixel(x, y, Color(v * 0.35, v, v * 0.25))
			var sd := clampf(float(info["soil_depth"]) / 1.5, 0.0, 1.0)
			panels["soil"].set_pixel(x, y, Color(sd, sd * 0.7, sd * 0.45))
			var c: Color = Planet.surface_color(d)
			panels["albedo"].set_pixel(x, y, Color(c.r * 4.0, c.g * 4.0, c.b * 4.0))
	for k in panels:
		panels[k].save_png("%s/%s_%s.png" % [SHOT_DIR, name, k])
	_log.append("field %s %d^2 over +-%.0f km in %d ms"
		% [name, res, span / 1000.0, Time.get_ticks_msec() - t0])

## Hypsometric tint with a hard line at zero, so the shoreline is unmistakable
## and elevation is still readable either side of it.
func _hypso(h: float) -> Color:
	if h < 0.0:
		var t := clampf(-h / 4000.0, 0.0, 1.0)
		return Color(0.05, 0.18, 0.55).lerp(Color(0.01, 0.03, 0.14), t)
	var u := clampf(h / 3000.0, 0.0, 1.0)
	if u < 0.02:
		return Color(0.95, 0.92, 0.55)
	if u < 0.35:
		return Color(0.16, 0.42, 0.18).lerp(Color(0.55, 0.52, 0.24), u / 0.35)
	return Color(0.55, 0.52, 0.24).lerp(Color(0.97, 0.97, 0.99), (u - 0.35) / 0.65)

## What the quadtree is actually drawing, by depth.
##
## "Chunks exist" and "chunks are on screen" are different claims, and a stats
## counter that only reports the first cannot tell them apart. This walks the
## tree and reports residency and visibility separately, per level.
func _report_tree() -> void:
	var resident := {}
	var drawn := {}
	var visible_flag := {}
	var stack: Array = []
	for r in main.terrain.roots:
		stack.append(r)
	var nodes := 0
	while not stack.is_empty():
		var n = stack.pop_back()
		nodes += 1
		if n.chunk != null:
			resident[n.depth] = int(resident.get(n.depth, 0)) + 1
			if n.drawn:
				drawn[n.depth] = int(drawn.get(n.depth, 0)) + 1
			if n.chunk.visible:
				visible_flag[n.depth] = int(visible_flag.get(n.depth, 0)) + 1
		for c in n.children:
			stack.append(c)
	# A parent only hands off to its children once every leaf under it has a
	# mesh. One leaf that will never get one therefore pins the whole branch at
	# its coarsest level, so those are worth counting separately.
	var starved := {}
	var starved_states := {}
	stack.clear()
	for r2 in main.terrain.roots:
		stack.append(r2)
	while not stack.is_empty():
		var n2 = stack.pop_back()
		if n2.children.is_empty() and n2.chunk == null:
			starved[n2.depth] = int(starved.get(n2.depth, 0)) + 1
			var key := "s%d" % n2.state
			starved_states[key] = int(starved_states.get(key, 0)) + 1
		for c2 in n2.children:
			stack.append(c2)
	if not starved.is_empty():
		var sd: Array = starved.keys()
		sd.sort()
		var sp: Array[String] = []
		for d in sd:
			sp.append("d%d:%d" % [d, int(starved[d])])
		_log.append("leaves with no mesh: %s | states %s"
			% [" ".join(sp), starved_states])
	var depths: Array = resident.keys()
	depths.sort()
	var parts: Array[String] = []
	for d in depths:
		parts.append("d%d res %d drawn %d vis %d"
			% [d, int(resident[d]), int(drawn.get(d, 0)), int(visible_flag.get(d, 0))])
	_log.append("tree nodes %d | %s" % [nodes, "  ".join(parts)])
	_log.append("terrain node visible %s  child count %d"
		% [main.terrain.visible, main.terrain.get_child_count()])

## Sample the streamer while nothing moves.
##
## "Stationary" is the case a terrain streamer is least often tested in and the
## one a player spends most of their time in, so it gets its own probe: if the
## quadtree is still churning after several seconds with a frozen camera,
## something is oscillating rather than converging.
func _watch_stats(secs: float) -> void:
	var t0 := Time.get_ticks_msec()
	var lo := 1 << 30
	var hi := 0
	var churn := 0
	var last := -1
	var frames := 0
	var q_hi := 0
	var flight_hi := 0
	while Time.get_ticks_msec() - t0 < int(secs * 1000.0):
		await get_tree().process_frame
		var st: Dictionary = main.terrain.stats()
		var c := int(st["chunks"])
		lo = mini(lo, c)
		hi = maxi(hi, c)
		q_hi = maxi(q_hi, int(st.get("queued", 0)))
		flight_hi = maxi(flight_hi, int(st.get("in_flight", 0)))
		if last >= 0 and c != last:
			churn += 1
		last = c
		frames += 1
	var st2: Dictionary = main.terrain.stats()
	_log.append("stats %.0fs frames %d chunks %d (min %d max %d, %d changes) queued %d/%d in_flight %d/%d handoffs %d"
		% [secs, frames, int(st2["chunks"]), lo, hi, churn,
		int(st2.get("queued", 0)), q_hi, int(st2.get("in_flight", 0)), flight_hi,
		int(st2.get("handoffs", 0))])

func _write_ack(seq: int, done: bool) -> void:
	var f := FileAccess.open(ACK_PATH, FileAccess.WRITE)
	if f == null:
		return
	f.store_string(JSON.stringify({"seq": seq, "done": done, "log": _log}))
	f.close()

# ------------------------------------------------------------------- shaders ---
## Re-read a shader from disk with its `#include` tree expanded inline.
##
## Assigning the raw file text would leave the include directives to Godot's
## preprocessor, which resolves them through the resource cache and so keeps
## serving whatever the includes contained at load time. Expanding them here from
## FileAccess is what makes editing a .gdshaderinc actually take effect.
func _expand(path: String, seen: Dictionary) -> String:
	if seen.has(path):
		return ""
	seen[path] = true
	var src := FileAccess.get_file_as_string(path)
	if src.is_empty():
		push_warning("empty or missing shader source: %s" % path)
	var out := ""
	for line in src.split("\n"):
		var t := line.strip_edges()
		if t.begins_with("#include"):
			var q1 := t.find("\"")
			var q2 := t.rfind("\"")
			if q1 >= 0 and q2 > q1:
				out += _expand(t.substr(q1 + 1, q2 - q1 - 1), seen) + "\n"
				continue
		out += line + "\n"
	return out

func _reload_shaders() -> void:
	var targets := {
		"res://shaders/terrain_ground.gdshader": main.terrain.debug_materials()[0],
		"res://shaders/ocean.gdshader": main.terrain.debug_materials()[1],
	}
	if main.sky_mat != null:
		targets["res://shaders/atmosphere_sky.gdshader"] = main.sky_mat
	if main.orbit_ocean != null and main.orbit_ocean.material() != null:
		targets["res://shaders/orbit_ocean.gdshader"] = main.orbit_ocean.material()
	for path in targets:
		var mat: ShaderMaterial = targets[path]
		if mat == null or mat.shader == null:
			continue
		var code := _expand(path, {})
		if code.strip_edges().is_empty():
			_log.append("reload skipped (no source): %s" % path)
			continue
		mat.shader.code = code
		_log.append("reloaded %s (%d chars)" % [path.get_file(), code.length()])

func _terrain_mats() -> Array:
	var mats: Array = main.terrain.debug_materials()
	if main.orbit_ocean != null and main.orbit_ocean.material() != null:
		mats.append(main.orbit_ocean.material())
	if main.sky_mat != null:
		mats.append(main.sky_mat)
	return mats

func _set_param(name: String, value) -> void:
	var v = value
	if typeof(value) == TYPE_ARRAY:
		var a: Array = value
		if a.size() == 3:
			v = Vector3(float(a[0]), float(a[1]), float(a[2]))
		elif a.size() == 4:
			v = Color(float(a[0]), float(a[1]), float(a[2]), float(a[3]))
	for mat in _terrain_mats():
		mat.set_shader_parameter(name, v)
	_log.append("set %s = %s" % [name, v])

func _set_env(name: String, value) -> void:
	var env := main.get_viewport().world_3d.environment
	if env == null:
		var cam := main.get_viewport().get_camera_3d()
		if cam != null:
			env = cam.environment
	if env == null:
		_log.append("no environment")
		return
	env.set(name, value)
	_log.append("env %s = %s" % [name, value])

# -------------------------------------------------------------------- siting ---
func _place_sun() -> void:
	var up := _site.normalized()
	var ref := Vector3(0, 1, 0)
	if absf(up.dot(ref)) > 0.995:
		ref = Vector3(1, 0, 0)
	var east := ref.cross(up).normalized()
	var north := up.cross(east).normalized()
	var elev := deg_to_rad(_sun_elevation)
	var az := deg_to_rad(_sun_azimuth)
	Frames.helion_dir = (up * sin(elev)
		+ (north * cos(az) + east * sin(az)) * cos(elev)).normalized()

func _report_site() -> void:
	var info: Dictionary = Planet.sample_info(_site)
	print("site: %s  elev %.0f m  temp %.1f C  precip %.0f mm  veg %.2f  soil %.2f m  rock %s"
		% [info["biome_name"], info["elevation"], info["temp_mean"], info["precip"],
		info["vegetation"], info["soil_depth"], info["rock_name"]])
	_log.append("site %s elev %.0f veg %.2f"
		% [info["biome_name"], info["elevation"], info["vegetation"]])

# --------------------------------------------------------------------- shots ---
func _shot(name: String, alt: float, pitch: float, yaw_off: float,
		settle: bool) -> void:
	var p: AsterraPlayer = main.player
	var d := _site.normalized()
	var r: float = Planet.cfg.planet_radius + maxf(Planet.terrain_height(d), 0.0) + alt
	var look := -Frames.helion_dir
	var moved := absf(alt - _last_alt) > 1.0
	_last_alt = alt

	p.world_pos = Vec3D.new(d.x * r, d.y * r, d.z * r)
	Frames.rebase(p.world_pos)
	_aim(p, look, pitch, yaw_off)
	p.vertical_speed = 0.0
	p._sync_transform()

	# Only a change of altitude forces a full restream. A shader edit leaves the
	# geometry exactly as it was, so the wait is a fraction of a second instead of
	# half a minute -- which is the entire point of this harness. The short wait
	# is still a real wait: photographing a quadtree mid-rebuild puts holes in the
	# image that are indistinguishable from a shading bug.
	# `settle=false` is an optimisation for shots that did not move, and honouring
	# it after a teleport photographs whatever the quadtree happened to have
	# resident at the old altitude -- which looks exactly like a rendering bug.
	if moved:
		settle = true
	var want_stable := 30 if moved else 6
	var limit: int = SETTLE_LIMIT_MS if moved else 4000
	var stable := 0
	var last_chunks := -1
	var deadline := Time.get_ticks_msec() + limit
	while settle and Time.get_ticks_msec() < deadline:
		p.world_pos = Vec3D.new(d.x * r, d.y * r, d.z * r)
		p.vertical_speed = 0.0
		_aim(p, look, pitch, yaw_off)
		p._sync_transform()
		await get_tree().process_frame
		var st: Dictionary = main.terrain.stats()
		var idle: bool = int(st["in_flight"]) == 0 and int(st["handoffs"]) == 0 \
			and int(st["chunks"]) > 0 and int(st["chunks"]) == last_chunks
		last_chunks = int(st["chunks"])
		stable = stable + 1 if idle else 0
		if stable > want_stable:
			break
	# Two separate things need time on a static camera before the image means
	# anything: the exposure controller adapts over a fifth of a second, and FSR2
	# is temporal -- a teleport throws away its history, and a single frame after
	# one is a jittered, unconverged sample rather than the image the player sees.
	for i in 48:
		await get_tree().process_frame
	await RenderingServer.frame_post_draw
	var img := get_viewport().get_texture().get_image()
	img.save_png("%s/%s.png" % [SHOT_DIR, name])
	var info: Dictionary = Planet.sample_info(d)
	var ground: float = Planet.terrain_height(d)
	_log.append("shot %s alt %.0f chunks %d | cam_r %.0f ground %.0f above %.1f water %s biome %s sun_elev %.1f"
		% [name, alt, int(main.terrain.stats()["chunks"]),
		p.world_pos.length() - Planet.cfg.planet_radius, ground,
		p.world_pos.length() - Planet.cfg.planet_radius - ground,
		"yes" if Planet.has_water(d) else "no", info["biome_name"],
		rad_to_deg(asin(clampf(d.dot(Frames.helion_dir), -1.0, 1.0)))])
	print("  %s" % _log[-1])

func _aim(p: AsterraPlayer, target_dir: Vector3, pitch: float, yaw_off: float) -> void:
	var up := p.up_dir()
	var ref := Vector3(0, 1, 0)
	if absf(up.dot(ref)) > 0.995:
		ref = Vector3(1, 0, 0)
	var east := ref.cross(up).normalized()
	var north := up.cross(east).normalized()
	var t := target_dir - up * target_dir.dot(up)
	if t.length() > 1e-6:
		t = t.normalized()
		p.yaw = atan2(t.dot(east), t.dot(north)) + deg_to_rad(yaw_off)
	p.pitch = pitch
