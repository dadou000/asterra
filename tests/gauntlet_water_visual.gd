extends Node
## ASTERRA GAUNTLET §8/§10-11 -- VISUAL temperate-forest spring test.
## Boots the real Main scene, injects a production point water source on dry
## sloped land, time-accelerates the sparse solver, enables the visible local
## water surface, and photographs the source + downstream channel over time.
##
##   godot --path . res://tests/GauntletWaterVisual.tscn --quit-after 1200000 -- \
##       --biome=temperate_forest --q=25 --timescale=30 --sim-s=600 --res=1600x900
##
## Output: user://gauntlet_water_visual/<ts>/*.png + metrics.json

const OUT_ROOT := "user://gauntlet_water_visual"
const SOURCE_ID := "gauntlet_spring"

var _biome := "temperate_forest"
var _q := 25.0
var _timescale := 30.0
var _sim_s := 600.0
var _res := Vector2i(1600, 900)

var main: Node3D
var _site := Vector3.ZERO
var _dir_down := Vector3.ZERO      # downhill unit tangent at the site
var _out := ""
var _m := {}
var _frames := 0
var _wall0 := 0
var _done := false
var _r := 1000000.0
var _cand_logged := false
var _steep := false


func _ready() -> void:
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	_wall0 = Time.get_ticks_msec()
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--biome="): _biome = a.trim_prefix("--biome=")
		elif a.begins_with("--q="): _q = float(a.trim_prefix("--q="))
		elif a.begins_with("--timescale="): _timescale = float(a.trim_prefix("--timescale="))
		elif a.begins_with("--sim-s="): _sim_s = float(a.trim_prefix("--sim-s="))
		elif a == "--steep": _steep = true
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
	_m = {"biome": _biome, "q_m3_s": _q, "timescale": _timescale, "sim_s": _sim_s,
		"errors": [], "samples": []}

	_p("[wv] booting Main")
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

	# --- pick a sloped, dry temperate-forest site with sustained downhill fall ---
	var base := _find_steep_site() if _steep else BiomeSites.find(_biome)
	if base == Vector3.ZERO:
		_fail("no site for '%s'" % _biome); return
	_site = _seek_sloped_dry(base)
	_dir_down = _steepest_descent(_site)
	# sun: low from downstream so the stream throws a bright specular glint toward
	# the oblique camera -- the clearest "this is liquid water" cue on dark terrain
	_place_sun(_site, _dir_down, 28.0)

	var prof := _profile(_site, _dir_down)
	_m["site"] = {
		"dir": [_site.x, _site.y, _site.z],
		"down": [_dir_down.x, _dir_down.y, _dir_down.z],
		"elevation_m": Planet.terrain_height(_site),
		"has_water": Planet.has_water(_site),
		"profile": prof,
	}
	_p("[wv] site elev %.0f m  down-fall %.0f m over 5 km  maxslope %.1f deg  wet=%s" % [
		Planet.terrain_height(_site), prof["total_drop_m"], prof["max_slope_deg"],
		Planet.has_water(_site)])

	# --- wire the sparse solver + visible surface ---
	if WaterSystem.has_method("set_automatic_physical_hydrolod_focus_direction"):
		WaterSystem.set_automatic_physical_hydrolod_focus_direction(_site)
	if WaterSystem.has_method("set_dynamic_surface_anchor_direction"):
		WaterSystem.set_dynamic_surface_anchor_direction(_site)
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
	_p("[wv] hydro time_scale=%.0f  surface_render=%s  local_surface_avail=%s" % [
		_timescale,
		WaterSystem.dynamic_surface_render_enabled() if WaterSystem.has_method("dynamic_surface_render_enabled") else "?",
		LocalWaterSurface.renderer_available() if (LocalWaterSurface and LocalWaterSurface.has_method("renderer_available")) else "?"])

	var rt_dl := Time.get_ticks_msec() + 150000
	while Time.get_ticks_msec() < rt_dl and String(WaterSystem.sparse_runtime_state()) != "ready":
		await get_tree().process_frame
	_p("[wv] sparse_runtime_state=%s" % String(WaterSystem.sparse_runtime_state()))

	# The surface cache bridge re-anchors its reconstruction field to OceanSystem's
	# centre on every runtime cycle, which drags the visible field away from our
	# test site. Detach that so the anchor stays pinned where we put it.
	var rt_for_cache: Object = WaterSystem.sparse_runtime()
	if rt_for_cache != null and SparseHydroSurfaceCache != null \
			and rt_for_cache.has_signal("cycle_completed") \
			and rt_for_cache.cycle_completed.is_connected(
				SparseHydroSurfaceCache._on_runtime_cycle_completed):
		rt_for_cache.cycle_completed.disconnect(
			SparseHydroSurfaceCache._on_runtime_cycle_completed)
		_p("[wv] detached surface-cache ocean re-anchor")
	if WaterSystem.has_method("set_dynamic_surface_anchor_direction"):
		WaterSystem.set_dynamic_surface_anchor_direction(_site)

	# instrument the sparse runtime so we can see whether the frontier expands
	var srt: Object = WaterSystem.sparse_runtime() if WaterSystem.has_method("sparse_runtime") else null
	if srt != null:
		if srt.has_signal("cycle_completed"):
			srt.cycle_completed.connect(func(cid, report):
				if cid % 300 == 0:
					var ar: Array = report.get("activation_results", [])
					var reach: Dictionary = WaterSystem.gpu_stats().get("sparse_reachability", {})
					_p("[wv] cyc %d activity=%s  reach{eval %s acc %s blk %s last=%s}" % [
						cid, str(report.get("activity_tiles","?")),
						str(reach.get("evaluations","?")), str(reach.get("accepted","?")),
						str(reach.get("blocked","?")), JSON.stringify(reach.get("last", {}))])
					if ar.size() > 0:
						_p("[wv]   activation_results=%s" % JSON.stringify(ar)))
		if srt.has_signal("frontier_overflow"):
			srt.frontier_overflow.connect(func(cid): _p("[wv] frontier_overflow at cyc %d" % cid))
		var fr: Object = srt.get("frontier")
		if fr != null and fr.has_signal("candidates_ready"):
			fr.candidates_ready.connect(func(rid, cands, ovf):
				if not _cand_logged and (cands as Array).size() > 0:
					_cand_logged = true
					_p("[wv] first frontier candidate: %s  overflow=%s" % [JSON.stringify(cands), ovf]))

	_p("[wv] surface_cache_avail=%s  cycles0=%s" % [
		(SparseHydroSurfaceCache.available() if (SparseHydroSurfaceCache and SparseHydroSurfaceCache.has_method("available")) else "?"),
		str(_hydro_cycles())])

	# --- shot T0: dry, before the source ---
	await _cache_settle(30)
	_p("[wv] surface_diag %s" % _surface_diag())
	await _shot("t0_dry_oblique", _cam_oblique(), _look(_cam_oblique(), _mid_point(120.0)))

	# --- inject ---
	var err := WaterSystem.upsert_point_water_source(SOURCE_ID, _site, _q, Vector3.ZERO, -1, true)
	_p("[wv] upsert -> %d, count=%d" % [int(err), WaterSystem.point_water_source_count()])

	# --- run: WALL-time driven. Each interval: settle the cache and shoot from
	#     two angles (a close oblique down the flow line + a mid overhead). ---
	var wall_total := maxf(_sim_s, 30.0)
	var shots := 6
	var shot_i := 1
	while shot_i <= shots and not _done:
		var seg_end := Time.get_ticks_msec() + int(wall_total / float(shots) * 1000.0)
		while Time.get_ticks_msec() < seg_end and not _done:
			await get_tree().process_frame
			if WaterSystem.has_method("set_dynamic_surface_anchor_direction"):
				WaterSystem.set_dynamic_surface_anchor_direction(_site)
			if SparseHydroSurfaceCache and SparseHydroSurfaceCache.has_method("request_update"):
				SparseHydroSurfaceCache.request_update()
		await _cache_settle(16)
		var cyc := _hydro_cycles()
		await _shot("t%d_oblique" % shot_i, _cam_oblique(), _look(_cam_oblique(), _mid_point(160.0)))
		await _shot("t%d_overhead" % shot_i, _cam_overhead(), _look(_cam_overhead(), _mid_point(220.0)))
		var geom := _flow_geom()
		_m["samples"].append({
			"shot": shot_i, "hydro_cycles": cyc,
			"tiles": _tile_count(), "diag": _volume_hint(), "flow_geom": geom,
		})
		_p("[wv] shot %d  hydro_cycles=%d  tiles=%s  flow_geom=%s" % [
			shot_i, cyc, str(_tile_count()), JSON.stringify(geom)])
		_p("[wv]   surface_diag=%s" % _surface_diag())
		shot_i += 1

	_finish()


# ---- siting helpers -------------------------------------------------------
func _tangent(d: Vector3) -> Array:
	var up := d.normalized()
	var ref := Vector3(0, 1, 0)
	if absf(up.dot(ref)) > 0.995: ref = Vector3(1, 0, 0)
	var east := ref.cross(up).normalized()
	var north := up.cross(east).normalized()
	return [up, east, north]

func _steepest_descent(d: Vector3) -> Vector3:
	var t := _tangent(d)
	var east: Vector3 = t[1]
	var north: Vector3 = t[2]
	var h0 := Planet.terrain_height(d)
	var best_az := 0.0
	var best_drop := -1e9
	for i in 72:
		var az := TAU * float(i) / 72.0
		var probe := (d + (east * sin(az) + north * cos(az)) * (400.0 / _r)).normalized()
		var drop := h0 - Planet.terrain_height(probe)
		if drop > best_drop:
			best_drop = drop
			best_az = az
	return (east * sin(best_az) + north * cos(best_az)).normalized()

## Highest-relief dry-land cell on the planet, ignoring biome -- the surest place
## to see water organise into a channel and run a long way downhill.
func _find_steep_site() -> Vector3:
	var f := Planet.fields
	var g := Planet.grid
	var best := -1.0e30
	var best_c := -1
	for c in g.cell_count:
		var b: int = f.biome[c]
		if b == PlanetFields.Biome.OCEAN or b == PlanetFields.Biome.SHELF_SEA \
				or b == PlanetFields.Biome.ICE_CAP:
			continue
		if f.elev[c] < 150.0:
			continue
		var score: float = minf(f.relief[c], 1400.0) \
			- f.floodplain[c] * 320.0 - f.wetland[c] * 320.0
		if score > best:
			best = score
			best_c = c
	if best_c < 0:
		return Vector3.ZERO
	return g.cell_dir(best_c)

func _seek_sloped_dry(start: Vector3) -> Vector3:
	# Follow the steepest-descent line downhill and stop where the forward 2 km
	# profile is genuinely steep and sustained -- a broad gentle slope sheet-flows
	# (physically correct) instead of forming the long channel we want to see.
	var d := start.normalized()
	var best_d := d
	var best_score := -1e9
	for _step in 16:
		var dd := _steepest_descent(d)
		var prof := _profile(d, dd)
		var drop := float(prof["total_drop_m"])
		var slope := float(prof["max_slope_deg"])
		var dry := not Planet.has_water(d)
		var land := Planet.terrain_height(d) > 5.0
		if dry and land:
			var score := drop + slope * 40.0
			if score > best_score:
				best_score = score
				best_d = d
			if drop >= 240.0 and slope >= 5.0:
				return d
		d = (d + dd * (450.0 / _r)).normalized()
	return best_d

func _profile(d: Vector3, down: Vector3) -> Dictionary:
	var dists := [0.0, 250.0, 500.0, 1000.0, 2000.0, 3500.0, 5000.0]
	var pts := []
	var prev := Planet.terrain_height(d)
	var total := 0.0
	var mx := 0.0
	for k in dists.size():
		var s: float = dists[k]
		var p := (d + down * (s / _r)).normalized()
		var h := Planet.terrain_height(p)
		pts.append({"dist_m": s, "h": h, "wet": Planet.has_water(p)})
		if k > 0:
			var seg: float = float(dists[k]) - float(dists[k - 1])
			mx = maxf(mx, rad_to_deg(atan2(prev - h, maxf(seg, 1.0))))
			total += maxf(prev - h, 0.0)
		prev = h
	return {"points": pts, "total_drop_m": total, "max_slope_deg": mx}

func _mid_point(dist_m: float) -> Vector3:
	return (_site + _dir_down * (dist_m / _r)).normalized()

# ---- camera --------------------------------------------------------------
func _surface_world(dir: Vector3, extra_alt: float) -> Vec3D:
	var d := dir.normalized()
	var rr: float = _r + maxf(Planet.terrain_height(d), 0.0) + extra_alt
	return Vec3D.new(d.x * rr, d.y * rr, d.z * rr)

func _cam_overhead() -> Vec3D:
	# straight above a point just downstream of the source, framing the wetted patch
	return _surface_world(_mid_point(220.0), 780.0)

func _cam_oblique() -> Vec3D:
	# behind & above the source, looking down the flow line
	var d := (_site - _dir_down * (360.0 / _r)).normalized()
	var rr: float = _r + maxf(Planet.terrain_height(d), 0.0) + 190.0
	return Vec3D.new(d.x * rr, d.y * rr, d.z * rr)

func _look(cam: Vec3D, target_dir: Vector3) -> Vector3:
	var tw := _surface_world(target_dir, 0.0)
	return Vector3(float(tw.x - cam.x), float(tw.y - cam.y), float(tw.z - cam.z)).normalized()

func _place_sun(site: Vector3, down: Vector3, elev_deg: float) -> void:
	var t := _tangent(site)
	var up: Vector3 = t[0]
	# sun comes from downstream (-down) with a slight side bias, so its reflection
	# lands on water flowing away from the oblique camera
	var side := up.cross(down).normalized()
	var e := deg_to_rad(elev_deg)
	Frames.helion_dir = (up * sin(e) + (down * -0.88 + side * 0.22).normalized() * cos(e)).normalized()
	if main != null:
		main._sync_sun_direction(true)
		main._sync_solar_brightness(true)

# ---- shot --------------------------------------------------------------
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
	for i in 40:
		p.world_pos = cam
		p._sync_transform()
		await get_tree().process_frame
	await RenderingServer.frame_post_draw
	var img := get_viewport().get_texture().get_image()
	img.save_png("%s/%s.png" % [_out, name])
	_p("[wv]   wrote %s.png" % name)

func _cache_settle(frames: int) -> void:
	for i in frames:
		if WaterSystem.has_method("set_dynamic_surface_anchor_direction"):
			WaterSystem.set_dynamic_surface_anchor_direction(_site)
		if SparseHydroSurfaceCache and SparseHydroSurfaceCache.has_method("request_update"):
			SparseHydroSurfaceCache.request_update()
		await get_tree().process_frame

# ---- telemetry -------------------------------------------------------
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

## Plan-view geometry of the active-tile set, projected onto the flow line.
## A natural river reaches far downstream with a modest cross-flow width and a
## leading edge well below the source; a pond spreads roughly equally all ways.
func _flow_geom() -> Dictionary:
	var srt: Object = WaterSystem.sparse_runtime() if WaterSystem.has_method("sparse_runtime") else null
	if srt == null:
		return {}
	var sched: Object = srt.get("scheduler")
	if sched == null or sched.get("pool") == null:
		return {}
	var pool: Object = sched.get("pool")
	var recs: Array = pool.active_records()
	var side: Vector3 = _site.normalized().cross(_dir_down).normalized()
	var down_min := 1e9
	var down_max := -1e9
	var cross_lo := 1e9
	var cross_hi := -1e9
	var lead_dir := _site
	var n := 0
	for rec in recs:
		var key: Variant = (rec as Dictionary).get("key")
		if key == null:
			continue
		var uv: Vector2 = HydroTileTopology.tile_center_face_uv(key)
		var td: Vector3 = CubeSphere.face_uv_to_dir(key.face, uv.x, uv.y)
		var off: Vector3 = (td - _site.normalized()) * _r
		var dn: float = off.dot(_dir_down)
		var cr: float = off.dot(side)
		if dn > down_max:
			down_max = dn
			lead_dir = td
		down_min = minf(down_min, dn)
		cross_lo = minf(cross_lo, cr)
		cross_hi = maxf(cross_hi, cr)
		n += 1
	if n == 0:
		return {"tiles": 0}
	return {
		"tiles": n,
		"downstream_m": roundi(down_max),
		"upstream_m": roundi(down_min),
		"width_m": roundi(cross_hi - cross_lo),
		"lead_drop_m": roundi(Planet.terrain_height(_site) - Planet.terrain_height(lead_dir)),
	}

func _surface_diag() -> String:
	var s := "?"
	if LocalWaterSurface != null and LocalWaterSurface.has_method("stats"):
		var ls: Dictionary = LocalWaterSurface.stats()
		s = "local{avail=%s render=%s cache=%s}" % [
			ls.get("available", "?"), ls.get("render_enabled", "?"),
			ls.get("cache_available", "?")]
	var patch: Node = LocalWaterSurface.get_node_or_null("LocalWaterSurfacePatch") \
		if LocalWaterSurface != null else null
	if patch != null:
		s += " patch_visible=%s" % str(patch.visible)
	if SparseHydroSurfaceCache != null and SparseHydroSurfaceCache.has_method("stats"):
		var cs: Variant = SparseHydroSurfaceCache.stats()
		if cs is Dictionary:
			s += " cache=%s" % JSON.stringify(cs)
	var res: Object = WaterSystem.surface_resources() if WaterSystem.has_method("surface_resources") else null
	if res != null and res.has_method("revision"):
		s += " field_rev=%s half_extent=%s" % [
			str(res.revision()),
			str(res.field_half_extent_m()) if res.has_method("field_half_extent_m") else "?"]
	if WaterSystem.has_method("dynamic_surface_anchor_frame"):
		var af: Dictionary = WaterSystem.dynamic_surface_anchor_frame()
		var ad: Variant = af.get("dir", Vector3.ZERO)
		if ad is Vector3:
			s += " anchor_off_site_m=%.0f" % (
				acos(clampf((ad as Vector3).normalized().dot(_site.normalized()), -1.0, 1.0)) * _r)
	return s

func _volume_hint() -> Variant:
	var g: Dictionary = WaterSystem.gpu_stats()
	var rt: Dictionary = g.get("sparse_runtime", {})
	return rt.get("last_solver_diagnostics", {})

func _p(s: String) -> void:
	print(s)

func _fail(m: String) -> void:
	_m["errors"].append(m); _p("[wv] FAIL: %s" % m); _finish()

func _finish() -> void:
	if _done: return
	_done = true
	var f := FileAccess.open("%s/metrics.json" % _out, FileAccess.WRITE)
	if f != null:
		f.store_string(JSON.stringify(_m, "  ")); f.close()
	if WaterSystem.has_method("remove_point_water_source"):
		WaterSystem.remove_point_water_source(SOURCE_ID)
	_p("[wv] wrote %s" % ProjectSettings.globalize_path(_out))
	get_tree().quit(0)

func _process(_dt: float) -> void:
	_frames += 1
	if not _done and (_frames > 800000 or (Time.get_ticks_msec() - _wall0) > 1200000):
		_m["errors"].append("failsafe quit"); _finish()
