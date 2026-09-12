extends Node
## Gauntlet probe: does the native weather GCM actually build cloud coverage given
## time-warped spin-up, and do the volumetric clouds then render?
##
##   godot --path . res://tests/GauntletWeatherSoak.tscn --quit-after 400000 -- \
##       --biome=temperate_forest --sun=18 --azimuth=95 --res=1600x900
##
## Output: user://gauntlet_soak/<ts>/  (log.txt + a few PNGs)

const WARP := 4096.0
const SOAK_WALL_S := 330.0
const COVERAGE_TARGET := 0.05
const OUT_ROOT := "user://gauntlet_soak"

var _biome := "temperate_forest"
var _sun_elev := 18.0
var _sun_az := 95.0
var _res := Vector2i(1600, 900)

var main: Node3D
var _site := Vector3.ZERO
var _out := ""
var _log: Array[String] = []
var _frames := 0
var _wall0 := 0
var _done := false


func _ready() -> void:
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	_wall0 = Time.get_ticks_msec()
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--biome="): _biome = arg.trim_prefix("--biome=")
		elif arg.begins_with("--sun="): _sun_elev = float(arg.trim_prefix("--sun="))
		elif arg.begins_with("--azimuth="): _sun_az = float(arg.trim_prefix("--azimuth="))
		elif arg.begins_with("--res="):
			var wh := arg.trim_prefix("--res=").split("x", false)
			if wh.size() == 2: _res = Vector2i(int(wh[0]), int(wh[1]))

	DisplayServer.window_set_size(_res)
	DisplayServer.window_set_position(Vector2i(0, 0))
	get_window().size = _res
	get_window().content_scale_size = _res
	await get_tree().process_frame

	_out = "%s/%d" % [OUT_ROOT, Time.get_unix_time_from_system()]
	DirAccess.make_dir_recursive_absolute(_out)

	_p("[soak] booting Main")
	main = load("res://scenes/Main.tscn").instantiate()
	add_child(main)
	var deadline := Time.get_ticks_msec() + 180000
	while not main._started and Time.get_ticks_msec() < deadline:
		await get_tree().process_frame
	if not main._started:
		_p("[soak] Main never started"); _finish(); return
	_p("[soak] Main started %.1fs" % ((Time.get_ticks_msec() - _wall0) / 1000.0))

	main.player.set_mouse_captured(false)
	main.player.input_enabled = false
	main.player.vertical_speed = 0.0
	main.hud.visible = false
	main.map.visible = false
	main._game_orbit = null

	_site = BiomeSites.find(_biome)
	if _site == Vector3.ZERO:
		_p("[soak] no site"); _finish(); return
	_place_sun()

	_p("[soak] weather_native=%s vol_clouds=%s" % [
		ClassDB.class_exists(&"WeatherNative"), VolumetricClouds != null])
	_p("[soak] set_simulation_speed(%d)" % int(WARP))
	WeatherSystem.set_simulation_speed(WARP)

	# --- soak loop: log coverage vs sim-time -------------------------------
	var next_log := 0.0
	var last_mean := -1.0
	var plateau := 0
	var t0 := Time.get_ticks_msec()
	while (Time.get_ticks_msec() - t0) < int(SOAK_WALL_S * 1000.0):
		await get_tree().process_frame
		var wall := (Time.get_ticks_msec() - t0) / 1000.0
		if wall >= next_log:
			next_log += 5.0
			var g := _tex_stats(WeatherSystem.global_weather_texture)
			var l := _tex_stats(WeatherSystem.local_weather_texture)
			var cov_site := _sample_coverage(WeatherSystem.global_weather_texture, _site)
			_p("[soak] wall=%5.1fs sim=%8.0fs  G r[min %.4f max %.4f mean %.4f]  L r[min %.4f max %.4f mean %.4f]  site_cov=%.4f  state=%s" % [
				wall, Frames.system_time_s,
				g.x, g.y, g.z, l.x, l.y, l.z, cov_site,
				str(WeatherSystem.get("global_state_revision"))])
			# require spatial variation so the flat NEUTRAL_CLOUD=0.16 fallback field
			# does not count as "spun up"
			if g.z >= COVERAGE_TARGET and (g.y - g.z) > 0.02:
				_p("[soak] coverage reached target mean %.4f (max %.4f)" % [g.z, g.y])
				break
			if last_mean >= 0.0 and absf(g.z - last_mean) < 0.0015 and g.z > 0.02:
				plateau += 1
			else:
				plateau = 0
			last_mean = g.z
			if plateau >= 5:
				_p("[soak] coverage plateaued at mean %.4f" % g.z)
				break

	# --- shots at whatever coverage we reached ----------------------------
	WeatherSystem.set_simulation_speed(1.0)
	await _shot("s1_orbit", 900000.0, -1.15)
	await _shot("s2_high_oblique", 60000.0, -0.55)
	await _shot("s3_midair_sidelit", 3500.0, -0.18)
	await _shot("s6_up_at_clouds", 1400.0, 0.62)
	await _shot("s7_above_layer_down", 18000.0, -1.35)
	await _shot("s8_in_layer", 6000.0, -0.10)

	# --- fallback sky-cloud path forced on, for comparison ---------------
	var sky_mat: ShaderMaterial = _sky_material()
	if sky_mat != null:
		sky_mat.set_shader_parameter("u_cloud_enabled", 1.0)
		_p("[soak] forced sky u_cloud_enabled=1 (fallback path)")
		await _shot("s4_fallback_forced_midair", 3500.0, -0.18)
		await _shot("s5_fallback_forced_orbit", 900000.0, -1.15)

	_finish()


func _process(_dt: float) -> void:
	_frames += 1
	if not _done and (_frames > 300000 or (Time.get_ticks_msec() - _wall0) > 600000):
		_p("[soak] failsafe quit"); _finish()


func _finish() -> void:
	if _done: return
	_done = true
	var f := FileAccess.open("%s/log.txt" % _out, FileAccess.WRITE)
	if f != null:
		f.store_string("\n".join(_log)); f.close()
	_p("[soak] wrote %s" % ProjectSettings.globalize_path(_out))
	get_tree().quit(0)


func _p(s: String) -> void:
	print(s)
	_log.append(s)


func _sky_material() -> ShaderMaterial:
	var env := main.get_viewport().world_3d.environment
	if env != null and env.sky != null and env.sky.sky_material is ShaderMaterial:
		return env.sky.sky_material
	return null


func _tex_stats(tex: Texture2D) -> Vector3:
	# returns (min, max, mean) of the R channel
	if tex == null: return Vector3(-1, -1, -1)
	var img: Image = tex.get_image()
	if img == null: return Vector3(-2, -2, -2)
	if img.is_compressed(): img.decompress()
	var w := img.get_width()
	var h := img.get_height()
	var step := maxi(1, int(sqrt(float(w * h) / 4096.0)))
	var mn := 1e9
	var mx := -1e9
	var acc := 0.0
	var n := 0
	for y in range(0, h, step):
		for x in range(0, w, step):
			var r := img.get_pixel(x, y).r
			mn = minf(mn, r); mx = maxf(mx, r); acc += r; n += 1
	return Vector3(mn, mx, acc / maxf(n, 1)) if n > 0 else Vector3(-3, -3, -3)


func _sample_coverage(tex: Texture2D, dir: Vector3) -> float:
	if tex == null: return -1.0
	var img: Image = tex.get_image()
	if img == null: return -2.0
	if img.is_compressed(): img.decompress()
	var d := dir.normalized()
	var u := 0.5 + atan2(d.z, d.x) / TAU
	var v := 0.5 - asin(clampf(d.y, -1.0, 1.0)) / PI
	var px := clampi(int(u * img.get_width()), 0, img.get_width() - 1)
	var py := clampi(int(v * img.get_height()), 0, img.get_height() - 1)
	return img.get_pixel(px, py).r


func _place_sun() -> void:
	var up := _site.normalized()
	var ref := Vector3(0, 1, 0)
	if absf(up.dot(ref)) > 0.995: ref = Vector3(1, 0, 0)
	var east := ref.cross(up).normalized()
	var north := up.cross(east).normalized()
	var e := deg_to_rad(_sun_elev)
	var a := deg_to_rad(_sun_az)
	Frames.helion_dir = (up * sin(e) + (north * cos(a) + east * sin(a)) * cos(e)).normalized()
	if main != null:
		main._sync_sun_direction(true)
		main._sync_solar_brightness(true)


func _shot(name: String, alt: float, pitch: float) -> void:
	var p = main.player
	var d := _site.normalized()
	var r: float = Planet.cfg.planet_radius + maxf(Planet.terrain_height(d), 0.0) + alt
	var look := -Frames.helion_dir
	p.world_pos = Vec3D.new(d.x * r, d.y * r, d.z * r)
	Frames.rebase(p.world_pos)
	_aim(p, look, pitch)
	p.vertical_speed = 0.0
	p._sync_transform()
	var deadline := Time.get_ticks_msec() + 14000
	while Time.get_ticks_msec() < deadline:
		p.world_pos = Vec3D.new(d.x * r, d.y * r, d.z * r)
		p.vertical_speed = 0.0
		_aim(p, look, pitch)
		p._sync_transform()
		await get_tree().process_frame
	for i in 48:
		await get_tree().process_frame
	await RenderingServer.frame_post_draw
	var img := get_viewport().get_texture().get_image()
	img.save_png("%s/%s.png" % [_out, name])
	var cov := _sample_coverage(WeatherSystem.global_weather_texture, d)
	_p("[soak] shot %s alt %.0f site_cov %.4f" % [name, alt, cov])


func _aim(p, target_dir: Vector3, pitch: float) -> void:
	var up: Vector3 = p.up_dir()
	var ref := Vector3(0, 1, 0)
	if absf(up.dot(ref)) > 0.995: ref = Vector3(1, 0, 0)
	var east := ref.cross(up).normalized()
	var north := up.cross(east).normalized()
	var t: Vector3 = target_dir - up * target_dir.dot(up)
	if t.length() > 1e-6:
		t = t.normalized()
		p.yaw = atan2(t.dot(east), t.dot(north))
	p.pitch = pitch
