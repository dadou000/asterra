extends Node
## ASTERRA GAUNTLET §6-10 -- temperate-forest spring test on the PRODUCTION sparse
## hydrology path. Renderer-independent: verdicts come from solver diagnostics,
## total-volume readback and active-tile telemetry, not from pixels.
##
##   godot --path . res://tests/GauntletWater.tscn --quit-after 900000 -- \
##       --biome=temperate_forest --q=8 --run-s=180 --drain-s=90
##
## Output: user://gauntlet_water/<ts>/metrics.json

const OUT_ROOT := "user://gauntlet_water"
const SOURCE_ID := "gauntlet_spring"

var _biome := "temperate_forest"
var _q := 8.0
var _run_s := 180.0
var _drain_s := 90.0
var _use_vol_diag := false  # --voldiag to enable; isolating a push-constant crash

var main: Node3D
var _site := Vector3.ZERO
var _out := ""
var _m := {}
var _frames := 0
var _wall0 := 0
var _done := false
var _vol_diag: Object = null
var _last_vol_m3 := -1.0
var _vol_pending := false


func _ready() -> void:
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	_wall0 = Time.get_ticks_msec()
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--biome="): _biome = a.trim_prefix("--biome=")
		elif a.begins_with("--q="): _q = float(a.trim_prefix("--q="))
		elif a.begins_with("--run-s="): _run_s = float(a.trim_prefix("--run-s="))
		elif a.begins_with("--drain-s="): _drain_s = float(a.trim_prefix("--drain-s="))
		elif a == "--voldiag": _use_vol_diag = true

	_out = "%s/%d" % [OUT_ROOT, Time.get_unix_time_from_system()]
	DirAccess.make_dir_recursive_absolute(_out)
	_m = {"biome": _biome, "q_m3_s": _q, "run_s": _run_s, "drain_s": _drain_s,
		"errors": [], "samples": [], "verdicts": {}}

	_p("[water] booting Main")
	main = load("res://scenes/Main.tscn").instantiate()
	add_child(main)
	var dl := Time.get_ticks_msec() + 180000
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

	_site = BiomeSites.find(_biome)
	if _site == Vector3.ZERO:
		_fail("no site for biome '%s'" % _biome); return
	var info: Dictionary = Planet.sample_info(_site)
	_m["site"] = {
		"dir": [_site.x, _site.y, _site.z],
		"biome": info.get("biome_name", "?"),
		"elevation_m": info.get("elevation", 0.0),
		"terrain_height_m": Planet.terrain_height(_site.normalized()),
		"has_water": Planet.has_water(_site.normalized()),
		"precip_mm": info.get("precip", 0.0),
	}
	_p("[water] site %s elev %.0fm has_water=%s" % [
		info.get("biome_name","?"), info.get("elevation",0.0),
		Planet.has_water(_site.normalized())])

	# --- §6: terrain must be dry, above sea level, visibly sloped, with downstream fall.
	_m["slope_profile"] = _profile_downhill(_site.normalized())
	var prof: Dictionary = _m["slope_profile"]
	_m["verdicts"]["site_land_dry"] = (not Planet.has_water(_site.normalized())) \
		and float(_m["site"]["terrain_height_m"]) > 1.0
	_m["verdicts"]["site_sloped"] = float(prof.get("max_slope_deg", 0.0)) >= 1.5
	_m["verdicts"]["site_has_fall"] = float(prof.get("total_drop_m", 0.0)) >= 20.0

	# Force the fine hydro LOD onto the site (no need to fly the player there).
	if WaterSystem.has_method("set_automatic_physical_hydrolod_focus_direction"):
		WaterSystem.set_automatic_physical_hydrolod_focus_direction(_site.normalized())
	if WaterSystem.has_method("set_dynamic_surface_anchor_direction"):
		WaterSystem.set_dynamic_surface_anchor_direction(_site.normalized())
	if WaterSystem.has_method("set_dynamic_surface_render_enabled"):
		WaterSystem.set_dynamic_surface_render_enabled(true)
	if LocalWaterSurface and LocalWaterSurface.has_method("set_render_enabled"):
		LocalWaterSurface.set_render_enabled(true)

	# --- wait for the sparse runtime.
	var rt_dl := Time.get_ticks_msec() + 150000
	while Time.get_ticks_msec() < rt_dl:
		if String(WaterSystem.sparse_runtime_state()) == "ready":
			break
		await get_tree().process_frame
	_m["sparse_runtime_state_at_start"] = String(WaterSystem.sparse_runtime_state())
	_p("[water] sparse runtime state: %s" % _m["sparse_runtime_state_at_start"])
	if _m["sparse_runtime_state_at_start"] != "ready":
		_m["errors"].append("sparse runtime not ready: %s" % _m["sparse_runtime_state_at_start"])

	_setup_volume_diag()
	var t0_stats := _stats_snapshot()
	_m["stats_before_source"] = t0_stats
	_p("[water] tiles before source: %s" % str(t0_stats.get("atlas_tiles", "?")))

	# --- §7: inject the production point source.
	var err := WaterSystem.upsert_point_water_source(
		SOURCE_ID, _site.normalized(), _q, Vector3.ZERO, -1, true)
	_m["upsert_error"] = int(err)
	_p("[water] upsert_point_water_source -> %d, count=%d" % [
		int(err), WaterSystem.point_water_source_count()])
	_m["verdicts"]["source_registered"] = (err == OK) and WaterSystem.point_water_source_count() >= 1

	# one-time full gpu_stats dump so we can see the real atlas / diagnostic keys
	await get_tree().process_frame
	await get_tree().process_frame
	_m["gpu_stats_after_upsert"] = WaterSystem.gpu_stats()
	var rt0: Dictionary = _m["gpu_stats_after_upsert"].get("sparse_runtime", {})
	_p("[water] gpu_stats.sparse_runtime keys: %s" % str(rt0.keys()))
	_p("[water] gpu_stats.sparse_atlas: %s" % JSON.stringify(_m["gpu_stats_after_upsert"].get("sparse_atlas", {})))
	_p("[water] sources: %s" % JSON.stringify(rt0.get("sources", {})))

	# --- §8: run. Log volume + tiles + solver diagnostics over time.
	var inject_start := Time.get_ticks_msec()
	await _monitor(inject_start, _run_s, true)
	var v_run := _last_vol_m3
	var expected := _q * ((Time.get_ticks_msec() - inject_start) / 1000.0)
	_m["volume_after_run_m3"] = v_run
	_m["volume_expected_m3"] = expected
	if v_run >= 0.0 and expected > 1.0:
		var ratio := v_run / expected
		_m["conservation_ratio"] = ratio
		_m["verdicts"]["mass_conservation"] = ratio >= 0.6 and ratio <= 1.5
		_p("[water] volume %.1f m3 vs expected %.1f (ratio %.2f)" % [v_run, expected, ratio])
	else:
		_m["errors"].append("no volume readback")
		_m["verdicts"]["mass_conservation"] = false

	var s_run := _stats_snapshot()
	_m["stats_after_run"] = s_run
	_m["verdicts"]["tiles_activated"] = int(s_run.get("atlas_tiles", 0)) > int(t0_stats.get("atlas_tiles", 0))
	_m["verdicts"]["solver_finite"] = not bool(s_run.get("solver_nonfinite", false))

	# --- §9: shut the source and watch drainage.
	WaterSystem.set_point_water_source_enabled(SOURCE_ID, false)
	_p("[water] source disabled; draining %.0fs" % _drain_s)
	var drain_start := Time.get_ticks_msec()
	await _monitor(drain_start, _drain_s, false)
	var v_drain := _last_vol_m3
	_m["volume_after_drain_m3"] = v_drain
	# after shutoff, volume must not keep climbing at the injection rate
	if v_run >= 0.0 and v_drain >= 0.0:
		var drift := v_drain - v_run
		var drain_expected_if_still_on := _q * ((Time.get_ticks_msec() - drain_start) / 1000.0)
		_m["post_shutoff_drift_m3"] = drift
		_m["verdicts"]["source_shutoff_ok"] = drift < 0.5 * drain_expected_if_still_on
		_p("[water] post-shutoff volume drift %.1f m3 (would be +%.1f if still injecting)" % [
			drift, drain_expected_if_still_on])

	WaterSystem.remove_point_water_source(SOURCE_ID)
	_finish()


func _profile_downhill(d: Vector3) -> Dictionary:
	# tangent frame
	var up := d
	var ref := Vector3(0, 1, 0)
	if absf(up.dot(ref)) > 0.995: ref = Vector3(1, 0, 0)
	var east := ref.cross(up).normalized()
	var north := up.cross(east).normalized()
	var r: float = Planet.cfg.planet_radius
	var h0: float = Planet.terrain_height(d)
	# steepest descent among 24 azimuths at 120 m
	var best_az := 0.0
	var best_drop := -1e9
	for i in 24:
		var az := TAU * float(i) / 24.0
		var probe := (d + (east * sin(az) + north * cos(az)) * (120.0 / r)).normalized()
		var drop := h0 - Planet.terrain_height(probe)
		if drop > best_drop:
			best_drop = drop
			best_az = az
	var dir_h := east * sin(best_az) + north * cos(best_az)
	var pts := []
	var dists := [0.0, 60.0, 150.0, 300.0, 600.0, 1200.0, 2400.0, 4000.0]
	var prev_h := h0
	var total_drop := 0.0
	var max_slope := 0.0
	for k in dists.size():
		var s: float = dists[k]
		var p := (d + dir_h * (s / r)).normalized()
		var h: float = Planet.terrain_height(p)
		pts.append({"dist_m": s, "height_m": h, "wet": Planet.has_water(p)})
		if k > 0:
			var seg: float = float(dists[k]) - float(dists[k - 1])
			var slope_deg := rad_to_deg(atan2(prev_h - h, maxf(seg, 1.0)))
			max_slope = maxf(max_slope, slope_deg)
			total_drop += maxf(prev_h - h, 0.0)
		prev_h = h
	return {"steepest_az_rad": best_az, "points": pts,
		"total_drop_m": total_drop, "max_slope_deg": max_slope}


func _setup_volume_diag() -> void:
	if not _use_vol_diag:
		return
	var atlas: Variant = WaterSystem.get("_sparse_atlas")
	if atlas == null:
		_m["errors"].append("no _sparse_atlas for volume diag")
		return
	if not ClassDB.class_exists(&"SparseHydroVolumeDiagnosticsGPU") \
			and not _has_script_class("SparseHydroVolumeDiagnosticsGPU"):
		pass
	var VD = load("res://scripts/water/sparse_hydro_volume_diagnostics_gpu.gd")
	if VD == null:
		_m["errors"].append("volume diag script missing"); return
	_vol_diag = VD.new()
	_vol_diag.name = "GauntletVolDiag"
	add_child(_vol_diag)
	if _vol_diag.has_signal("volume_ready"):
		_vol_diag.volume_ready.connect(func(_rid, v): _last_vol_m3 = v; _vol_pending = false)
	if _vol_diag.has_signal("readback_failed"):
		_vol_diag.readback_failed.connect(func(_rid, e):
			_vol_pending = false
			_m["errors"].append("volume readback failed %d" % int(e)))
	var e: Variant = _vol_diag.call("initialize_from_atlas", atlas, false)
	_p("[water] volume diag init -> %s" % str(e))


func _request_volume() -> void:
	if _vol_diag == null or _vol_pending: return
	if not bool(_vol_diag.call("initialized_ok")): return
	var rid := int(_vol_diag.call("request_volume"))
	if rid >= 0: _vol_pending = true


func _stats_snapshot() -> Dictionary:
	var g: Dictionary = WaterSystem.gpu_stats()
	var atlas: Dictionary = g.get("sparse_atlas", {})
	var rt: Dictionary = g.get("sparse_runtime", {})
	var diag: Dictionary = rt.get("last_solver_diagnostics", {})
	var nonfinite := false
	for key in ["nan", "inf", "nan_count", "negative_depth", "cfl_violations", "checkerboard"]:
		var v: Variant = diag.get(key, 0)
		if (v is float or v is int) and float(v) > 0.0:
			nonfinite = true
	var tiles := 0
	for tk in ["active_tiles", "resident_tiles", "tile_count", "allocated_tiles", "tiles"]:
		if atlas.has(tk):
			tiles = int(atlas[tk]); break
	return {
		"state": String(WaterSystem.sparse_runtime_state()),
		"atlas_tiles": tiles,
		"atlas": atlas,
		"solver_diagnostics": diag,
		"solver_nonfinite": nonfinite,
		"source_count": WaterSystem.point_water_source_count(),
	}


func _monitor(t0: int, seconds: float, injecting: bool) -> void:
	var next_log := 0.0
	var end := t0 + int(seconds * 1000.0)
	while Time.get_ticks_msec() < end:
		await get_tree().process_frame
		_request_volume()
		var wall := (Time.get_ticks_msec() - t0) / 1000.0
		if wall >= next_log:
			next_log += 5.0
			var s := _stats_snapshot()
			var rec := {
				"phase": "inject" if injecting else "drain",
				"t_s": wall, "vol_m3": _last_vol_m3,
				"tiles": int(s.get("atlas_tiles", -1)),
				"state": String(s.get("state", "?")),
				"nonfinite": bool(s.get("solver_nonfinite", false)),
			}
			_m["samples"].append(rec)
			_p("[water] %s t=%5.1fs vol=%9.1f tiles=%d state=%s nonfinite=%s" % [
				rec["phase"], wall, _last_vol_m3, rec["tiles"], rec["state"], rec["nonfinite"]])


func _p(s: String) -> void:
	print(s); if _m.has("_log"): _m["_log"].append(s)


func _fail(msg: String) -> void:
	_m["errors"].append(msg)
	_p("[water] FAIL: %s" % msg)
	_finish()


func _finish() -> void:
	if _done: return
	_done = true
	var f := FileAccess.open("%s/metrics.json" % _out, FileAccess.WRITE)
	if f != null:
		f.store_string(JSON.stringify(_m, "  ")); f.close()
	_p("[water] verdicts: %s" % JSON.stringify(_m.get("verdicts", {})))
	_p("[water] wrote %s" % ProjectSettings.globalize_path(_out))
	get_tree().quit(0)


func _process(_dt: float) -> void:
	_frames += 1
	if not _done and (_frames > 500000 or (Time.get_ticks_msec() - _wall0) > 900000):
		_m["errors"].append("failsafe quit")
		_finish()


func _has_script_class(_n: String) -> bool:
	return false
