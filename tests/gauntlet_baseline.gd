extends Node
## ASTERRA PLANETARY NATURALNESS GAUNTLET -- baseline capture harness.
##
## Boots the real Main scene, sites a temperate-forest observer, places the sun
## for morning side-lighting, and writes a battery of rendered PNGs plus a
## machine-readable metrics file so cloud / atmosphere / coherence defects can be
## looked at instead of guessed at.
##
##   godot --path . res://tests/GauntletBaseline.tscn -- --biome=temperate_forest \
##       --sun=18 --azimuth=95 --res=1600x900
##
## Output: user://gauntlet/<run>/*.png + metrics.json

const OUT_ROOT := "user://gauntlet"
const SETTLE_LIMIT_MS := 16000

var _biome := "temperate_forest"
var _sun_elev := 18.0
var _sun_az := 95.0
var _res := Vector2i(1600, 900)

var main: Node3D
var _site := Vector3.ZERO
var _out := ""
var _metrics := {}
var _frames := 0
var _wall0 := 0
var _done := false


func _ready() -> void:
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	_wall0 = Time.get_ticks_msec()
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--biome="):
			_biome = arg.trim_prefix("--biome=")
		elif arg.begins_with("--sun="):
			_sun_elev = float(arg.trim_prefix("--sun="))
		elif arg.begins_with("--azimuth="):
			_sun_az = float(arg.trim_prefix("--azimuth="))
		elif arg.begins_with("--res="):
			var wh := arg.trim_prefix("--res=").split("x", false)
			if wh.size() == 2:
				_res = Vector2i(int(wh[0]), int(wh[1]))

	DisplayServer.window_set_size(_res)
	DisplayServer.window_set_position(Vector2i(0, 0))
	get_window().size = _res
	get_window().content_scale_size = _res
	await get_tree().process_frame

	_out = "%s/%d" % [OUT_ROOT, Time.get_unix_time_from_system()]
	DirAccess.make_dir_recursive_absolute(_out)

	_metrics["run"] = _out
	_metrics["viewport"] = [get_viewport().get_visible_rect().size.x, get_viewport().get_visible_rect().size.y]
	_metrics["sun_elev_deg"] = _sun_elev
	_metrics["sun_az_deg"] = _sun_az
	_metrics["biome"] = _biome
	_metrics["cloud_base_m"] = 800.0
	_metrics["cloud_top_m"] = 14500.0
	_metrics["shots"] = []
	_metrics["errors"] = []

	print("[gauntlet] booting Main ...")
	main = load("res://scenes/Main.tscn").instantiate()
	add_child(main)
	var boot_deadline := Time.get_ticks_msec() + 180000
	while not main._started and Time.get_ticks_msec() < boot_deadline:
		await get_tree().process_frame
	if not main._started:
		_metrics["errors"].append("Main never reached _started within 180s")
		_finish()
		return
	print("[gauntlet] Main started at %.1fs" % ((Time.get_ticks_msec() - _wall0) / 1000.0))

	main.player.set_mouse_captured(false)
	main.player.input_enabled = false
	main.player.vertical_speed = 0.0
	main.hud.visible = false
	main.map.visible = false

	# main.gd re-derives Frames.helion_dir from the orbital sim clock every _process
	# (main.gd:531 _advance_orbit), which silently overrides any harness sun. Null the
	# orbit so _advance_orbit early-returns and the harness owns the sun -- but LEAVE
	# Frames.playing true so system_time_s keeps advancing and WeatherSystem / cloud
	# coverage fields still evolve. (Freezing the clock earlier suppressed clouds.)
	main._game_orbit = null

	_metrics["weather_native_loaded"] = ClassDB.class_exists(&"WeatherNative")
	_metrics["volumetric_clouds_autoload"] = VolumetricClouds != null

	_site = BiomeSites.find(_biome)
	if _site == Vector3.ZERO:
		_metrics["errors"].append("no site for biome '%s'" % _biome)
		_finish()
		return
	_place_sun()

	var info: Dictionary = Planet.sample_info(_site)
	_metrics["site"] = {
		"dir": [_site.x, _site.y, _site.z],
		"biome_name": info.get("biome_name", "?"),
		"elevation_m": info.get("elevation", 0.0),
		"temp_mean_c": info.get("temp_mean", 0.0),
		"precip_mm": info.get("precip", 0.0),
		"vegetation": info.get("vegetation", 0.0),
		"soil_depth_m": info.get("soil_depth", 0.0),
		"terrain_height_m": Planet.terrain_height(_site.normalized()),
		"has_water": Planet.has_water(_site.normalized()),
	}
	print("[gauntlet] site: %s elev %.0fm veg %.2f" % [info.get("biome_name","?"), info.get("elevation",0.0), info.get("vegetation",0.0)])

	# Orbit-elevation cache carries terrain shape at distance; photographing before
	# it lands gives a smooth ball. Wait it out once.
	var orbit_deadline := Time.get_ticks_msec() + 150000
	while Time.get_ticks_msec() < orbit_deadline:
		if Planet.orbit_texture_face_res >= 700:
			break
		await get_tree().process_frame
	_metrics["orbit_texture_face_res"] = Planet.orbit_texture_face_res
	print("[gauntlet] orbit texture face res %d" % Planet.orbit_texture_face_res)

	# Wait for the one-time weather pre-spin to finish so the compositor has a real
	# coverage field (not the flat NEUTRAL_CLOUD fallback) before the cloud shots.
	var weather_wait := Time.get_ticks_msec() + 320000
	if WeatherSystem.has_method("weather_prespin_active"):
		while Time.get_ticks_msec() < weather_wait and WeatherSystem.weather_prespin_active():
			await get_tree().process_frame
	# a few seconds for the spun-up field to publish + advect
	var settle_end := Time.get_ticks_msec() + 6000
	while Time.get_ticks_msec() < settle_end:
		await get_tree().process_frame
	_metrics["weather_global_tex"] = WeatherSystem.global_weather_texture != null
	_metrics["weather_local_tex"] = WeatherSystem.local_weather_texture != null
	_metrics["weather_state_rev"] = WeatherSystem.get("global_state_revision")
	_metrics["sim_time_s_at_shots"] = Frames.system_time_s
	print("[gauntlet] weather global_tex=%s local_tex=%s sim_time=%.1f" % [
		WeatherSystem.global_weather_texture != null,
		WeatherSystem.local_weather_texture != null, Frames.system_time_s])

	# --- still battery -------------------------------------------------------
	await _shot("g01_orbit", 900000.0, -1.15, 0.0, true)
	await _shot("g02_high_oblique", 60000.0, -0.55, 0.0, true)
	await _shot("g03_midair_sidelit", 3500.0, -0.18, 0.0, true)
	await _shot("g04_midair_horizon", 2500.0, -0.03, 35.0, true)
	await _shot("g05_ground_forest", 2.0, 0.04, 0.0, true)
	await _shot("g06_up_at_clouds", 1400.0, 0.62, 0.0, true)
	await _shot("g08_above_layer_down", 18000.0, -1.35, 0.0, true)
	await _shot("g09_in_layer", 6000.0, -0.10, 20.0, true)

	# --- cloud-motion triplet: no teleport between frames, real-time gaps ---
	await _shot("g07_motion_t0", 3500.0, -0.12, 15.0, true)
	await _hold_and_capture("g07_motion_t1", 8.0)
	await _hold_and_capture("g07_motion_t2", 8.0)

	_finish()


func _process(_dt: float) -> void:
	_frames += 1
	# Failsafe: never spin forever if a shot await stalls.
	if not _done and (_frames > 400000 or (Time.get_ticks_msec() - _wall0) > 1200000):
		_metrics["errors"].append("failsafe quit: frames=%d wall=%dms" % [_frames, Time.get_ticks_msec() - _wall0])
		_finish()


func _finish() -> void:
	if _done:
		return
	_done = true
	var f := FileAccess.open("%s/metrics.json" % _out, FileAccess.WRITE)
	if f != null:
		f.store_string(JSON.stringify(_metrics, "  "))
		f.close()
	print("[gauntlet] wrote %s" % ProjectSettings.globalize_path(_out))
	print("[gauntlet] metrics: %s" % JSON.stringify(_metrics))
	get_tree().quit(0)


func _place_sun() -> void:
	var up := _site.normalized()
	var ref := Vector3(0, 1, 0)
	if absf(up.dot(ref)) > 0.995:
		ref = Vector3(1, 0, 0)
	var east := ref.cross(up).normalized()
	var north := up.cross(east).normalized()
	var e := deg_to_rad(_sun_elev)
	var a := deg_to_rad(_sun_az)
	Frames.helion_dir = (up * sin(e) + (north * cos(a) + east * sin(a)) * cos(e)).normalized()
	if main != null:
		main._sync_sun_direction(true)
		main._sync_solar_brightness(true)
	_metrics["helion_dir"] = [Frames.helion_dir.x, Frames.helion_dir.y, Frames.helion_dir.z]


func _aim(p, target_dir: Vector3, pitch: float, yaw_off: float) -> void:
	var up: Vector3 = p.up_dir()
	var ref := Vector3(0, 1, 0)
	if absf(up.dot(ref)) > 0.995:
		ref = Vector3(1, 0, 0)
	var east := ref.cross(up).normalized()
	var north := up.cross(east).normalized()
	var t: Vector3 = target_dir - up * target_dir.dot(up)
	if t.length() > 1e-6:
		t = t.normalized()
		p.yaw = atan2(t.dot(east), t.dot(north)) + deg_to_rad(yaw_off)
	p.pitch = pitch


var _last_alt := -1.0

func _shot(name: String, alt: float, pitch: float, yaw_off: float, settle: bool) -> void:
	var p = main.player
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
	# Re-assert the harness sun in case any per-frame consumer moved it.
	Frames.helion_dir = -look
	if main != null:
		main._sync_sun_direction(true)
	# exposure adaptation + FSR2 temporal convergence
	for i in 48:
		await get_tree().process_frame
	await RenderingServer.frame_post_draw
	var img := get_viewport().get_texture().get_image()
	img.save_png("%s/%s.png" % [_out, name])
	var info: Dictionary = Planet.sample_info(d)
	var rec := {
		"name": name, "alt_m": alt, "pitch": pitch, "yaw_off_deg": yaw_off,
		"chunks": int(main.terrain.stats()["chunks"]),
		"cam_alt_m": p.world_pos.length() - Planet.cfg.planet_radius,
		"ground_m": Planet.terrain_height(d),
		"biome_name": info.get("biome_name", "?"),
		"has_water": Planet.has_water(d),
		"sun_elev_at_site_deg": rad_to_deg(asin(clampf(d.dot(Frames.helion_dir), -1.0, 1.0))),
		"fps": Engine.get_frames_per_second(),
		"mean_luma": _mean_luma(img),
	}
	_metrics["shots"].append(rec)
	print("[gauntlet] %s" % JSON.stringify(rec))


func _hold_and_capture(name: String, seconds: float) -> void:
	# Hold the camera exactly where it is (no rebase, no re-aim) so FSR history and
	# exposure survive; let clouds/weather advance in real time, then capture.
	var t_end := Time.get_ticks_msec() + int(seconds * 1000.0)
	while Time.get_ticks_msec() < t_end:
		await get_tree().process_frame
	await RenderingServer.frame_post_draw
	var img := get_viewport().get_texture().get_image()
	img.save_png("%s/%s.png" % [_out, name])
	var rec := {"name": name, "held_s": seconds, "fps": Engine.get_frames_per_second(), "mean_luma": _mean_luma(img)}
	_metrics["shots"].append(rec)
	print("[gauntlet] %s" % JSON.stringify(rec))


func _mean_luma(img: Image) -> float:
	var small: Image = img.duplicate()
	small.resize(64, 36, Image.INTERPOLATE_BILINEAR)
	var acc := 0.0
	for y in 36:
		for x in 64:
			var c: Color = small.get_pixel(x, y)
			acc += 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b
	return acc / (64.0 * 36.0)
