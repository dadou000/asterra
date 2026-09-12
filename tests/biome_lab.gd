extends Node
## ASTERRA BIOME LAB -- boot Planet Studio once, drive its per-biome Terrain +
## Texture authoring (the phase47 editor tools), compose every land biome from
## real-life references, screenshot each, iterate via a command file.
##
## Uses ONLY the in-editor authoring API:
##   editor._phase47_ensure_biome_profile / _rebuild_biome_profile   (Biome Terrain tab)
##   editor._phase47_ensure_biome_texture_slot / _rebuild_biome_texture (Texture tab)
##   session.apply()
##
##   godot --path . res://tests/BiomeLab.tscn --quit-after 6000000 -- --res=1280x720
##
## cmd file:  user://biome_lab/cmd.json
## results:   user://biome_lab/out/<id>/*.png + result_<id>.json
## stop:      user://biome_lab/stop
##
## cmd.json:
## {
##   "id": 2,
##   "biomes": [7, 11, 16],                 // land biome ids to (re)author; omit = all land
##   "shots":  [7, 11, 16],                 // which to photograph; omit = same as biomes
##   "terrain": {"7": {"blend_km": 3, "layers": [{"type":"NOISE_LAYER","scale":5,"amount":22,"param":3,"seed":1}]}},
##   "texture": {"7": [{"texture_choice":4,"color":[0.22,0.33,0.17],"slope_max":34},
##                     {"texture_choice":1,"color":[0.34,0.28,0.2],"slope_min":32}]},
##   "sun": 58
## }
## Omitted biome -> ported terrain default + this harness's reference texture palette.

const LAB_ROOT := "user://biome_lab"
const LAND_BIOMES: Array[int] = [2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17]

var _res := Vector2i(1280, 720)
var _max_jobs := 60

var main: Node3D
var editor: Node            # PlanetStudioLive (phase46 editor Control)
var session: RefCounted     # WorldAuthoringSession
var _r := 1000000.0
var _wall0 := 0
var _done := false
var _jobs := 0
var _last_mtime := 0
var _cmd_path := ""
var _names: Array = []


func _ready() -> void:
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	_wall0 = Time.get_ticks_msec()
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--res="):
			var wh := a.trim_prefix("--res=").split("x", false)
			if wh.size() == 2: _res = Vector2i(int(wh[0]), int(wh[1]))
		elif a.begins_with("--max-jobs="):
			_max_jobs = int(a.trim_prefix("--max-jobs="))
	DisplayServer.window_set_size(_res)
	get_window().size = _res
	DirAccess.make_dir_recursive_absolute(LAB_ROOT + "/out")
	_cmd_path = ProjectSettings.globalize_path(LAB_ROOT + "/cmd.json")
	await get_tree().process_frame

	get_tree().set_meta("launch_mode", "planet_studio")
	_p("booting Main in planet_studio mode...")
	main = load("res://scenes/Main.tscn").instantiate()
	add_child(main)
	var dl := Time.get_ticks_msec() + 240000
	while not main._started and Time.get_ticks_msec() < dl:
		await get_tree().process_frame
	if not main._started:
		_p("FATAL: Main never started"); get_tree().quit(1); return

	# wait for the runtime host to open the live editor
	dl = Time.get_ticks_msec() + 120000
	while editor == null and Time.get_ticks_msec() < dl:
		editor = _find_editor()
		await get_tree().process_frame
	if editor == null:
		_p("FATAL: PlanetStudioLive editor never appeared"); get_tree().quit(1); return
	var sv: Variant = editor.get("_session")
	if not (sv is RefCounted):
		_p("FATAL: editor._session missing"); get_tree().quit(1); return
	session = sv
	_r = Planet.cfg.planet_radius
	_names = PlanetFields.BIOME_NAMES
	_p("editor + session bound. terrain profile = %s" % str(session.active_terrain_profile()))

	# quiet the player, freeze the orbital clock, wait for world caches
	if main.player != null:
		main.player.set("input_enabled", false)
		if main.player.has_method("set_mouse_captured"):
			main.player.call("set_mouse_captured", false)
	Frames.playing = false
	var otx := Time.get_ticks_msec() + 150000
	while Time.get_ticks_msec() < otx and Planet.orbit_texture_face_res < 700:
		await get_tree().process_frame
	_p("orbit_texture_face_res=%d  PlanetContext.ready=%s" % [
		Planet.orbit_texture_face_res, _pc_ready()])

	_p("READY. waiting for %s  (stop: %s/stop)" % [
		_cmd_path, ProjectSettings.globalize_path(LAB_ROOT)])
	await _watch()


func _watch() -> void:
	while not _done:
		if FileAccess.file_exists(ProjectSettings.globalize_path(LAB_ROOT + "/stop")):
			_p("stop file -- exit"); break
		if _jobs >= _max_jobs or (Time.get_ticks_msec() - _wall0) > 5500000:
			_p("limit -- exit"); break
		var mt := 0
		if FileAccess.file_exists(_cmd_path):
			mt = int(FileAccess.get_modified_time(_cmd_path))
		if mt > 0 and mt != _last_mtime:
			_last_mtime = mt
			await get_tree().create_timer(0.3).timeout
			await _run_job()
		await get_tree().create_timer(0.5).timeout
	_done = true
	get_tree().quit(0)


func _run_job() -> void:
	var parsed: Variant = JSON.parse_string(FileAccess.get_file_as_string(_cmd_path))
	if not (parsed is Dictionary):
		_p("bad cmd.json"); return
	var job: Dictionary = parsed
	var jid := int(job.get("id", _jobs + 1))
	_jobs += 1
	var t0 := Time.get_ticks_msec()
	_p("---- job %d ----" % jid)
	var out_dir := "%s/out/%d" % [LAB_ROOT, jid]
	DirAccess.make_dir_recursive_absolute(out_dir)

	var biomes: Array = job.get("biomes", [])
	var want: Array[int] = []
	if biomes.is_empty():
		want.assign(LAND_BIOMES)
	else:
		for b in biomes:
			want.append(int(b))
	var terr_ov: Dictionary = job.get("terrain", {})
	var tex_ov: Dictionary = job.get("texture", {})

	var terrain: Resource = session.active_terrain_profile()
	if terrain == null:
		_p("no terrain profile"); return

	for bid in want:
		_author_biome(terrain, bid, terr_ov.get(str(bid), null), tex_ov.get(str(bid), null))
	terrain.call("ensure_valid")
	session.call("_mark_dirty", WorldAuthoringSession.ApplyScope.GRAPH)
	session.call("apply")
	_p("  applied %d biome slot-sets, waiting for terrain reload..." % want.size())

	# let the renderer pick up the new authoring terrain + warm the geomorph cache
	for i in 260:
		await get_tree().process_frame

	var sun_elev: float = float(job.get("sun", 58.0))
	var shots: Array = job.get("shots", biomes)
	var shot_ids: Array[int] = []
	if shots.is_empty():
		shot_ids.assign(want)
	else:
		for s in shots:
			shot_ids.append(int(s))

	# extended warm-up on the first shot site so the clipmap streaming catches up
	if not shot_ids.is_empty():
		var s0 := _biome_site(int(shot_ids[0]))
		if s0 != Vector3.ZERO:
			await _fly_to(s0, 170.0, 600)

	var rec := {"id": jid, "biomes": want, "diag": {}}
	for bid in shot_ids:
		var site := _biome_site(bid)
		if site == Vector3.ZERO:
			_p("  biome %d (%s): no site" % [bid, _bname(bid)])
			continue
		_place_sun(site, sun_elev)
		await _fly_to(site, 170.0, 360)
		await _shot("%s/biome_%02d_%s" % [out_dir, bid, _bslug(bid)])
		await _fly_to(site, 3.0, 160)
		await _shot("%s/biome_%02d_%s_ground" % [out_dir, bid, _bslug(bid)])
		rec["diag"][str(bid)] = _biome_diag(terrain, bid)
		_p("  biome %d %s: %s" % [bid, _bname(bid), rec["diag"][str(bid)]])

	rec["secs"] = (Time.get_ticks_msec() - t0) / 1000.0
	var f := FileAccess.open("%s/result_%d.json" % [LAB_ROOT, jid], FileAccess.WRITE)
	if f != null: f.store_string(JSON.stringify(rec, "  ")); f.close()
	var f2 := FileAccess.open("%s/result.json" % out_dir, FileAccess.WRITE)
	if f2 != null: f2.store_string(JSON.stringify(rec, "  ")); f2.close()
	_p("---- job %d done %.0fs -> %s ----" % [jid, rec["secs"],
		ProjectSettings.globalize_path(out_dir)])


# ---- authoring -------------------------------------------------------
func _author_biome(terrain: Resource, bid: int, terr_override: Variant,
		tex_override: Variant) -> void:
	# Biome Terrain (displacement)
	var prof: Resource = editor.call("_phase47_ensure_biome_profile", terrain, bid)
	if prof != null:
		var stack: Dictionary
		if terr_override is Dictionary:
			stack = _norm_terrain_stack(terr_override)
		else:
			stack = editor.call("_phase47_biome_terrain_defaults", bid)
		editor.call("_phase47_rebuild_biome_profile", prof.get(&"graph"), stack)
	# Biome Texture (material)
	var tslot: Resource = editor.call("_phase47_ensure_biome_texture_slot", terrain, bid)
	if tslot != null:
		var layers: Array
		if tex_override is Array:
			layers = _norm_tex_layers(tex_override)
		else:
			layers = _tex_reference(bid)
		editor.call("_phase47_rebuild_biome_texture", tslot.get(&"graph"), layers)


func _norm_terrain_stack(d: Dictionary) -> Dictionary:
	var layers: Array = []
	for lv in d.get("layers", []):
		var l: Dictionary = lv
		layers.append({
			"type": String(l.get("type", "NOISE_LAYER")),
			"scale": float(l.get("scale", 5.0)),
			"amount": float(l.get("amount", 20.0)),
			"param": int(l.get("param", 3)),
			"seed": int(l.get("seed", 1337)),
			"angle_deg": float(l.get("angle_deg", 90.0)),
		})
	return {"blend_km": float(d.get("blend_km", 2.0)), "layers": layers}


func _norm_tex_layers(a: Array) -> Array:
	var out: Array = []
	for lv in a:
		var l: Dictionary = lv
		var c: Array = l.get("color", [0.4, 0.35, 0.28])
		var cb: Array = l.get("color_b", c)
		out.append({
			"texture_choice": int(l.get("texture_choice", 0)),
			"color": Color(c[0], c[1], c[2]),
			"color_b": Color(cb[0], cb[1], cb[2]),
			"gradient_strength": float(l.get("gradient_strength", 0.0)),
			"height_min": float(l.get("height_min", -1000000.0)),
			"height_max": float(l.get("height_max", 1000000.0)),
			"height_relative": bool(l.get("height_relative", false)),
			"slope_min": float(l.get("slope_min", 0.0)),
			"slope_max": float(l.get("slope_max", 180.0)),
			"softness": float(l.get("softness", 20.0)),
			"opacity": float(l.get("opacity", 1.0)),
			"noise_scale": float(l.get("noise_scale", 0.0)),
			"noise_strength": float(l.get("noise_strength", 0.0)),
			"tint_strength": float(l.get("tint_strength", 0.0)),
			"seed": int(l.get("seed", randi() % 900000)),
			"roughness_value": float(l.get("roughness_value", 0.9)),
			"roughness_enabled": bool(l.get("roughness_enabled", false)),
			"metallic_value": float(l.get("metallic_value", 0.0)),
			"metallic_enabled": bool(l.get("metallic_enabled", false)),
			"emission_color": Color(1.0, 0.6, 0.2),
			"emission_strength": 1.0, "emission_enabled": false,
			"anisotropy_value": 0.0, "anisotropy_enabled": false,
			"custom_texture_index": -1,
			"cavity_min": -1.0, "cavity_max": 1.0,
			"gradient_curve": CurveFieldData.identity(),
		})
	return out


## One flat/gradient colour band + a slope-exposed rock band, tuned per biome to a
## real-world reference. texture_choice: 0 flat, 1 Ground, 2 Grass, 3 Mud, 4 Forest.
func _tex_reference(bid: int) -> Array:
	var v := func(cc: Array, extra: Dictionary = {}) -> Dictionary:
		var base := {
			"texture_choice": 0, "color": Color(cc[0], cc[1], cc[2]),
			"color_b": Color(cc[0], cc[1], cc[2]), "gradient_strength": 0.0,
			"height_min": -1000000.0, "height_max": 1000000.0, "height_relative": false,
			"slope_min": 0.0, "slope_max": 180.0, "softness": 22.0, "opacity": 1.0,
			"noise_scale": 0.6, "noise_strength": 0.14, "tint_strength": 0.5,
			"seed": (bid * 733 + 17) % 900000,
			"roughness_value": 0.92, "roughness_enabled": true,
			"metallic_value": 0.0, "metallic_enabled": false,
			"anisotropy_value": 0.0, "anisotropy_enabled": false,
			"emission_color": Color(1, 0.6, 0.2), "emission_strength": 1.0, "emission_enabled": false,
			"custom_texture_index": -1, "cavity_min": -1.0, "cavity_max": 1.0,
			"gradient_curve": CurveFieldData.identity(),
		}
		for k in extra: base[k] = extra[k]
		return base
	var rock := func(cc: Array) -> Dictionary:
		return v.call(cc, {"texture_choice": 1, "slope_min": 33.0, "softness": 12.0,
			"noise_strength": 0.1, "roughness_value": 0.96})
	match bid:
		2:  return [v.call([0.86, 0.90, 0.95], {"roughness_value": 0.35}),
			v.call([0.72, 0.80, 0.90], {"slope_min": 40.0})]                       # ICE_CAP
		3:  return [v.call([0.40, 0.39, 0.31], {"texture_choice": 2}),
			v.call([0.30, 0.36, 0.27], {"slope_max": 20.0, "opacity": 0.6}),
			rock.call([0.46, 0.44, 0.40])]                                        # TUNDRA
		4:  return [v.call([0.17, 0.25, 0.18], {"texture_choice": 4}),
			v.call([0.30, 0.24, 0.17], {"slope_min": 26.0, "opacity": 0.7}),
			rock.call([0.40, 0.38, 0.35])]                                        # TAIGA
		5:  return [v.call([0.56, 0.51, 0.41], {"texture_choice": 1}),
			rock.call([0.50, 0.47, 0.42])]                                        # COLD_DESERT
		6:  return [v.call([0.45, 0.44, 0.24], {"texture_choice": 2,
			"color_b": Color(0.35, 0.42, 0.22), "gradient_strength": 0.5,
			"height_relative": true}),
			rock.call([0.44, 0.40, 0.33])]                                        # TEMPERATE_GRASSLAND
		7:  return [v.call([0.23, 0.33, 0.17], {"texture_choice": 4}),
			v.call([0.30, 0.24, 0.16], {"slope_min": 24.0, "opacity": 0.75}),
			rock.call([0.36, 0.33, 0.29])]                                        # TEMPERATE_FOREST
		8:  return [v.call([0.13, 0.27, 0.15], {"texture_choice": 4, "roughness_value": 0.8}),
			v.call([0.20, 0.24, 0.16], {"slope_min": 22.0, "opacity": 0.8}),
			rock.call([0.30, 0.32, 0.28])]                                        # TEMPERATE_RAINFOREST
		9:  return [v.call([0.40, 0.40, 0.26], {"texture_choice": 2}),
			v.call([0.66, 0.60, 0.48], {"slope_min": 26.0}),
			rock.call([0.70, 0.64, 0.52])]                                        # MEDITERRANEAN
		10: return [v.call([0.52, 0.46, 0.27], {"texture_choice": 2,
			"color_b": Color(0.44, 0.40, 0.24), "gradient_strength": 0.4}),
			rock.call([0.48, 0.44, 0.36])]                                        # STEPPE
		11: return [v.call([0.76, 0.61, 0.40], {"texture_choice": 1,
			"color_b": Color(0.82, 0.68, 0.46), "gradient_strength": 0.35,
			"height_relative": true, "roughness_value": 0.7}),
			v.call([0.58, 0.42, 0.28], {"slope_min": 30.0})]                      # HOT_DESERT
		12: return [v.call([0.58, 0.50, 0.26], {"texture_choice": 2}),
			v.call([0.50, 0.33, 0.22], {"slope_min": 20.0, "opacity": 0.7}),
			rock.call([0.52, 0.40, 0.30])]                                        # SAVANNA
		13: return [v.call([0.22, 0.34, 0.16], {"texture_choice": 4}),
			v.call([0.48, 0.28, 0.18], {"slope_min": 22.0, "opacity": 0.7}),
			rock.call([0.44, 0.30, 0.22])]                                        # TROPICAL_SEASONAL_FOREST
		14: return [v.call([0.11, 0.29, 0.14], {"texture_choice": 4, "roughness_value": 0.78}),
			v.call([0.18, 0.22, 0.14], {"slope_min": 20.0, "opacity": 0.82}),
			rock.call([0.30, 0.28, 0.22])]                                        # TROPICAL_RAINFOREST
		15: return [v.call([0.27, 0.30, 0.20], {"texture_choice": 3, "roughness_value": 0.6}),
			v.call([0.20, 0.26, 0.18], {"height_relative": true, "height_max": 2.0,
			"opacity": 0.8})]                                                     # WETLAND
		16: return [v.call([0.30, 0.38, 0.24], {"texture_choice": 2, "slope_max": 30.0}),
			rock.call([0.50, 0.50, 0.52]),
			v.call([0.88, 0.90, 0.94], {"height_relative": true, "height_min": 40.0,
			"softness": 40.0, "roughness_value": 0.4})]                           # ALPINE
		17: return [v.call([0.44, 0.42, 0.40], {"texture_choice": 1, "roughness_value": 0.97}),
			v.call([0.32, 0.30, 0.29], {"slope_min": 40.0})]                      # BARE_ROCK
		_:  return [v.call([0.35, 0.33, 0.26])]


# ---- camera / shot -------------------------------------------------
func _place_sun(site: Vector3, elev_deg: float) -> void:
	var t := _tangent(site)
	var up: Vector3 = t[0]
	var east: Vector3 = t[1]
	Frames.helion_dir = (up * sin(deg_to_rad(elev_deg))
		+ east * cos(deg_to_rad(elev_deg))).normalized()
	if main.has_method("_sync_sun_direction"): main._sync_sun_direction(true)
	if main.has_method("_sync_solar_brightness"): main._sync_solar_brightness(true)

func _fly_to(site: Vector3, alt: float, frames: int) -> void:
	var d := site.normalized()
	var rr: float = _r + maxf(Planet.terrain_height(d), 0.0) + alt
	var cam := Vec3D.new(d.x * rr, d.y * rr, d.z * rr)
	var t := _tangent(site)
	var east: Vector3 = t[1]
	var up: Vector3 = t[0]
	var look := (east * 0.55 - up * 0.62).normalized()
	var p = main.player
	for i in frames:
		p.world_pos = cam
		Frames.rebase(p.world_pos)
		var pu: Vector3 = p.up_dir()
		var ref := Vector3(0, 1, 0)
		if absf(pu.dot(ref)) > 0.995: ref = Vector3(1, 0, 0)
		var pe := ref.cross(pu).normalized()
		var pn := pu.cross(pe).normalized()
		var flat := look - pu * look.dot(pu)
		if flat.length() > 1e-6:
			flat = flat.normalized()
			p.yaw = atan2(flat.dot(pe), flat.dot(pn))
		p.pitch = clampf(asin(clampf(look.dot(pu), -1.0, 1.0)), -1.4, 1.4)
		if "vertical_speed" in p: p.vertical_speed = 0.0
		p._sync_transform()
		await get_tree().process_frame
	var vc := get_viewport().get_camera_3d()
	var pc: Object = p.get("camera")
	_p("    [cam] viewport_cam=%s  player_cam=%s  same=%s  vc_pos=%s" % [
		str(vc), str(pc), str(vc == pc),
		str(vc.global_position.snappedf(1.0)) if vc != null else "-"])
	var gc := get_node_or_null("/root/GroundGeometryClipmap")
	if gc != null:
		var st: Dictionary = gc.gpu_stream_stats() if gc.has_method("gpu_stream_stats") else {}
		_p("    [clip] active_levels=%s view_ring_instances=%s cache_ready=%s" % [
			str(st.get("active_levels")), str(st.get("view_ring_instances")),
			str(st.get("terrain_cache_ready"))])
	# lighting / environment snapshot
	var nlights := 0
	var stack2: Array[Node] = [main]
	while not stack2.is_empty():
		var n: Node = stack2.pop_back()
		if n is DirectionalLight3D:
			nlights += 1
			_p("    [light] DirLight '%s' energy=%.2f visible=%s dir=%s" % [
				n.name, (n as DirectionalLight3D).light_energy,
				(n as DirectionalLight3D).visible,
				str(-(n as DirectionalLight3D).global_transform.basis.z.snappedf(0.01))])
		elif n is WorldEnvironment and (n as WorldEnvironment).environment != null:
			var e := (n as WorldEnvironment).environment
			_p("    [env] bg=%d tonemap=%d exposure=%.2f ambient_energy=%.2f" % [
				e.background_mode, e.tonemap_mode, e.tonemap_exposure, e.ambient_light_energy])
		for c in n.get_children():
			stack2.append(c)
	_p("    [sun] helion=%s  playing=%s" % [str(Frames.helion_dir.snappedf(0.01)), str(Frames.playing)])
	var gq := get_node_or_null("/root/GraphicsQuality")
	if gq != null and gq.has_method("solar_irradiance"):
		_p("    [gq] solar_irradiance=%.3f" % gq.solar_irradiance())
	var gg := get_node_or_null("/root/GroundGeometryClipmap")
	if gg != null and gg.get("_material") is ShaderMaterial:
		var sm := gg.get("_material") as ShaderMaterial
		_p("    [tmat] u_sun_intensity=%s u_sun_dir=%s u_relief_ready=%s u_ctx_ready=%s u_material_global_ready=%s" % [
			str(sm.get_shader_parameter("u_sun_intensity")),
			str(sm.get_shader_parameter("u_sun_dir")),
			str(sm.get_shader_parameter("u_relief_ready")),
			str(sm.get_shader_parameter("u_ctx_ready")),
			str(sm.get_shader_parameter("u_material_global_ready"))])

func _shot(path_noext: String) -> void:
	await RenderingServer.frame_post_draw
	get_viewport().get_texture().get_image().save_png(path_noext + ".png")


# ---- helpers ------------------------------------------------------
func _first_descendant_named(root: Node, want: String) -> Node:
	var stack: Array[Node] = [root]
	while not stack.is_empty():
		var n: Node = stack.pop_back()
		if n != root and String(n.name) == want:
			return n
		for c in n.get_children():
			stack.append(c)
	return null

func _find_editor() -> Node:
	var stack: Array[Node] = [main]
	while not stack.is_empty():
		var n: Node = stack.pop_back()
		if n.name == "PlanetStudioLive":
			return n
		for c in n.get_children():
			stack.append(c)
	return null

func _pc_ready() -> bool:
	var pc := get_node_or_null("/root/PlanetContext")
	return pc != null and bool(pc.get("ready_state"))

func _bname(bid: int) -> String:
	return String(_names[bid]) if bid < _names.size() else "biome%d" % bid

func _bslug(bid: int) -> String:
	return _bname(bid).to_lower().replace(" ", "_")

func _tangent(d: Vector3) -> Array:
	var up := d.normalized()
	var ref := Vector3(0, 1, 0)
	if absf(up.dot(ref)) > 0.995: ref = Vector3(1, 0, 0)
	var east := ref.cross(up).normalized()
	var north := up.cross(east).normalized()
	return [up, east, north]

func _biome_site(bid: int) -> Vector3:
	var f := Planet.fields
	var g := Planet.grid
	var best := -1.0
	var best_c := -1
	for c in g.cell_count:
		if f.biome[c] != bid:
			continue
		var pure := 0
		for k in 8:
			if f.biome[g.nbr[c * 8 + k]] == bid:
				pure += 1
		var relief: float = f.relief[c] if "relief" in f else 0.0
		var score := float(pure) * 100.0 + minf(relief, 600.0) * 0.4
		if score > best:
			best = score
			best_c = c
	return g.cell_dir(best_c).normalized() if best_c >= 0 else Vector3.ZERO

func _biome_diag(_terrain: Resource, _bid: int) -> String:
	var g := get_node_or_null("/root/GroundGeometryClipmap")
	if g == null:
		return "no clipmap"
	var m: Variant = g.get("_material")
	if not (m is ShaderMaterial):
		return "no material"
	var sm := m as ShaderMaterial
	return "biome_layer_count=%s biome_blend_m=%s tex_layer_count=%s tex_custom_count=%s | author_disp_ready=%s ctx_ready=%s cache_ready=%s disp_fp=%s" % [
		str(sm.get_shader_parameter("u_biome_layer_count")),
		str(sm.get_shader_parameter("u_biome_blend_m")),
		str(sm.get_shader_parameter("u_biome_tex_layer_count")),
		str(sm.get_shader_parameter("u_biome_tex_custom_count")),
		str(sm.get_shader_parameter("u_author_disp_ready")),
		str(sm.get_shader_parameter("u_ctx_ready")),
		str(sm.get_shader_parameter("u_terrain_cache_ready")),
		str(g.get("_displacement_fingerprint")).substr(0, 24)]

func _p(s: String) -> void:
	print("[biome] " + s)

func _process(_dt: float) -> void:
	if not _done and (Time.get_ticks_msec() - _wall0) > 6000000:
		_done = true; get_tree().quit(0)
