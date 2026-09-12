extends Node
## ASTERRA GAUNTLET -- composed-terrain river-to-coast test.
##
## 1. Finds a shoreline land cell with a steep hinterland and walks ~1.2 km
##    inland to a headwater point.
## 2. Bakes a real hydraulic-erosion pass (ErosionRegionBake -> Deltas) over the
##    headwater->coast corridor so the terrain actually carries a carved valley.
## 3. Drops a production point water source at the headwater and time-accelerates
##    the sparse solver.
## 4. Photographs the valley + the river mouth and logs whether the wetted-tile
##    set tracks the carved channel down to sea level.
##
##   godot --path . res://tests/GauntletRiverCoast.tscn --quit-after 1800000 -- \
##       --q=45 --timescale=25 --sim-s=360 --carve-radius=900 --droplets=70000 --res=1280x720
##
## Output: user://gauntlet_river_coast/<ts>/*.png + metrics.json

const OUT_ROOT := "user://gauntlet_river_coast"
const SOURCE_ID := "gauntlet_river"

var _q := 45.0
var _timescale := 25.0
var _sim_s := 360.0
var _res := Vector2i(1280, 720)
var _carve_radius := 600.0
var _droplets := 70000
var _inland_m := 950.0
var _source_frac := 0.0          # 0 = at the headwater; >0 = that far down the carved corridor
var _sun_elev := 52.0
var _gaussian_probe := false

var main: Node3D
var _head := Vector3.ZERO          # headwater point
var _src := Vector3.ZERO           # actual water-source point (headwater or down-corridor)
var _sea := Vector3.ZERO           # unit tangent at _head pointing at the coast
var _shore := Vector3.ZERO         # shoreline cell direction (elev ~ 0)
var _head_sea_m := 1500.0          # great-circle metres head -> shore
var _out := ""
var _m := {}
var _frames := 0
var _wall0 := 0
var _done := false
var _r := 1000000.0
var _cand_logged := false


func _ready() -> void:
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	_wall0 = Time.get_ticks_msec()
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--q="): _q = float(a.trim_prefix("--q="))
		elif a.begins_with("--timescale="): _timescale = float(a.trim_prefix("--timescale="))
		elif a.begins_with("--sim-s="): _sim_s = float(a.trim_prefix("--sim-s="))
		elif a.begins_with("--carve-radius="): _carve_radius = float(a.trim_prefix("--carve-radius="))
		elif a.begins_with("--droplets="): _droplets = int(a.trim_prefix("--droplets="))
		elif a.begins_with("--inland="): _inland_m = float(a.trim_prefix("--inland="))
		elif a.begins_with("--source-frac="): _source_frac = float(a.trim_prefix("--source-frac="))
		elif a.begins_with("--sun="): _sun_elev = float(a.trim_prefix("--sun="))
		elif a == "--gaussian-probe": _gaussian_probe = true
		elif a.begins_with("--res="):
			var wh := a.trim_prefix("--res=").split("x", false)
			if wh.size() == 2: _res = Vector2i(int(wh[0]), int(wh[1]))

	DisplayServer.window_set_size(_res)
	DisplayServer.window_set_position(Vector2i(0, 0))
	get_window().size = _res
	get_window().content_scale_size = _res
	await get_tree().process_frame

	_out = "%s/%d" % [OUT_ROOT, Time.get_unix_time_from_system()]
	DirAccess.make_dir_recursive_absolute(_out)
	_m = {"q_m3_s": _q, "timescale": _timescale, "sim_s": _sim_s,
		"carve_radius_m": _carve_radius, "droplets": _droplets, "errors": [], "samples": []}

	_p("[rc] booting Main")
	main = load("res://scenes/Main.tscn").instantiate()
	add_child(main)
	var dl := Time.get_ticks_msec() + 200000
	while not main._started and Time.get_ticks_msec() < dl:
		await get_tree().process_frame
	if not main._started:
		_fail("Main never started"); return
	main.player.set_mouse_captured(false)
	main.player.input_enabled = false
	main.player.vertical_speed = 0.0
	main.hud.visible = false
	main.map.visible = false
	main._game_orbit = null
	_r = Planet.cfg.planet_radius

	# distant terrain shape comes from the orbit-elevation texture; photographing
	# before it is built gives a flat/!lit read (see memory: orbit-elevation-...)
	var otx := Time.get_ticks_msec() + 150000
	while Time.get_ticks_msec() < otx and Planet.orbit_texture_face_res < 700:
		await get_tree().process_frame
	_p("[rc] orbit_texture_face_res=%d" % Planet.orbit_texture_face_res)

	# --- 1. locate a coastal headwater --------------------------------------
	var coast := _find_coastal_headwater()
	if coast.is_empty():
		_fail("no coastal headwater found"); return
	_head = (coast["site"] as Vector3).normalized()
	_sea = (coast["sea_dir"] as Vector3).normalized()
	_shore = (coast["shore"] as Vector3).normalized()
	_head_sea_m = float(coast["head_sea_m"])
	_p("[rc] headwater elev %.0f m   shore elev %.0f m   head->shore %.0f m" % [
		Planet.terrain_height(_head), Planet.terrain_height(_shore), _head_sea_m])
	_p("[rc] pre-carve profile: %s" % JSON.stringify(_corridor_profile()))
	_src = _head if _source_frac <= 0.001 else _corr_point(clampf(_source_frac, 0.0, 0.9))

	if _gaussian_probe:
		await _run_gaussian_probe()
		return

	# --- 2. carve the valley (real hydraulic erosion -> Deltas) ------------
	_carve_valley()
	_p("[rc] post-carve profile: %s" % JSON.stringify(_corridor_profile()))
	_m["corridor_profile_post_carve"] = _corridor_profile()

	# let the render mirror + clipmap catch the new Deltas
	for i in 90:
		await get_tree().process_frame

	# sun high and from behind the headwater, so the seaward valley-oblique view
	# and the straight-down mouth view are both front-lit (a low seaward sun just
	# silhouettes the land toward the camera)
	_place_sun(_sun_elev)

	_m["site"] = {
		"head": [_head.x, _head.y, _head.z], "sea": [_sea.x, _sea.y, _sea.z],
		"head_elev_m": Planet.terrain_height(_head),
		"shore_elev_m": Planet.terrain_height(_shore),
		"head_sea_m": _head_sea_m,
	}

	# --- 3. wire the sparse solver + visible surface ----------------------
	if WaterSystem.has_method("set_automatic_physical_hydrolod_focus_direction"):
		WaterSystem.set_automatic_physical_hydrolod_focus_direction(_src)
	if WaterSystem.has_method("set_dynamic_surface_anchor_direction"):
		WaterSystem.set_dynamic_surface_anchor_direction(_mid_corridor())
	if WaterSystem.has_method("set_dynamic_surface_render_enabled"):
		WaterSystem.set_dynamic_surface_render_enabled(true)
	if LocalWaterSurface and LocalWaterSurface.has_method("set_render_enabled"):
		LocalWaterSurface.set_render_enabled(true)
	var rt: Object = WaterSystem.sparse_runtime() if WaterSystem.has_method("sparse_runtime") else null
	if rt != null:
		rt.set("time_scale", _timescale)
		if rt.get("max_time_debt_s") != null:
			rt.set("max_time_debt_s", maxf(float(rt.get("max_time_debt_s")), _timescale * 0.2))
		if rt.get("max_gpu_substeps") != null:
			rt.set("max_gpu_substeps", 64)

	var rt_dl := Time.get_ticks_msec() + 150000
	while Time.get_ticks_msec() < rt_dl and String(WaterSystem.sparse_runtime_state()) != "ready":
		await get_tree().process_frame
	_p("[rc] sparse_runtime_state=%s" % String(WaterSystem.sparse_runtime_state()))

	var rt_for_cache: Object = WaterSystem.sparse_runtime()
	if rt_for_cache != null and SparseHydroSurfaceCache != null \
			and rt_for_cache.has_signal("cycle_completed") \
			and rt_for_cache.cycle_completed.is_connected(
				SparseHydroSurfaceCache._on_runtime_cycle_completed):
		rt_for_cache.cycle_completed.disconnect(
			SparseHydroSurfaceCache._on_runtime_cycle_completed)
		_p("[rc] detached surface-cache ocean re-anchor")

	var srt: Object = WaterSystem.sparse_runtime() if WaterSystem.has_method("sparse_runtime") else null
	if srt != null and srt.has_signal("cycle_completed"):
		srt.cycle_completed.connect(func(cid, report):
			if cid % 400 == 0:
				var reach: Dictionary = WaterSystem.gpu_stats().get("sparse_reachability", {})
				_p("[rc] cyc %d activity=%s reach{eval %s acc %s blk %s}" % [
					cid, str(report.get("activity_tiles", "?")),
					str(reach.get("evaluations", "?")), str(reach.get("accepted", "?")),
					str(reach.get("blocked", "?"))]))
	if srt != null:
		var fr: Object = srt.get("frontier")
		if fr != null and fr.has_signal("candidates_ready"):
			fr.candidates_ready.connect(func(_rid, cands, ovf):
				if not _cand_logged and (cands as Array).size() > 0:
					_cand_logged = true
					_p("[rc] first frontier candidate: %s ovf=%s" % [JSON.stringify(cands), ovf]))

	# --- shot T0: dry carved valley --------------------------------------
	await _cache_settle(30)
	_p("[rc] waterline elev %.0f m at %s" % [Planet.terrain_height(_waterline()),
		JSON.stringify(_corridor_profile())])
	await _shot("t0_valley_oblique", _cam_valley_oblique(), _look(_cam_valley_oblique(), _waterline()))
	await _shot("t0_mouth_over", _cam_mouth_over(), _look(_cam_mouth_over(), _waterline()))

	# --- 4. inject the source -----------------------------------------
	_p("[rc] source at frac %.2f  elev %.0f m" % [_source_frac, Planet.terrain_height(_src)])
	var err := WaterSystem.upsert_point_water_source(SOURCE_ID, _src, _q, Vector3.ZERO, -1, true)
	_p("[rc] upsert -> %d, count=%d" % [int(err), WaterSystem.point_water_source_count()])

	var wall_total := maxf(_sim_s, 30.0)
	var shots := 6
	var shot_i := 1
	while shot_i <= shots and not _done:
		var seg_end := Time.get_ticks_msec() + int(wall_total / float(shots) * 1000.0)
		while Time.get_ticks_msec() < seg_end and not _done:
			await get_tree().process_frame
			if SparseHydroSurfaceCache and SparseHydroSurfaceCache.has_method("request_update"):
				SparseHydroSurfaceCache.request_update()
		await _cache_settle(16)
		var cyc := _hydro_cycles()
		await _shot("t%d_valley_oblique" % shot_i, _cam_valley_oblique(),
			_look(_cam_valley_oblique(), _waterline()))
		await _shot("t%d_mouth_over" % shot_i, _cam_mouth_over(), _look(_cam_mouth_over(), _waterline()))
		await _shot("t%d_mouth_oblique" % shot_i, _cam_mouth_oblique(),
			_look(_cam_mouth_oblique(), _waterline()))
		var geom := _flow_geom()
		_m["samples"].append({
			"shot": shot_i, "hydro_cycles": cyc, "tiles": _tile_count(),
			"diag": _volume_hint(), "flow_geom": geom,
		})
		_p("[rc] shot %d cyc=%d tiles=%s flow=%s" % [
			shot_i, cyc, str(_tile_count()), JSON.stringify(geom)])
		shot_i += 1

	_finish()


# ---- coastal siting -----------------------------------------------------
func _tangent(d: Vector3) -> Array:
	var up := d.normalized()
	var ref := Vector3(0, 1, 0)
	if absf(up.dot(ref)) > 0.995: ref = Vector3(1, 0, 0)
	var east := ref.cross(up).normalized()
	var north := up.cross(east).normalized()
	return [up, east, north]

func _steepest(d: Vector3, ascend: bool) -> Vector3:
	var t := _tangent(d)
	var east: Vector3 = t[1]
	var north: Vector3 = t[2]
	var h0 := Planet.terrain_height(d)
	var best_az := 0.0
	var best := -1e30
	for i in 72:
		var az := TAU * float(i) / 72.0
		var probe := (d + (east * sin(az) + north * cos(az)) * (300.0 / _r)).normalized()
		var dh := Planet.terrain_height(probe) - h0
		var v := dh if ascend else -dh
		if v > best:
			best = v
			best_az = az
	return (east * sin(best_az) + north * cos(best_az)).normalized()

## A shoreline land cell with a steep hinterland; then walk inland to a headwater.
func _find_coastal_headwater() -> Dictionary:
	var f := Planet.fields
	var g := Planet.grid
	var best := -1.0e30
	var best_c := -1
	for c in g.cell_count:
		if f.elev[c] <= 3.0 or f.elev[c] > 320.0:
			continue
		if f.biome[c] == PlanetFields.Biome.ICE_CAP:
			continue
		var touches_sea := false
		for k in 8:
			var nb: int = g.nbr[c * 8 + k]
			if f.elev[nb] <= 0.0:
				touches_sea = true
				break
		if not touches_sea:
			continue
		var score: float = minf(f.relief[c], 1200.0) \
			- f.wetland[c] * 450.0 - f.floodplain[c] * 300.0
		if score > best:
			best = score
			best_c = c
	if best_c < 0:
		return {}

	var shore: Vector3 = g.cell_dir(best_c).normalized()
	# walk inland (steepest ascent) to the headwater
	var d := shore
	for _i in 12:
		var up_dir := _steepest(d, true)
		var nxt := (d + up_dir * (_inland_m / 6.0 / _r)).normalized()
		if Planet.terrain_height(nxt) <= Planet.terrain_height(d) + 0.5:
			break
		d = nxt
		if Planet.terrain_height(d) > 300.0:
			break
	var head := d
	var to_sea := _steepest(head, false)
	var arc: float = acos(clampf(head.dot(shore), -1.0, 1.0)) * _r
	return {"site": head, "sea_dir": to_sea, "shore": shore, "head_sea_m": maxf(arc, 300.0)}

func _mid_corridor() -> Vector3:
	return (_head + _sea * (_head_sea_m * 0.5 / _r)).normalized()

func _corr_point(frac: float) -> Vector3:
	return (_head + _sea * (_head_sea_m * frac / _r)).normalized()

func _corridor_profile() -> Array:
	var out: Array = []
	for k in 11:
		var frac := float(k) / 10.0
		var p := _corr_point(frac)
		out.append({
			"frac": frac, "dist_m": roundi(_head_sea_m * frac),
			"h": roundi(Planet.terrain_height(p)), "wet": Planet.has_water(p),
		})
	return out

# ---- valley carve -----------------------------------------------------
## Three overlapping hydraulic-erosion bakes down the corridor: a broad gentle
## valley-shaping pass, then two tighter incising passes that cut a continuous
## thalweg from the upper corridor down through the shoreline.
func _carve_valley() -> void:
	var radius := maxf(_carve_radius, 300.0)
	var gentle := {"droplet_count": 26000, "erode_radius": 3, "inertia": 0.08}
	var incise := {"droplet_count": maxi(_droplets, 40000), "erode_radius": 2, "inertia": 0.05}
	# incising passes stay in the UPPER half of the corridor; the last stretch to
	# the shore is left as a natural gentle slope so the channel does not carve a
	# below-sea-level pit that traps the flow short of the waterline
	var passes := [
		{"c": _mid_corridor(), "r": radius, "hard": 0.55, "p": gentle, "tag": "valley"},
		{"c": _corr_point(0.35), "r": radius * 0.6, "hard": 0.5, "p": incise, "tag": "upper_channel"},
		{"c": _corr_point(0.50), "r": radius * 0.5, "hard": 0.5, "p": incise, "tag": "mid_channel"},
	]
	_m["carve_bakes"] = []
	for pass_def in passes:
		var t0 := Time.get_ticks_msec()
		var rr: float = pass_def["r"]
		var res := clampi(int(rr / 4.5), 96, 260)
		var st: Dictionary = ErosionRegionBake.bake(
			pass_def["c"], rr, res, pass_def["hard"], pass_def["p"])
		st["tag"] = pass_def["tag"]
		st["radius_m"] = rr
		st["secs"] = (Time.get_ticks_msec() - t0) / 1000.0
		_p("[rc] carve '%s' r=%.0f res=%d -> min %.1f mean %.1f max %.1f  (%.1fs, %s pts)" % [
			pass_def["tag"], rr, res,
			st.get("sim_min_delta_m", 0.0), st.get("sim_mean_delta_m", 0.0),
			st.get("sim_max_delta_m", 0.0), st["secs"], str(st.get("native_points_written", 0))])
		(_m["carve_bakes"] as Array).append(st)

# ---- cameras --------------------------------------------------------
func _surface_world(dir: Vector3, extra_alt: float) -> Vec3D:
	var d := dir.normalized()
	var rr: float = _r + maxf(Planet.terrain_height(d), 0.0) + extra_alt
	return Vec3D.new(d.x * rr, d.y * rr, d.z * rr)

func _cam_valley_oblique() -> Vec3D:
	# behind the source, low + steep, looking down the carved channel -- little
	# sky in frame so auto-exposure meters the valley
	var back := _src if _src.length_squared() > 0.5 else _head
	var d := (back - _sea * (240.0 / _r)).normalized()
	var rr: float = _r + maxf(Planet.terrain_height(d), 0.0) + 150.0
	return Vec3D.new(d.x * rr, d.y * rr, d.z * rr)

func _cam_mouth_over() -> Vec3D:
	# low + straight down over the river mouth: keeps sky/ocean out of frame so
	# the auto-exposure opens up on the dark valley instead of metering the sky
	var m := (_waterline() - _sea * (60.0 / _r)).normalized()
	return _surface_world(m, 190.0)

func _cam_mouth_oblique() -> Vec3D:
	# low, off to one side of the channel mouth, steep look-down -- sees the
	# channel meet the sea in profile with little sky in frame
	var wl := _waterline()
	var t := _tangent(wl)
	var side: Vector3 = (t[0] as Vector3).cross(_sea).normalized()
	var d := (wl - _sea * (120.0 / _r) + side * (200.0 / _r)).normalized()
	var rr: float = _r + maxf(Planet.terrain_height(d), 0.0) + 130.0
	return Vec3D.new(d.x * rr, d.y * rr, d.z * rr)

## Corridor point nearest sea level -- where the carved channel meets the ocean.
func _waterline() -> Vector3:
	var prev := _head
	for k in 41:
		var frac := float(k) / 40.0
		var p := _corr_point(frac)
		if Planet.terrain_height(p) <= 0.0:
			return prev
		prev = p
	return _corr_point(0.85)

func _look(cam: Vec3D, target_dir: Vector3) -> Vector3:
	var tw := _surface_world(target_dir, 0.0)
	return Vector3(float(tw.x - cam.x), float(tw.y - cam.y), float(tw.z - cam.z)).normalized()

func _place_sun(elev_deg: float) -> void:
	var t := _tangent(_head)
	var up: Vector3 = t[0]
	var side := up.cross(_sea).normalized()
	var e := deg_to_rad(elev_deg)
	# azimuth: mostly cross-channel with a slight from-behind bias -> the carved
	# channel walls shadow into the cut (reads its depth) and the water glints
	Frames.helion_dir = (up * sin(e) + (side * 0.85 + _sea * -0.35).normalized() * cos(e)).normalized()
	if main != null:
		main._sync_sun_direction(true)
		main._sync_solar_brightness(true)

# ---- shot ----------------------------------------------------------
func _shot(name: String, cam: Vec3D, look_dir: Vector3) -> void:
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
	# hold the shot ~1.5 s so HumanEyeExposure adapts to this framing before capture
	for i in 95:
		p.world_pos = cam
		p._sync_transform()
		await get_tree().process_frame
	await RenderingServer.frame_post_draw
	var img := get_viewport().get_texture().get_image()
	img.save_png("%s/%s.png" % [_out, name])
	_p("[rc]   wrote %s.png" % name)

func _cache_settle(frames: int) -> void:
	for i in frames:
		if SparseHydroSurfaceCache and SparseHydroSurfaceCache.has_method("request_update"):
			SparseHydroSurfaceCache.request_update()
		await get_tree().process_frame

# ---- telemetry ---------------------------------------------------
func _hydro_cycles() -> int:
	var rt: Object = WaterSystem.sparse_runtime() if WaterSystem.has_method("sparse_runtime") else null
	if rt != null and rt.has_method("stats"):
		return int(rt.stats().get("cycles_completed", 0))
	return 0

func _tile_count() -> Variant:
	var g: Dictionary = WaterSystem.gpu_stats()
	var rt: Dictionary = g.get("sparse_runtime", {})
	var sched: Dictionary = rt.get("scheduler", {})
	for k in ["active_tiles", "resident_tiles", "reserved", "live_tiles", "allocated"]:
		if sched.has(k): return sched[k]
	return sched

## Wetted-tile geometry along the corridor axis + whether it reaches sea level.
func _flow_geom() -> Dictionary:
	var srt: Object = WaterSystem.sparse_runtime() if WaterSystem.has_method("sparse_runtime") else null
	if srt == null: return {}
	var sched: Object = srt.get("scheduler")
	if sched == null or sched.get("pool") == null: return {}
	var pool: Object = sched.get("pool")
	var recs: Array = pool.active_records()
	var side: Vector3 = _head.cross(_sea).normalized()
	var along_min := 1e9
	var along_max := -1e9
	var cross_lo := 1e9
	var cross_hi := -1e9
	var lead_dir := _head
	var min_lead_elev := 1e9
	var below_sea := 0
	var n := 0
	for rec in recs:
		var key: Variant = (rec as Dictionary).get("key")
		if key == null: continue
		var uv: Vector2 = HydroTileTopology.tile_center_face_uv(key)
		var td: Vector3 = CubeSphere.face_uv_to_dir(key.face, uv.x, uv.y)
		var off: Vector3 = (td - _head) * _r
		var al: float = off.dot(_sea)
		var cr: float = off.dot(side)
		if al > along_max:
			along_max = al
			lead_dir = td
		along_min = minf(along_min, al)
		cross_lo = minf(cross_lo, cr)
		cross_hi = maxf(cross_hi, cr)
		var teh := Planet.terrain_height(td)
		min_lead_elev = minf(min_lead_elev, teh)
		if teh <= 0.5:
			below_sea += 1
		n += 1
	if n == 0: return {"tiles": 0}
	return {
		"tiles": n,
		"downstream_m": roundi(along_max),
		"upstream_m": roundi(along_min),
		"width_m": roundi(cross_hi - cross_lo),
		"reach_frac_to_sea": snappedf(along_max / maxf(_head_sea_m, 1.0), 0.01),
		"lead_elev_m": roundi(Planet.terrain_height(lead_dir)),
		"lowest_wet_elev_m": roundi(min_lead_elev),
		"tiles_at_or_below_sea": below_sea,
		"reached_ocean": below_sea > 0,
	}

func _surface_diag_lite() -> String:
	var s := "?"
	if LocalWaterSurface != null and LocalWaterSurface.has_method("stats"):
		var ls: Dictionary = LocalWaterSurface.stats()
		s = "avail=%s render=%s cache=%s" % [
			ls.get("available", "?"), ls.get("render_enabled", "?"),
			ls.get("cache_available", "?")]
	var patch: Node = LocalWaterSurface.get_node_or_null("LocalWaterSurfacePatch") \
		if LocalWaterSurface != null else null
	if patch != null:
		s += " patch_visible=%s" % str(patch.visible)
	return s

## Writes a synthetic Gaussian bump straight into the dynamic-surface field to
## prove whether LocalWaterSurface can draw ANYTHING (isolates renderer from the
## sparse->field reconstruction).
func _run_gaussian_probe() -> void:
	_place_sun(_sun_elev)
	if WaterSystem.has_method("set_dynamic_surface_anchor_direction"):
		WaterSystem.set_dynamic_surface_anchor_direction(_src)
	if WaterSystem.has_method("set_dynamic_surface_render_enabled"):
		WaterSystem.set_dynamic_surface_render_enabled(true)
	if LocalWaterSurface and LocalWaterSurface.has_method("set_render_enabled"):
		LocalWaterSurface.set_render_enabled(true)
	var dl := Time.get_ticks_msec() + 60000
	while Time.get_ticks_msec() < dl and not (
			WaterSystem.has_method("dynamic_surface_available")
			and WaterSystem.dynamic_surface_available()):
		await get_tree().process_frame
	for i in 40:
		if WaterSystem.has_method("set_dynamic_surface_anchor_direction"):
			WaterSystem.set_dynamic_surface_anchor_direction(_src)
		await get_tree().process_frame
	var e: int = int(WaterSystem.debug_write_surface_gaussian(6.0, 220.0, Vector2.ZERO, 1.0))
	_p("[rc] gaussian write -> %d  %s" % [e, _surface_diag_lite()])
	for i in 50:
		await get_tree().process_frame
	var over := _surface_world(_src, 430.0)
	await _shot("probe_over", over, _look(over, _src))
	var od := (_src - _sea * (300.0 / _r)).normalized()
	var obl := _surface_world(od, 150.0)
	await _shot("probe_oblique", obl, _look(obl, _corr_point(0.06)))
	_p("[rc] gaussian probe done  %s" % _surface_diag_lite())
	_finish()

func _volume_hint() -> Variant:
	var g: Dictionary = WaterSystem.gpu_stats()
	var rt: Dictionary = g.get("sparse_runtime", {})
	return rt.get("last_solver_diagnostics", {})

func _p(s: String) -> void:
	print(s)

func _fail(m: String) -> void:
	_m["errors"].append(m); _p("[rc] FAIL: %s" % m); _finish()

func _finish() -> void:
	if _done: return
	_done = true
	var f := FileAccess.open("%s/metrics.json" % _out, FileAccess.WRITE)
	if f != null:
		f.store_string(JSON.stringify(_m, "  ")); f.close()
	if WaterSystem.has_method("remove_point_water_source"):
		WaterSystem.remove_point_water_source(SOURCE_ID)
	_p("[rc] wrote %s" % ProjectSettings.globalize_path(_out))
	get_tree().quit(0)

func _process(_dt: float) -> void:
	_frames += 1
	if not _done and (_frames > 1600000 or (Time.get_ticks_msec() - _wall0) > 2400000):
		_m["errors"].append("failsafe quit"); _finish()
