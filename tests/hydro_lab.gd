extends Node
## ASTERRA HYDRO LAB -- boot-once, command-file-driven terrain + water iteration.
##
## Boots the real Main scene ONCE (planet, orbit texture, sparse runtime), then
## watches a JSON command file. Each time the file changes it: resets terrain
## Deltas to the captured baseline, applies a fresh terrain composition (hydraulic
## erosion bakes / radial ridge-valley brushes), optionally drops a point water
## source and time-accelerates the sparse solver, then writes frames + telemetry.
##
## Iteration cost is ~30-120 s (no reboot) instead of a 4-5 min cold launch.
##
##   godot --path . res://tests/HydroLab.tscn --quit-after 5400000 -- --res=1280x720
##
## Command file:  user://hydro_lab/cmd.json      (write this to drive a job)
## Results:       user://hydro_lab/out/<id>/*.png + result_<id>.json
## Stop:          create user://hydro_lab/stop
##
## cmd.json schema (all keys optional except id):
## {
##   "id": 3,
##   "site": {"mode": "coastal", "inland_m": 950},          // or "reuse" to keep last
##   "terrain": [
##     {"op":"erode", "frac":0.42, "radius":360, "droplets":60000, "hardness":0.5,
##      "erode_radius":2, "inertia":0.05},
##     {"op":"valley","frac":0.6, "radius":220, "delta_m":-14, "hardness":0.5},
##     {"op":"ridge", "frac":0.5, "radius":160, "delta_m":10,  "hardness":0.5}
##   ],
##   "water":  {"source_frac":0.35, "q":80},                 // omit -> dry terrain only
##   "run":    {"timescale":35, "wall_s":90, "shots":3},
##   "sun":    {"elev":34, "mode":"cross"},                  // cross | behind
##   "cams":   ["valley_oblique","mouth_over","mouth_oblique"],
##   "coast_drain": true                                      // toggle the sea-level sink
## }

const LAB_ROOT := "user://hydro_lab"

var _res := Vector2i(1280, 720)
var _max_jobs := 200

var main: Node3D
var _r := 1000000.0
var _baseline_deltas: Dictionary = {}
var _wall0 := 0
var _frames := 0
var _done := false
var _jobs_run := 0
var _last_cmd_mtime := 0
var _cmd_path := ""

# resolved site (persists across "reuse" jobs)
var _head := Vector3.ZERO
var _sea := Vector3.ZERO
var _shore := Vector3.ZERO
var _head_sea_m := 1500.0
var _src := Vector3.ZERO
var _water_live := false
var _prev_water := false


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
	get_window().content_scale_size = _res
	DirAccess.make_dir_recursive_absolute(LAB_ROOT + "/out")
	_cmd_path = ProjectSettings.globalize_path(LAB_ROOT + "/cmd.json")
	await get_tree().process_frame

	_p("booting Main (one-time)...")
	main = load("res://scenes/Main.tscn").instantiate()
	add_child(main)
	var dl := Time.get_ticks_msec() + 220000
	while not main._started and Time.get_ticks_msec() < dl:
		await get_tree().process_frame
	if not main._started:
		_p("FATAL: Main never started"); get_tree().quit(1); return
	main.player.set_mouse_captured(false)
	main.player.input_enabled = false
	main.player.vertical_speed = 0.0
	main.hud.visible = false
	main.map.visible = false
	main._game_orbit = null
	_r = Planet.cfg.planet_radius

	var otx := Time.get_ticks_msec() + 150000
	while Time.get_ticks_msec() < otx and Planet.orbit_texture_face_res < 700:
		await get_tree().process_frame
	_p("orbit_texture_face_res=%d" % Planet.orbit_texture_face_res)

	var rt_dl := Time.get_ticks_msec() + 150000
	while Time.get_ticks_msec() < rt_dl and String(WaterSystem.sparse_runtime_state()) != "ready":
		await get_tree().process_frame
	_p("sparse_runtime_state=%s" % String(WaterSystem.sparse_runtime_state()))
	_detach_ocean_reanchor()

	_baseline_deltas = Deltas.serialize()
	_p("captured terrain baseline (%d edited tiles)" % Deltas.edited_tile_count())

	if WaterSystem.has_method("set_dynamic_surface_render_enabled"):
		WaterSystem.set_dynamic_surface_render_enabled(true)
	if LocalWaterSurface and LocalWaterSurface.has_method("set_render_enabled"):
		LocalWaterSurface.set_render_enabled(true)

	_p("READY. waiting for %s" % _cmd_path)
	_p("       (write a job to cmd.json; create %s/stop to exit)"
		% ProjectSettings.globalize_path(LAB_ROOT))
	await _watch_loop()


func _watch_loop() -> void:
	while not _done:
		if FileAccess.file_exists(ProjectSettings.globalize_path(LAB_ROOT + "/stop")):
			_p("stop file seen -- exiting"); break
		if _jobs_run >= _max_jobs:
			_p("max jobs reached -- exiting"); break
		if (Time.get_ticks_msec() - _wall0) > 5000000:
			_p("failsafe wall limit -- exiting"); break
		var mt := _cmd_mtime()
		if mt > 0 and mt != _last_cmd_mtime:
			_last_cmd_mtime = mt
			await get_tree().create_timer(0.25).timeout  # let the writer finish
			await _run_job()
		await get_tree().create_timer(0.5).timeout
	_done = true
	get_tree().quit(0)


func _cmd_mtime() -> int:
	if not FileAccess.file_exists(_cmd_path):
		return 0
	return int(FileAccess.get_modified_time(_cmd_path))


func _run_job() -> void:
	var txt := FileAccess.get_file_as_string(_cmd_path)
	var parsed: Variant = JSON.parse_string(txt)
	if not (parsed is Dictionary):
		_p("bad cmd.json (not an object) -- skipping"); return
	var job: Dictionary = parsed
	var jid := int(job.get("id", _jobs_run + 1))
	_jobs_run += 1
	var t_start := Time.get_ticks_msec()
	_p("---- job %d start ----" % jid)

	var out_dir := "%s/out/%d" % [LAB_ROOT, jid]
	DirAccess.make_dir_recursive_absolute(out_dir)
	var rec := {"id": jid, "cmd": job, "errors": []}

	# 1. reset previous water + terrain -------------------------------------
	if _water_live or _prev_water:
		if _water_live:
			WaterSystem.remove_point_water_source("lab_src")
		_water_live = false
		await _hard_reset_sparse()
	Deltas.deserialize(_baseline_deltas)
	Deltas.notify_changed(_head if _head != Vector3.ZERO else Vector3.UP, _r)
	Deltas.all_changed.emit()
	for i in 30:
		await get_tree().process_frame

	# 2. site --------------------------------------------------------------
	var site_spec: Dictionary = job.get("site", {})
	var site_mode := String(site_spec.get("mode", "reuse" if _head != Vector3.ZERO else "coastal"))
	if site_mode != "reuse" or _head == Vector3.ZERO:
		var inland: float = float(site_spec.get("inland_m", 950.0))
		var coast := _find_coastal_headwater(inland)
		if coast.is_empty():
			rec["errors"].append("no coastal headwater"); _write_result(rec, out_dir); return
		_head = (coast["site"] as Vector3).normalized()
		_sea = (coast["sea_dir"] as Vector3).normalized()
		_shore = (coast["shore"] as Vector3).normalized()
		_head_sea_m = float(coast["head_sea_m"])
	rec["site"] = {"head_elev_m": roundi(Planet.terrain_height(_head)),
		"shore_elev_m": roundi(Planet.terrain_height(_shore)),
		"head_sea_m": roundi(_head_sea_m)}
	rec["profile_pre"] = _corridor_profile()

	# 3. terrain composition --------------------------------------------
	var ops: Array = job.get("terrain", [])
	rec["terrain_ops"] = []
	for op_v in ops:
		var op: Dictionary = op_v
		var st := _apply_terrain_op(op)
		(rec["terrain_ops"] as Array).append(st)
		_p("  terrain %s -> %s" % [op.get("op", "?"), JSON.stringify(st)])
	Deltas.notify_changed(_mid_corridor(), _head_sea_m)
	for i in 90:
		await get_tree().process_frame
	rec["profile_post"] = _corridor_profile()

	# 4. sun + coast-drain toggle -------------------------------------
	var sun_spec: Dictionary = job.get("sun", {})
	_place_sun(float(sun_spec.get("elev", 34.0)), String(sun_spec.get("mode", "cross")))
	_set_coast_drain(bool(job.get("coast_drain", true)))

	# 5. dry-terrain shots --------------------------------------------
	var cams: Array = job.get("cams", ["valley_oblique", "mouth_over", "mouth_oblique"])
	await _settle(40)
	for cam_name in cams:
		await _shot("%s/j%d_dry_%s" % [out_dir, jid, cam_name], String(cam_name))

	# 6. water run --------------------------------------------------
	var water_spec: Variant = job.get("water", null)
	rec["samples"] = []
	if water_spec is Dictionary:
		var ws: Dictionary = water_spec
		var sfrac: float = clampf(float(ws.get("source_frac", 0.35)), 0.0, 0.92)
		_src = _head if sfrac <= 0.001 else _corr_point(sfrac)
		var q: float = float(ws.get("q", 80.0))
		var run_spec: Dictionary = job.get("run", {})
		var tscale: float = float(run_spec.get("timescale", 35.0))
		var wall_s: float = float(run_spec.get("wall_s", 90.0))
		var shots: int = maxi(int(run_spec.get("shots", 3)), 1)
		var rt: Object = WaterSystem.sparse_runtime()
		if rt != null:
			rt.set("time_scale", tscale)
			if rt.get("max_gpu_substeps") != null:
				rt.set("max_gpu_substeps", 64)
		if WaterSystem.has_method("set_automatic_physical_hydrolod_focus_direction"):
			WaterSystem.set_automatic_physical_hydrolod_focus_direction(_src)
		if WaterSystem.has_method("set_dynamic_surface_anchor_direction"):
			WaterSystem.set_dynamic_surface_anchor_direction(_mid_corridor())
		var err := WaterSystem.upsert_point_water_source("lab_src", _src, q, Vector3.ZERO, -1, true)
		_water_live = (err == OK)
		_prev_water = true
		_p("  water src frac %.2f elev %.0f q %.0f -> %d" % [
			sfrac, Planet.terrain_height(_src), q, int(err)])
		var per := wall_s / float(shots)
		for s in range(1, shots + 1):
			var seg_end := Time.get_ticks_msec() + int(per * 1000.0)
			while Time.get_ticks_msec() < seg_end and not _done:
				await get_tree().process_frame
				if SparseHydroSurfaceCache and SparseHydroSurfaceCache.has_method("request_update"):
					SparseHydroSurfaceCache.request_update()
			await _settle(16)
			var geom := _flow_geom()
			geom["diag"] = _diag()
			(rec["samples"] as Array).append(geom)
			_p("  shot %d  %s" % [s, JSON.stringify(geom)])
			for cam_name in cams:
				await _shot("%s/j%d_t%d_%s" % [out_dir, jid, s, cam_name], String(cam_name))

	_prev_water = water_spec is Dictionary
	rec["secs"] = (Time.get_ticks_msec() - t_start) / 1000.0
	_write_result(rec, out_dir)
	_p("---- job %d done in %.0fs -> %s ----" % [
		jid, rec["secs"], ProjectSettings.globalize_path(out_dir)])


func _write_result(rec: Dictionary, out_dir: String) -> void:
	var f := FileAccess.open("%s/result.json" % out_dir, FileAccess.WRITE)
	if f != null:
		f.store_string(JSON.stringify(rec, "  ")); f.close()
	var f2 := FileAccess.open("%s/result_%d.json" % [LAB_ROOT, int(rec.get("id", 0))], FileAccess.WRITE)
	if f2 != null:
		f2.store_string(JSON.stringify(rec, "  ")); f2.close()


# ---- terrain ops -------------------------------------------------------
func _apply_terrain_op(op: Dictionary) -> Dictionary:
	var kind := String(op.get("op", ""))
	var frac: float = clampf(float(op.get("frac", 0.5)), 0.0, 1.0)
	var center := _corr_point(frac)
	var radius: float = maxf(float(op.get("radius", 300.0)), 40.0)
	var hardness: float = clampf(float(op.get("hardness", 0.5)), 0.0, 0.95)
	if kind == "erode":
		var params := {
			"droplet_count": maxi(int(op.get("droplets", 55000)), 2000),
			"erode_radius": maxi(int(op.get("erode_radius", 2)), 1),
			"inertia": clampf(float(op.get("inertia", 0.05)), 0.0, 1.0),
		}
		var res := clampi(int(radius / 4.5), 96, 280)
		var st: Dictionary = ErosionRegionBake.bake(center, radius, res, hardness, params)
		st["op"] = "erode"; st["frac"] = frac; st["radius"] = radius
		return st
	elif kind == "valley" or kind == "lower" or kind == "ridge" or kind == "raise":
		var delta: float = float(op.get("delta_m", -12.0 if kind in ["valley", "lower"] else 12.0))
		var n := Deltas.apply_radial_brush(center, radius, delta, hardness, _r)
		Deltas.notify_changed(center, radius)
		return {"op": kind, "frac": frac, "radius": radius, "delta_m": delta,
			"points": n}
	elif kind == "channel":
		# Sweep a soft brush along the corridor applying a FIXED delta so the cut
		# follows the natural slope offset downward -- predictable and even, unlike
		# an absolute-bed target whose per-step cut swings wildly with the terrain.
		# Use a modest delta (~-14) so the trench floor is continuously below sea
		# level near the coast; the ocean then floods it as one estuary.
		var frac_a: float = clampf(float(op.get("frac_a", 0.40)), 0.0, 1.0)
		var frac_b: float = clampf(float(op.get("frac_b", 1.0)), 0.0, 1.0)
		var d_m: float = float(op.get("delta_m", -14.0))
		var w: float = maxf(float(op.get("width_m", 130.0)), 24.0)
		var span_m: float = maxf(absf(frac_b - frac_a) * _head_sea_m, 1.0)
		var steps: int = clampi(int(span_m / (w * 0.6)), 6, 400)
		var total := 0
		for si in steps + 1:
			var fr: float = lerpf(frac_a, frac_b, float(si) / float(steps))
			# per-step share so overlapping soft brushes sum to ~d_m, not a multiple
			total += Deltas.apply_radial_brush(_corr_point(fr), w, d_m * 0.62, 0.28, _r)
		Deltas.notify_changed(_corr_point((frac_a + frac_b) * 0.5), span_m + w)
		return {"op": "channel", "frac_a": frac_a, "frac_b": frac_b, "delta_m": d_m,
			"width_m": w, "steps": steps, "points": total}
	return {"op": kind, "error": "unknown op"}


# ---- coastal siting (from gauntlet_river_coast) ----------------------
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

func _find_coastal_headwater(inland_m: float) -> Dictionary:
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
			if f.elev[g.nbr[c * 8 + k]] <= 0.0:
				touches_sea = true
				break
		if not touches_sea:
			continue
		var score: float = minf(f.relief[c], 1200.0) - f.wetland[c] * 450.0 - f.floodplain[c] * 300.0
		if score > best:
			best = score
			best_c = c
	if best_c < 0:
		return {}
	var shore: Vector3 = g.cell_dir(best_c).normalized()
	var d := shore
	for _i in 12:
		var up_dir := _steepest(d, true)
		var nxt := (d + up_dir * (inland_m / 6.0 / _r)).normalized()
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

## Where the corridor meets the OPEN sea -- the OceanSystem macro coastline
## (has_water), not where a carved channel first dips below 0. Cameras aim here so
## the small TerrainEditDeltaGPU window (~512 m) straddles the estuary<->sea join.
func _waterline() -> Vector3:
	var prev := _head
	for k in 41:
		var p := _corr_point(float(k) / 40.0)
		if Planet.has_water(p):
			return prev
		prev = p
	return _corr_point(0.9)

func _corridor_profile() -> Array:
	var out: Array = []
	for k in 11:
		var frac := float(k) / 10.0
		var p := _corr_point(frac)
		out.append({"frac": frac, "dist_m": roundi(_head_sea_m * frac),
			"h": roundi(Planet.terrain_height(p)), "wet": Planet.has_water(p)})
	return out


# ---- cameras / sun / shot -----------------------------------------
func _surface_world(dir: Vector3, extra_alt: float) -> Vec3D:
	var d := dir.normalized()
	var rr: float = _r + maxf(Planet.terrain_height(d), 0.0) + extra_alt
	return Vec3D.new(d.x * rr, d.y * rr, d.z * rr)

func _cam(name: String) -> Array:
	# returns [cam_pos: Vec3D, look_target_dir: Vector3]
	match name:
		"valley_oblique":
			var back := _src if _src.length_squared() > 0.5 else _head
			var d := (back - _sea * (240.0 / _r)).normalized()
			return [_surface_world(d, 150.0), _waterline()]
		"mouth_over":
			var m := (_waterline() - _sea * (60.0 / _r)).normalized()
			return [_surface_world(m, 190.0), _waterline()]
		"mouth_oblique":
			var wl := _waterline()
			var t := _tangent(wl)
			var side: Vector3 = (t[0] as Vector3).cross(_sea).normalized()
			var d2 := (wl - _sea * (120.0 / _r) + side * (200.0 / _r)).normalized()
			return [_surface_world(d2, 130.0), wl]
		"head_over":
			return [_surface_world(_head, 260.0), _corr_point(0.15)]
		_:
			return [_surface_world(_mid_corridor(), 400.0), _mid_corridor()]

func _place_sun(elev_deg: float, mode: String) -> void:
	var t := _tangent(_head)
	var up: Vector3 = t[0]
	var side := up.cross(_sea).normalized()
	var e := deg_to_rad(elev_deg)
	var az: Vector3
	if mode == "behind":
		az = (_sea * -0.75 + side * 0.4).normalized()
	else:
		az = (side * 0.85 + _sea * -0.35).normalized()
	Frames.helion_dir = (up * sin(e) + az * cos(e)).normalized()
	if main != null:
		main._sync_sun_direction(true)
		main._sync_solar_brightness(true)

func _look(cam: Vec3D, target_dir: Vector3) -> Vector3:
	var tw := _surface_world(target_dir, 0.0)
	return Vector3(float(tw.x - cam.x), float(tw.y - cam.y), float(tw.z - cam.z)).normalized()

func _shot(path_noext: String, cam_name: String) -> void:
	var pair := _cam(cam_name)
	var cam: Vec3D = pair[0]
	var look_dir := _look(cam, pair[1])
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
	for i in 85:
		p.world_pos = cam
		p._sync_transform()
		await get_tree().process_frame
	await RenderingServer.frame_post_draw
	get_viewport().get_texture().get_image().save_png(path_noext + ".png")

func _settle(n: int) -> void:
	for i in n:
		if SparseHydroSurfaceCache and SparseHydroSurfaceCache.has_method("request_update"):
			SparseHydroSurfaceCache.request_update()
		await get_tree().process_frame


# ---- sparse hydro helpers ---------------------------------------
func _detach_ocean_reanchor() -> void:
	var rt: Object = WaterSystem.sparse_runtime()
	if rt != null and SparseHydroSurfaceCache != null \
			and rt.has_signal("cycle_completed") \
			and rt.cycle_completed.is_connected(SparseHydroSurfaceCache._on_runtime_cycle_completed):
		rt.cycle_completed.disconnect(SparseHydroSurfaceCache._on_runtime_cycle_completed)
		_p("detached surface-cache ocean re-anchor")

## Full sparse-runtime rebuild -- the only reliable way to guarantee zero
## residual water/tiles between water jobs. ~8-15 s, vs a ~4 min cold reboot.
func _hard_reset_sparse() -> void:
	if not WaterSystem.has_method("_bootstrap_sparse_runtime"):
		return
	WaterSystem._bootstrap_sparse_runtime()
	var dl := Time.get_ticks_msec() + 60000
	while Time.get_ticks_msec() < dl \
			and String(WaterSystem.sparse_runtime_state()) != "ready":
		await get_tree().process_frame
	_detach_ocean_reanchor()
	if WaterSystem.has_method("set_dynamic_surface_render_enabled"):
		WaterSystem.set_dynamic_surface_render_enabled(true)
	if LocalWaterSurface and LocalWaterSurface.has_method("set_render_enabled"):
		LocalWaterSurface.set_render_enabled(true)
	_p("  sparse reset -> %s, tiles %s"
		% [String(WaterSystem.sparse_runtime_state()), str(_tile_count())])

func _set_coast_drain(on: bool) -> void:
	var rt: Object = WaterSystem.sparse_runtime()
	if rt == null: return
	var solver: Object = rt.get("solver")
	if solver != null and solver.get("coast_drain_rate_per_s") != null:
		solver.set("coast_drain_rate_per_s", 0.6 if on else 0.0)
		solver.set("sea_level_m", 0.0)

func _tile_count() -> Variant:
	var sched: Dictionary = WaterSystem.gpu_stats().get("sparse_runtime", {}).get("scheduler", {})
	for k in ["active_tiles", "resident_tiles", "live_tiles", "allocated"]:
		if sched.has(k): return sched[k]
	return -1

func _diag() -> Dictionary:
	var rt: Dictionary = WaterSystem.gpu_stats().get("sparse_runtime", {})
	var d: Dictionary = rt.get("last_solver_diagnostics", {})
	return {"wet_cells": d.get("post_wet_cells"), "max_depth_m": d.get("post_max_depth_m"),
		"max_speed_mps": d.get("post_max_speed_mps"), "invalid": d.get("post_invalid_cells")}

func _flow_geom() -> Dictionary:
	var rt: Object = WaterSystem.sparse_runtime()
	if rt == null: return {}
	var sched: Object = rt.get("scheduler")
	if sched == null or sched.get("pool") == null: return {}
	var pool: Object = sched.get("pool")
	var recs: Array = pool.active_records()
	var side: Vector3 = _head.cross(_sea).normalized()
	var along_min := 1e9
	var along_max := -1e9
	var cross_lo := 1e9
	var cross_hi := -1e9
	var lead_dir := _head
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
		if Planet.terrain_height(td) <= 0.5:
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
		"tiles_at_or_below_sea": below_sea,
		"reached_ocean": below_sea > 0,
	}

func _p(s: String) -> void:
	print("[lab] " + s)

func _process(_dt: float) -> void:
	_frames += 1
