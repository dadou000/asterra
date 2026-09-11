extends Node
## Terrain render diagnostic. Boots Main, drives a ground camera over a temperate
## forest with a high back-light sun, and dumps the terrain material / shader /
## lighting / LOD state so we can tell a shader-compile failure apart from a flat
## LOD mesh apart from a lighting problem.
##
##   godot --path . res://tests/TerrainProbe.tscn --quit-after 400000 -- --res=1280x720

var _res := Vector2i(1280, 720)
var main: Node3D
var _r := 1000000.0


func _ready() -> void:
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--res="):
			var wh := a.trim_prefix("--res=").split("x", false)
			if wh.size() == 2: _res = Vector2i(int(wh[0]), int(wh[1]))
	DisplayServer.window_set_size(_res)
	get_window().size = _res
	await get_tree().process_frame

	var out := "user://terrain_probe/%d" % Time.get_unix_time_from_system()
	DirAccess.make_dir_recursive_absolute(out)

	_p("booting Main")
	main = load("res://scenes/Main.tscn").instantiate()
	add_child(main)
	var dl := Time.get_ticks_msec() + 220000
	while not main._started and Time.get_ticks_msec() < dl:
		await get_tree().process_frame
	if not main._started:
		_p("FATAL Main never started"); get_tree().quit(1); return
	main.player.set_mouse_captured(false)
	main.player.input_enabled = false
	main.hud.visible = false
	main.map.visible = false
	main._game_orbit = null
	Frames.playing = false   # stop the orbital clock re-deriving the sun each frame
	_r = Planet.cfg.planet_radius

	var otx := Time.get_ticks_msec() + 150000
	while Time.get_ticks_msec() < otx and Planet.orbit_texture_face_res < 700:
		await get_tree().process_frame
	_p("orbit_texture_face_res=%d" % Planet.orbit_texture_face_res)

	# site: temperate forest
	var site := BiomeSites.find("temperate_forest")
	if site == Vector3.ZERO:
		site = Vector3(1, 0.2, 0.1).normalized()
	site = site.normalized()
	var elev: float = Planet.terrain_height(site)
	_p("site elev %.1f m  has_water=%s" % [elev, Planet.has_water(site)])

	# terrain relief in the data (is there anything to see?)
	var t := _tangent(site)
	var east: Vector3 = t[1]
	var line := "terrain_height around site (m):"
	for s: float in [-400.0, -200.0, -50.0, 0.0, 50.0, 200.0, 400.0]:
		var d: Vector3 = (site + east * (s / _r)).normalized()
		line += "  %+.0f=%.1f" % [s, Planet.terrain_height(d)]
	_p(line)
	_p("macro_height at site = %.1f   pristine = %.1f"
		% [Planet.macro_height(site), Planet.pristine_height(site) if Planet.has_method("pristine_height") else NAN])

	# sun: near-noon, high above the site so terrain is fully lit and colour is
	# read at minimal atmospheric tint
	var up: Vector3 = t[0]
	Frames.helion_dir = (up * sin(deg_to_rad(72.0)) + east * cos(deg_to_rad(72.0))).normalized()
	if main.has_method("_sync_sun_direction"): main._sync_sun_direction(true)
	if main.has_method("_sync_solar_brightness"): main._sync_solar_brightness(true)
	_p("sun elevation at site = %.1f deg   helion=%s" % [
		rad_to_deg(asin(clampf(Frames.helion_dir.dot(up), -1.0, 1.0))), str(Frames.helion_dir)])
	# aerial perspective can wash the near surface toward a hazy tint -- probe both
	var ground: Node = get_node_or_null("/root/GroundGeometryClipmap")
	if ground != null and ground.get("_material") is ShaderMaterial:
		var gm := ground.get("_material") as ShaderMaterial
		_p("terrain shader params: u_terrain_aerial_strength=%s u_sun_intensity=%s u_relief_ready=%s" % [
			str(gm.get_shader_parameter("u_terrain_aerial_strength")),
			str(gm.get_shader_parameter("u_sun_intensity")),
			str(gm.get_shader_parameter("u_relief_ready"))])

	# ---- position the camera FIRST, hold a long time so the terrain cache can
	#      anchor + dispatch, THEN dump state and shoot ----
	var cam_a := _cam(site, 120.0)
	_place_cam(cam_a, east)
	_p("holding camera for cache warm-up...")
	for i in 600:
		_place_cam(cam_a, east)
		await get_tree().process_frame
		if i == 120 or i == 300 or i == 599:
			_p("--- warm frame %d ---" % i)
			_dump_terrain_state()

	_dump_lighting_state()
	_dump_planet_context()

	await _shot("%s/lowair_forward" % out, cam_a, east)
	await _shot("%s/lowair_down45" % out, _cam(site, 220.0), (east - up).normalized())
	await _shot("%s/ground_forward" % out, _cam(site, 3.0), east)
	_p("wrote %s" % ProjectSettings.globalize_path(out))
	get_tree().quit(0)


func _dump_planet_context() -> void:
	var pc: Node = get_node_or_null("/root/PlanetContext")
	if pc == null:
		_p("PlanetContext: NOT FOUND"); return
	_p("PlanetContext.ready_state=%s generation=%s" % [pc.get("ready_state"), pc.get("generation")])
	for tn in ["soil_texture", "surface_texture", "geology_texture", "structure_texture",
			"climate_texture", "hydrology_texture", "rock_texture", "biome_texture"]:
		var t: Variant = pc.get(tn)
		_p("  %s = %s" % [tn, "Texture2DArray(%dx%d x%d)" % [
			(t as Texture2DArray).get_width(), (t as Texture2DArray).get_height(),
			(t as Texture2DArray).get_layers()] if t is Texture2DArray else str(t)])
	_p("Planet.global_height_texture = %s  global_height_face_res=%s" % [
		Planet.global_height_texture, Planet.get("global_height_face_res")])
	_p("Planet.global_material_texture = %s  global_material_stats=%s" % [
		str(Planet.get("global_material_texture")),
		JSON.stringify(Planet.global_material_stats()) if Planet.has_method("global_material_stats") else "<no method>"])
	var mc: Node = get_node_or_null("/root/MaterialClipmap")
	_p("MaterialClipmap node = %s  global_texture()=%s" % [
		str(mc), str(mc.global_texture()) if mc != null and mc.has_method("global_texture") else "<n/a>"])
	var scat: Node = get_node_or_null("/root/TerrainScatter")
	if scat == null:
		_p("TerrainScatter: NOT FOUND")
	else:
		_p("TerrainScatter node script = %s" % (scat.get_script().resource_path if scat.get_script() else "?"))
		if scat.has_method("gpu_scatter_stats"):
			_p("TerrainScatter.gpu_scatter_stats = %s" % JSON.stringify(scat.call("gpu_scatter_stats")))
		if scat.has_method("scatter_stats"):
			_p("TerrainScatter.scatter_stats = %s" % JSON.stringify(scat.call("scatter_stats")))
		for prop in ["_have_anchor", "_debug_enabled", "_bound_macro", "_bound_context_generation",
				"_bound_terrain_cache_ready", "_bound_terrain_cache_generation"]:
			var v: Variant = scat.get(prop)
			_p("TerrainScatter.%s = %s" % [prop, str(v)])
		for bname in ["_grass_batch", "_geo_stone_batch", "_river_stone_batch"]:
			var b: Variant = scat.get(bname)
			if b is MultiMeshInstance3D:
				var mmi := b as MultiMeshInstance3D
				var mm := mmi.multimesh
				_p("TerrainScatter.%s: visible=%s instance_count=%s in_tree=%s" % [
					bname, str(mmi.visible), str(mm.instance_count if mm != null else -1),
					str(mmi.is_inside_tree())])
				if mmi.material_override is ShaderMaterial:
					var sm := mmi.material_override as ShaderMaterial
					_p("  shader params: u_scatter_terrain_cache_ready=%s u_ctx_ready=%s u_scatter_enabled=%s u_scatter_author_enabled=%s u_scatter_macro_ready=%s" % [
						str(sm.get_shader_parameter("u_scatter_terrain_cache_ready")),
						str(sm.get_shader_parameter("u_ctx_ready")),
						str(sm.get_shader_parameter("u_scatter_enabled")),
						str(sm.get_shader_parameter("u_scatter_author_enabled")),
						str(sm.get_shader_parameter("u_scatter_macro_ready"))])
			else:
				_p("TerrainScatter.%s = %s (not a MultiMeshInstance3D)" % [bname, str(b)])
	var terrain: Node = get_node_or_null("/root/GroundGeometryClipmap")
	if terrain != null:
		var tca: Variant = terrain.get("_terrain_cache_active")
		_p("_terrain_cache_active = %s" % str(tca))
		if tca is Node:
			var n := tca as Node
			for m in ["cache_ready", "texture", "anchor_generation"]:
				if n.has_method(m):
					_p("  %s() = %s" % [m, str(n.call(m))])
			for pr in ["_bindings_ready", "_bindings_building", "_jobs_dispatched",
					"_samples_dispatched", "_binding_generation"]:
				var v: Variant = n.get(pr)
				if v != null:
					_p("  %s = %s" % [pr, str(v)])
		var bcr: Variant = terrain.get("_bound_cache_ready")
		_p("_bound_cache_ready = %s  _concurrent_far_only=%s" % [
			str(bcr), str(terrain.get("_concurrent_far_only"))])
		if terrain.get("_material") is ShaderMaterial:
			var sm := terrain.get("_material") as ShaderMaterial
			_p("u_terrain_cache_ready param = %s" % str(sm.get_shader_parameter("u_terrain_cache_ready")))
			_p("u_height_enabled param = %s   u_material_clipmap_ready = %s" % [
				str(sm.get_shader_parameter("u_height_enabled")),
				str(sm.get_shader_parameter("u_material_clipmap_ready"))])


func _dump_terrain_state() -> void:
	var terrain: Node = get_node_or_null("/root/GroundGeometryClipmap")
	if terrain == null:
		_p("TERRAIN: /root/GroundGeometryClipmap NOT FOUND"); return
	_p("TERRAIN node: %s  script=%s" % [terrain.name, terrain.get_script().resource_path if terrain.get_script() else "?"])
	var mat: Variant = terrain.get("_material")
	if mat is ShaderMaterial:
		var sm := mat as ShaderMaterial
		var sh := sm.shader
		_p("TERRAIN material shader = %s" % (sh.resource_path if sh != null else "<null>"))
		if sh != null:
			var code_len := sh.code.length()
			_p("TERRAIN shader code length = %d  (rid valid=%s)"
				% [code_len, sh.get_rid().is_valid()])
	else:
		_p("TERRAIN _material is not a ShaderMaterial: %s" % str(mat))
	# child mesh instances + visibility
	var mesh_count := 0
	var vis_count := 0
	var err_mat := 0
	for c in _all_descendants(terrain):
		if c is MeshInstance3D or c is MultiMeshInstance3D:
			mesh_count += 1
			if (c as GeometryInstance3D).visible:
				vis_count += 1
			var ov: Variant = (c as GeometryInstance3D).material_override
			if ov is ShaderMaterial and (ov as ShaderMaterial).shader == null:
				err_mat += 1
	_p("TERRAIN mesh instances = %d  visible = %d  null-shader-override = %d"
		% [mesh_count, vis_count, err_mat])
	if terrain.has_method("gpu_stream_stats"):
		_p("TERRAIN gpu_stream_stats = %s" % JSON.stringify(terrain.gpu_stream_stats()))
	var scat: Node = get_node_or_null("/root/TerrainScatter")
	if scat != null:
		var pc: Node = get_node_or_null("/root/PlanetContext")
		_p("SCATTER frame check: PlanetContext.ready_state=%s generation=%s  TerrainScatter._bound_context_generation=%s _have_anchor=%s _bound_macro_valid=%s" % [
			str(pc.get("ready_state") if pc != null else "<no ctx>"),
			str(pc.get("generation") if pc != null else "?"),
			str(scat.get("_bound_context_generation")), str(scat.get("_have_anchor")),
			str(scat.get("_bound_macro") != null)])
		for bname in ["_grass_batch"]:
			var b: Variant = scat.get(bname)
			if b is MultiMeshInstance3D:
				_p("  %s.visible = %s" % [bname, str((b as MultiMeshInstance3D).visible)])
	for prop in ["_active_min_level", "_active_max_level", "_screen_space_raw_level",
			"_displacement_guard_m", "_displacement_guard_fallback", "_height_enabled"]:
		var v: Variant = terrain.get(prop)
		if v != null:
			_p("TERRAIN %s = %s" % [prop, str(v)])
	# GroundHeightPageAtlas readiness
	var pa: Node = get_node_or_null("/root/GroundHeightPageAtlas")
	if pa != null and pa.has_method("stats"):
		_p("GroundHeightPageAtlas.stats = %s" % JSON.stringify(pa.stats()))


func _dump_lighting_state() -> void:
	_p("Frames.helion_dir = %s" % str(Frames.helion_dir))
	if Engine.has_singleton("GraphicsQuality") or get_node_or_null("/root/GraphicsQuality") != null:
		var gq: Node = get_node_or_null("/root/GraphicsQuality")
		if gq != null and gq.has_method("solar_irradiance"):
			_p("GraphicsQuality.solar_irradiance() = %.3f" % gq.solar_irradiance())
	var lights := 0
	for n in _all_descendants(main):
		if n is DirectionalLight3D:
			var dl := n as DirectionalLight3D
			lights += 1
			_p("DirectionalLight3D '%s' energy=%.3f visible=%s temp=%s color=%s"
				% [dl.name, dl.light_energy, dl.visible,
					dl.light_temperature if "light_temperature" in dl else "?", dl.light_color])
		elif n is WorldEnvironment:
			var env := (n as WorldEnvironment).environment
			if env != null:
				_p("WorldEnvironment: bg_mode=%d tonemap=%d exposure=%.3f ambient_src=%d ambient_energy=%.3f sky_contrib=%.3f"
					% [env.background_mode, env.tonemap_mode, env.tonemap_exposure,
						env.ambient_light_source, env.ambient_light_energy,
						env.ambient_light_sky_contribution])
	_p("DirectionalLight3D count = %d" % lights)
	var cam := get_viewport().get_camera_3d()
	if cam != null and cam.attributes is CameraAttributesPractical:
		var ca := cam.attributes as CameraAttributesPractical
		_p("Camera attributes: auto_exposure=%s min=%.3f max=%.3f"
			% [ca.auto_exposure_enabled, ca.auto_exposure_min_sensitivity, ca.auto_exposure_max_sensitivity])
	elif cam != null:
		_p("Camera attributes = %s" % str(cam.attributes))


func _all_descendants(n: Node) -> Array:
	var out: Array = []
	for c in n.get_children():
		out.append(c)
		out.append_array(_all_descendants(c))
	return out

func _tangent(d: Vector3) -> Array:
	var up := d.normalized()
	var ref := Vector3(0, 1, 0)
	if absf(up.dot(ref)) > 0.995: ref = Vector3(1, 0, 0)
	var east := ref.cross(up).normalized()
	var north := up.cross(east).normalized()
	return [up, east, north]

func _cam(dir: Vector3, alt: float) -> Vec3D:
	var d := dir.normalized()
	var rr: float = _r + maxf(Planet.terrain_height(d), 0.0) + alt
	return Vec3D.new(d.x * rr, d.y * rr, d.z * rr)

func _place_cam(cam: Vec3D, look_dir: Vector3) -> void:
	var p = main.player
	p.world_pos = cam
	Frames.rebase(p.world_pos)
	var up: Vector3 = p.up_dir()
	var ref := Vector3(0, 1, 0)
	if absf(up.dot(ref)) > 0.995: ref = Vector3(1, 0, 0)
	var east := ref.cross(up).normalized()
	var north := up.cross(east).normalized()
	var flat := look_dir - up * look_dir.dot(up)
	if flat.length() > 1e-6:
		flat = flat.normalized()
		p.yaw = atan2(flat.dot(east), flat.dot(north))
	p.pitch = clampf(asin(clampf(look_dir.normalized().dot(up), -1.0, 1.0)), -1.5, 1.5)
	p.vertical_speed = 0.0
	p._sync_transform()

func _shot(path_noext: String, cam: Vec3D, look_dir: Vector3) -> void:
	for i in 90:
		_place_cam(cam, look_dir)
		await get_tree().process_frame
	await RenderingServer.frame_post_draw
	get_viewport().get_texture().get_image().save_png(path_noext + ".png")
	_p("  wrote %s.png" % path_noext.get_file())

func _settle(n: int) -> void:
	for i in n:
		await get_tree().process_frame

func _p(s: String) -> void:
	print("[probe] " + s)
