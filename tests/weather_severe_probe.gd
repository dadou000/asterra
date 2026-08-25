extends Node
## Objective severe-weather structure probe.
##
## The 86 km global mesh is evaluated for tropical cyclones and linear MCSs.
## The 2.2 km nest is evaluated for cumulonimbus lifecycle, squall-line shape,
## and persistent rotating-updraft (supercell-like) signatures.
##
## Run with:
##   godot --headless --path . tests/WeatherSevereProbe.tscn -- \
##     --days=15 --local-hours=18 --seed=1095976005 --output=res://tests/weather_severe_output

const GW := 256
const GH := 128
const LW := 192
const LH := 192
const CHANNELS := 4
const GLOBAL_DT := 90.0
const LOCAL_DT := 20.0
const SAMPLE_SECONDS := 3.0 * 3600.0
const LOCAL_SAMPLE_SECONDS := 10.0 * 60.0


func _ready() -> void:
	await get_tree().process_frame
	var days := maxi(_int_arg("days", 15), 1)
	var local_hours := maxi(_int_arg("local-hours", 18), 0)
	var seed := _int_arg("seed", 0x41535445)
	var output := ProjectSettings.globalize_path(
		_string_arg("output", "res://tests/weather_severe_output"))
	if DirAccess.make_dir_recursive_absolute(output) != OK:
		_fail("could not create output directory %s" % output)
		return
	var load_status := GDExtensionManager.load_extension("res://bin/asterra_weather.gdextension")
	if load_status != GDExtensionManager.LOAD_STATUS_OK \
			and load_status != GDExtensionManager.LOAD_STATUS_ALREADY_LOADED:
		_fail("extension load failed with status %d" % load_status)
		return
	var native: Object = ClassDB.instantiate(&"WeatherNative")
	if native == null or not native.has_method(&"get_global_convective_rgba"):
		_fail("severe-weather diagnostic API is unavailable")
		return

	var cfg := GenConfig.new()
	cfg.face_res = 48
	cfg.erosion_iterations = 12
	cfg.world_seed = seed
	Planet.adopt(PlanetBake.new(cfg).bake(Callable(), false))
	native.call(&"initialize", seed)
	native.call(&"set_global_surface_fields", _build_surface_fields())

	var report := {
		"seed": seed,
		"global_days": days,
		"global_samples": 0,
		"tropical_depression_samples": 0,
		"hurricane_samples": 0,
		"max_tropical_score": 0.0,
		"max_tropical_wind_ms": 0.0,
		"max_pressure_closure_pa": 0.0,
		"linear_mcs_samples": 0,
		"max_global_line_aspect": 0.0,
		"max_global_line_cells": 0,
		"max_global_ascent": 0.0,
		"max_global_anvil": 0.0,
	}
	var simulation_seconds := 0.0
	var next_sample := 0.0
	var total_steps := int(days * 86400.0 / GLOBAL_DT)
	for step in total_steps + 1:
		if simulation_seconds + 0.01 >= next_sample:
			var fields := _global_fields(native)
			var metrics := _measure_global(fields)
			_accumulate_global(report, metrics)
			if int(round(simulation_seconds)) % int(3.0 * 86400.0) == 0:
				_save_snapshot(fields, GW, GH, output.path_join(
					"global_day_%03d.png" % int(simulation_seconds / 86400.0)))
			print("SEVERE GLOBAL day %5.2f tc=%d hurricane=%d line=%d aspect=%.1f tcwind=%.1f maxwind=%.1f closure=%.0f/%.0fPa sst=%.1fK ascent=%.2f anvil=%.2f" % [
				simulation_seconds / 86400.0, metrics.tropical_count,
				metrics.hurricane_count, metrics.line_count, metrics.max_line_aspect,
				metrics.max_tropical_wind_ms, metrics.max_wind_any,
				metrics.max_pressure_closure_pa, metrics.max_tropical_closure_any,
				metrics.max_tropical_sst_k,
				metrics.max_ascent, metrics.max_anvil])
			if metrics.best_genesis_score > 0.0:
				print("  GENESIS score=%.5f cape=%.2f ascent=%.2f shear=%.2f cyclonic=%.3f" % [
					metrics.best_genesis_score, metrics.best_genesis.x,
					metrics.best_genesis.y, metrics.best_genesis.z, metrics.best_genesis.w])
			print("  CORES north=%d/%.6f/%.0fPa south=%d/%.6f/%.0fPa" % [
				int(metrics.core_north), metrics.core_north_activity, metrics.core_north_closure_pa,
				int(metrics.core_south), metrics.core_south_activity, metrics.core_south_closure_pa])
			if metrics.core_north >= 0.0:
				print("  NORTH signature storm=%.2f up=%.2f anvil=%.2f wind=%.1f rot=%.3f" % [
					metrics.north_signature.x, metrics.north_signature.y,
					metrics.north_signature.z, metrics.north_signature.w * 60.0,
					metrics.north_cyclonic])
			next_sample += SAMPLE_SECONDS
		if step == total_steps:
			break
		_set_solar_forcing(native, simulation_seconds)
		native.call(&"step_global", GLOBAL_DT)
		simulation_seconds += GLOBAL_DT

	var final_fields := _global_fields(native)
	var local_cell := _best_local_environment(final_fields)
	var local_direction := _global_cell_direction(local_cell)
	report["local_center_cell"] = local_cell
	report["local_center_lat_deg"] = rad_to_deg(asin(local_direction.y))
	report["local_center_lon_deg"] = rad_to_deg(atan2(local_direction.z, local_direction.x))
	if local_hours > 0:
		native.call(&"set_local_center", local_direction)
		native.call(&"reset_local_from_global")
		native.call(&"set_local_surface_fields", _build_local_surface_fields(native))
		var local_report := _run_local(native, simulation_seconds, local_hours, output)
		for key in local_report:
			report[key] = local_report[key]

	var validation_errors: Array[String] = []
	if days >= 4:
		if report.tropical_depression_samples == 0:
			validation_errors.push_back("no closed tropical-cyclone structure detected")
		if report.hurricane_samples == 0:
			validation_errors.push_back("no sustained-wind hurricane sample detected")
		if report.linear_mcs_samples == 0:
			validation_errors.push_back("no global linear convective system detected")
	if local_hours >= 6:
		if report.cumulonimbus_samples == 0:
			validation_errors.push_back("no local deep-convective lifecycle detected")
		if report.local_squall_line_samples == 0:
			validation_errors.push_back("no local squall-line structure detected")
		if report.supercell_like_samples < 2:
			validation_errors.push_back("no persistent local rotating-updraft signature detected")
		if report.local_runaway_detected:
			validation_errors.push_back("local nest became numerically unstable")
	report["passed"] = validation_errors.is_empty()
	report["validation_errors"] = validation_errors

	var report_file := FileAccess.open(output.path_join("severe_report.json"), FileAccess.WRITE)
	if report_file != null:
		report_file.store_string(JSON.stringify(report, "\t"))
		report_file.close()
	if validation_errors.is_empty():
		print("WEATHER_SEVERE_PROBE_OK %s" % JSON.stringify(report))
		get_tree().quit(0)
	else:
		_fail("; ".join(validation_errors))


func _global_fields(native: Object) -> Dictionary:
	return {
		"weather": native.call(&"get_global_weather_rgba"),
		"diagnostics": native.call(&"get_global_diagnostics_rgba"),
		"products": native.call(&"get_global_products_rgba"),
		"convective": native.call(&"get_global_convective_rgba"),
		"cores": native.call(&"get_tropical_core_diagnostics"),
	}


func _local_fields(native: Object) -> Dictionary:
	return {
		"weather": native.call(&"get_local_weather_rgba"),
		"diagnostics": native.call(&"get_local_diagnostics_rgba"),
		"products": native.call(&"get_local_products_rgba"),
		"convective": native.call(&"get_local_convective_rgba"),
	}


func _measure_global(fields: Dictionary) -> Dictionary:
	var weather: PackedFloat32Array = fields.weather
	var diagnostics: PackedFloat32Array = fields.diagnostics
	var products: PackedFloat32Array = fields.products
	var convective: PackedFloat32Array = fields.convective
	var cores: PackedFloat32Array = fields.cores
	var tropical_count := 0
	var hurricane_count := 0
	var max_tropical_score := 0.0
	var max_tropical_wind_ms := 0.0
	var max_pressure_closure_pa := 0.0
	var max_ascent := 0.0
	var max_anvil := 0.0
	var max_wind_any := 0.0
	var max_tropical_sst_k := 0.0
	var max_tropical_closure_any := 0.0
	var best_genesis_score := 0.0
	var best_genesis := Vector4.ZERO # CAPE, ascent, shear, signed cyclonic rotation
	var north_signature := Vector4.ZERO # storm, ascent, anvil, wind normalized
	var north_cyclonic := 0.0
	for y in GH:
		var latitude := PI * (0.5 - (float(y) + 0.5) / float(GH))
		for x in GW:
			var cell := x + y * GW
			var i := cell * CHANNELS
			max_ascent = maxf(max_ascent, convective[i])
			max_anvil = maxf(max_anvil, convective[i + 2])
			max_wind_any = maxf(max_wind_any, convective[i + 3] * 60.0)
			if absf(latitude) < deg_to_rad(5.0) or absf(latitude) > deg_to_rad(32.0):
				continue
			if products[i + 1] >= 0.0:
				max_tropical_sst_k = maxf(max_tropical_sst_k, 260.0 + products[i + 1] * 45.0)
				var signed_rotation := (diagnostics[i] - 0.5) * signf(latitude)
				var genesis_score := products[i + 2] * convective[i] \
					* maxf(1.0 - diagnostics[i + 3], 0.0) \
					* maxf(products[i + 1] - 0.70, 0.0)
				if genesis_score > best_genesis_score:
					best_genesis_score = genesis_score
					best_genesis = Vector4(products[i + 2], convective[i],
						diagnostics[i + 3], signed_rotation)
			max_tropical_closure_any = maxf(max_tropical_closure_any,
				_pressure_closure(weather, x, y) * 12000.0)
			# SST >= 295 K, organized deep convection, and a true local pressure
			# minimum are prerequisites; presentation color alone is not evidence.
			if products[i + 1] < (295.0 - 260.0) / 45.0 \
					or weather[i + 1] < 0.12 or convective[i] < 0.08 or convective[i + 2] < 0.10:
				continue
			var closure := _pressure_closure(weather, x, y)
			var signed_cyclonic := (diagnostics[i] - 0.5) * signf(latitude)
			var wind_ms := convective[i + 3] * 60.0
			if closure < 0.008 or signed_cyclonic < 0.018 or wind_ms < 10.0:
				continue
			var score := closure * 50.0 + signed_cyclonic * 4.0 \
				+ weather[i + 1] + convective[i + 2] + wind_ms / 60.0
			tropical_count += 1
			max_tropical_score = maxf(max_tropical_score, score)
			max_tropical_wind_ms = maxf(max_tropical_wind_ms, wind_ms)
			max_pressure_closure_pa = maxf(max_pressure_closure_pa, closure * 12000.0)
			if wind_ms >= 33.0 and closure >= 0.015:
				hurricane_count += 1
	var lines := _convective_components(weather, convective, GW, GH, true, 8)
	if cores[0] >= 0.0:
		var north_cell := int(cores[0])
		var north_i := north_cell * CHANNELS
		var north_y := north_cell / GW
		var north_lat := PI * (0.5 - (float(north_y) + 0.5) / float(GH))
		north_signature = Vector4(weather[north_i + 1], convective[north_i],
			convective[north_i + 2], convective[north_i + 3])
		north_cyclonic = (diagnostics[north_i] - 0.5) * signf(north_lat)
	return {
		"tropical_count": tropical_count,
		"hurricane_count": hurricane_count,
		"max_tropical_score": max_tropical_score,
		"max_tropical_wind_ms": max_tropical_wind_ms,
		"max_pressure_closure_pa": max_pressure_closure_pa,
		"line_count": lines.line_count,
		"max_line_aspect": lines.max_aspect,
		"max_line_cells": lines.max_cells,
		"max_ascent": max_ascent,
		"max_anvil": max_anvil,
		"max_wind_any": max_wind_any,
		"max_tropical_sst_k": max_tropical_sst_k,
		"max_tropical_closure_any": max_tropical_closure_any,
		"best_genesis_score": best_genesis_score,
		"best_genesis": best_genesis,
		"core_north": cores[0],
		"core_south": cores[1],
		"core_north_activity": cores[2],
		"core_south_activity": cores[3],
		"core_north_closure_pa": _pressure_closure(weather,
			int(cores[0]) % GW, int(cores[0]) / GW) * 12000.0 if cores[0] >= 0.0 else 0.0,
		"core_south_closure_pa": _pressure_closure(weather,
			int(cores[1]) % GW, int(cores[1]) / GW) * 12000.0 if cores[1] >= 0.0 else 0.0,
		"north_signature": north_signature,
		"north_cyclonic": north_cyclonic,
	}


func _pressure_closure(weather: PackedFloat32Array, x: int, y: int) -> float:
	var center := weather[(x + y * GW) * CHANNELS + 3]
	var ring_sum := 0.0
	var ring_count := 0
	# Four global cells are ~340 km: appropriate for the enclosing isobar of a
	# coarse-grid tropical cyclone. A two-cell ring sat inside the diffused core.
	for oy in range(-4, 5):
		for ox in range(-4, 5):
			if maxi(absi(ox), absi(oy)) != 4:
				continue
			var xx := (x + ox + GW) % GW
			var yy := clampi(y + oy, 0, GH - 1)
			ring_sum += weather[(xx + yy * GW) * CHANNELS + 3]
			ring_count += 1
	return ring_sum / float(ring_count) - center


func _accumulate_global(report: Dictionary, metrics: Dictionary) -> void:
	report.global_samples += 1
	if metrics.tropical_count > 0:
		report.tropical_depression_samples += 1
	if metrics.hurricane_count > 0:
		report.hurricane_samples += 1
	if metrics.line_count > 0:
		report.linear_mcs_samples += 1
	report.max_tropical_score = maxf(report.max_tropical_score, metrics.max_tropical_score)
	report.max_tropical_wind_ms = maxf(report.max_tropical_wind_ms, metrics.max_tropical_wind_ms)
	report.max_pressure_closure_pa = maxf(report.max_pressure_closure_pa, metrics.max_pressure_closure_pa)
	report.max_global_line_aspect = maxf(report.max_global_line_aspect, metrics.max_line_aspect)
	report.max_global_line_cells = maxi(report.max_global_line_cells, metrics.max_line_cells)
	report.max_global_ascent = maxf(report.max_global_ascent, metrics.max_ascent)
	report.max_global_anvil = maxf(report.max_global_anvil, metrics.max_anvil)


func _best_local_environment(fields: Dictionary) -> int:
	var weather: PackedFloat32Array = fields.weather
	var diagnostics: PackedFloat32Array = fields.diagnostics
	var products: PackedFloat32Array = fields.products
	var convective: PackedFloat32Array = fields.convective
	var best_cell := GW / 2 + GH / 2 * GW
	var best_score := -INF
	for y in range(12, GH - 12):
		var latitude := PI * (0.5 - (float(y) + 0.5) / float(GH))
		if absf(latitude) < deg_to_rad(20.0) or absf(latitude) > deg_to_rad(58.0):
			continue
		for x in GW:
			var cell := x + y * GW
			var i := cell * CHANNELS
			if products[i + 1] >= 0.0:
				continue
			var convergence := maxf(0.0, 0.5 - diagnostics[i + 1]) * 2.0
			var score := products[i + 2] * (0.20 + diagnostics[i + 3]) \
				* (0.20 + convective[i]) * (1.0 + convergence) \
				+ weather[i + 1] * 0.15
			if score > best_score:
				best_score = score
				best_cell = cell
	print("LOCAL NEST candidate cell=%d environment score=%.4f" % [best_cell, best_score])
	return best_cell


func _run_local(native: Object, start_seconds: float, hours: int, output: String) -> Dictionary:
	var samples := 0
	var cb_samples := 0
	var supercell_samples := 0
	var line_samples := 0
	var max_cb_cells := 0
	var max_rotating_cells := 0
	var max_line_aspect := 0.0
	var max_local_wind_ms := 0.0
	var max_ascent := 0.0
	var max_downdraft := 0.0
	var max_anvil := 0.0
	var max_abs_vorticity_s := 0.0
	var max_shear_ms := 0.0
	var runaway_detected := false
	var longest_cb_run := 0
	var longest_supercell_run := 0
	var cb_run := 0
	var supercell_run := 0
	var elapsed := 0.0
	var next_sample := 0.0
	var global_accumulator := 0.0
	var total_steps := int(hours * 3600.0 / LOCAL_DT)
	for step in total_steps + 1:
		if elapsed + 0.01 >= next_sample:
			var fields := _local_fields(native)
			var metrics := _measure_local(fields)
			samples += 1
			if metrics.cb_cells > 0:
				cb_samples += 1
				cb_run += 1
			else:
				cb_run = 0
			if metrics.rotating_cells > 0:
				supercell_samples += 1
				supercell_run += 1
			else:
				supercell_run = 0
			if metrics.line_count > 0:
				line_samples += 1
			longest_cb_run = maxi(longest_cb_run, cb_run)
			longest_supercell_run = maxi(longest_supercell_run, supercell_run)
			max_cb_cells = maxi(max_cb_cells, metrics.cb_cells)
			max_rotating_cells = maxi(max_rotating_cells, metrics.rotating_cells)
			max_line_aspect = maxf(max_line_aspect, metrics.max_line_aspect)
			max_local_wind_ms = maxf(max_local_wind_ms, metrics.max_wind_ms)
			max_ascent = maxf(max_ascent, metrics.max_ascent)
			max_downdraft = maxf(max_downdraft, metrics.max_downdraft)
			max_anvil = maxf(max_anvil, metrics.max_anvil)
			max_abs_vorticity_s = maxf(max_abs_vorticity_s, metrics.max_abs_vorticity_s)
			max_shear_ms = maxf(max_shear_ms, metrics.max_shear_ms)
			if metrics.cb_cells > int(LW * LH * 0.70) \
					and metrics.max_wind_ms >= 59.9 and samples >= 4:
				runaway_detected = true
			if int(round(elapsed)) % int(3.0 * 3600.0) == 0:
				_save_snapshot(fields, LW, LH, output.path_join(
					"local_hour_%03d.png" % int(elapsed / 3600.0)))
			print("SEVERE LOCAL hour %5.2f cb=%d rotating=%d line=%d aspect=%.1f wind=%.1f ascent=%.2f down=%.2f anvil=%.2f vort=%.7f shear=%.1f" % [
				elapsed / 3600.0, metrics.cb_cells, metrics.rotating_cells,
				metrics.line_count, metrics.max_line_aspect, metrics.max_wind_ms,
				metrics.max_ascent, metrics.max_downdraft, metrics.max_anvil,
				metrics.max_abs_vorticity_s, metrics.max_shear_ms])
			next_sample += LOCAL_SAMPLE_SECONDS
			if runaway_detected:
				push_warning("local nest runaway detected; stopping invalid integration early")
				break
		if step == total_steps:
			break
		_set_solar_forcing(native, start_seconds + elapsed)
		native.call(&"step_local", LOCAL_DT)
		elapsed += LOCAL_DT
		global_accumulator += LOCAL_DT
		while global_accumulator >= GLOBAL_DT:
			native.call(&"step_global", GLOBAL_DT)
			global_accumulator -= GLOBAL_DT
	return {
		"local_hours": hours,
		"local_samples": samples,
		"cumulonimbus_samples": cb_samples,
		"supercell_like_samples": supercell_samples,
		"local_squall_line_samples": line_samples,
		"max_cumulonimbus_cells": max_cb_cells,
		"max_rotating_updraft_cells": max_rotating_cells,
		"max_local_line_aspect": max_line_aspect,
		"max_local_wind_ms": max_local_wind_ms,
		"max_local_ascent": max_ascent,
		"max_local_downdraft": max_downdraft,
		"max_local_anvil": max_anvil,
		"max_local_abs_vorticity_s": max_abs_vorticity_s,
		"max_local_shear_ms": max_shear_ms,
		"longest_cumulonimbus_minutes": longest_cb_run * LOCAL_SAMPLE_SECONDS / 60.0,
		"longest_supercell_like_minutes": longest_supercell_run * LOCAL_SAMPLE_SECONDS / 60.0,
		"local_runaway_detected": runaway_detected,
	}


func _measure_local(fields: Dictionary) -> Dictionary:
	var weather: PackedFloat32Array = fields.weather
	var diagnostics: PackedFloat32Array = fields.diagnostics
	var convective: PackedFloat32Array = fields.convective
	var cb_cells := 0
	var rotating_cells := 0
	var max_wind_ms := 0.0
	var max_ascent := 0.0
	var max_downdraft := 0.0
	var max_anvil := 0.0
	var max_abs_vorticity_s := 0.0
	var max_shear_ms := 0.0
	# The 24-cell parent-nudging rim is boundary condition, not simulated storm
	# anatomy. Keep a wider guard so edge convergence cannot count as a supercell.
	for y in range(32, LH - 32):
		for x in range(32, LW - 32):
			var cell := x + y * LW
			var i := cell * CHANNELS
			max_ascent = maxf(max_ascent, convective[i])
			max_downdraft = maxf(max_downdraft, convective[i + 1])
			max_anvil = maxf(max_anvil, convective[i + 2])
			max_wind_ms = maxf(max_wind_ms, convective[i + 3] * 60.0)
			max_abs_vorticity_s = maxf(max_abs_vorticity_s,
				absf(diagnostics[i] - 0.5) * 0.0008)
			max_shear_ms = maxf(max_shear_ms, diagnostics[i + 3] * 35.0)
			var deep_convective := convective[i] >= 0.16 and convective[i + 2] >= 0.12 \
				and (weather[i + 2] >= 0.025 or convective[i + 1] >= 0.08)
			if deep_convective:
				cb_cells += 1
				# Mesocyclonic rotation typically wraps the updraft edge rather than
				# occupying the exact maximum-w pixel. At 2.2 km, a two-cell (~4.4 km)
				# collocation radius is a stricter and more physical discriminator.
				var rotating_updraft := false
				for oy in range(-2, 3):
					for ox in range(-2, 3):
						var nearby := (x + ox) + (y + oy) * LW
						var ni := nearby * CHANNELS
						if absf(diagnostics[ni] - 0.5) >= 0.025 \
								and diagnostics[ni + 3] >= 0.08:
							rotating_updraft = true
							break
					if rotating_updraft:
						break
				if rotating_updraft:
					rotating_cells += 1
	var lines := _convective_components(weather, convective, LW, LH, false, 24)
	return {
		"cb_cells": cb_cells,
		"rotating_cells": rotating_cells,
		"line_count": lines.line_count,
		"max_line_aspect": lines.max_aspect,
		"max_wind_ms": max_wind_ms,
		"max_ascent": max_ascent,
		"max_downdraft": max_downdraft,
		"max_anvil": max_anvil,
		"max_abs_vorticity_s": max_abs_vorticity_s,
		"max_shear_ms": max_shear_ms,
	}


func _convective_components(weather: PackedFloat32Array, convective: PackedFloat32Array,
		width: int, height: int, wrap_x: bool, minimum_cells: int) -> Dictionary:
	var visited := PackedByteArray()
	visited.resize(width * height)
	var line_count := 0
	var max_aspect := 0.0
	var max_cells := 0
	var queue: Array[int] = []
	for start in width * height:
		var start_x := start % width
		var start_y := start / width
		if not wrap_x and (start_x < 32 or start_x >= width - 32 \
				or start_y < 32 or start_y >= height - 32):
			visited[start] = 1
			continue
		var start_i := start * CHANNELS
		if visited[start] != 0 or weather[start_i + 1] < 0.15 \
				or convective[start_i] < 0.06:
			continue
		visited[start] = 1
		queue.clear()
		queue.push_back(start)
		var head := 0
		var points: Array[Vector2] = []
		while head < queue.size():
			var cell := queue[head]
			head += 1
			var x := cell % width
			var y := cell / width
			points.push_back(Vector2(x, y))
			var neighbours: Array[int] = []
			if wrap_x or x > 0:
				neighbours.push_back((x + width - 1) % width + y * width)
			if wrap_x or x + 1 < width:
				neighbours.push_back((x + 1) % width + y * width)
			if y > 0:
				neighbours.push_back(x + (y - 1) * width)
			if y + 1 < height:
				neighbours.push_back(x + (y + 1) * width)
			for neighbour in neighbours:
				var nx := neighbour % width
				var ny := neighbour / width
				if not wrap_x and (nx < 32 or nx >= width - 32 \
						or ny < 32 or ny >= height - 32):
					visited[neighbour] = 1
					continue
				var ni := neighbour * CHANNELS
				if visited[neighbour] == 0 and weather[ni + 1] >= 0.15 \
						and convective[ni] >= 0.06:
					visited[neighbour] = 1
					queue.push_back(neighbour)
		if points.size() < minimum_cells:
			continue
		var shape := _component_shape(points)
		max_cells = maxi(max_cells, points.size())
		max_aspect = maxf(max_aspect, shape.aspect)
		if shape.aspect >= 3.0:
			line_count += 1
	return {"line_count": line_count, "max_aspect": max_aspect, "max_cells": max_cells}


func _component_shape(points: Array[Vector2]) -> Dictionary:
	var mean := Vector2.ZERO
	for point in points:
		mean += point
	mean /= float(points.size())
	var xx := 0.0
	var xy := 0.0
	var yy := 0.0
	for point in points:
		var d := point - mean
		xx += d.x * d.x
		xy += d.x * d.y
		yy += d.y * d.y
	xx /= points.size()
	xy /= points.size()
	yy /= points.size()
	var trace := xx + yy
	var root := sqrt(maxf(0.0, (xx - yy) * (xx - yy) + 4.0 * xy * xy))
	var major := maxf((trace + root) * 0.5, 0.0)
	var minor := maxf((trace - root) * 0.5, 0.04)
	return {"aspect": sqrt(major / minor)}


func _save_snapshot(fields: Dictionary, width: int, height: int, path: String) -> void:
	var weather: PackedFloat32Array = fields.weather
	var diagnostics: PackedFloat32Array = fields.diagnostics
	var convective: PackedFloat32Array = fields.convective
	var image := Image.create(width, height * 2, false, Image.FORMAT_RGB8)
	for y in height:
		for x in width:
			var i := (x + y * width) * CHANNELS
			var base := Vector3(0.02, 0.035, 0.06)
			base += Vector3(0.62, 0.66, 0.72) * weather[i]
			base = base.lerp(Vector3(1.0, 0.12, 0.01), clampf(weather[i + 1] * 2.0, 0.0, 0.9))
			base = base.lerp(Vector3(0.03, 0.55, 1.0), clampf(weather[i + 2], 0.0, 0.9))
			image.set_pixel(x, y, Color(base.x, base.y, base.z))
			var structure := Vector3(convective[i + 1], convective[i], convective[i + 2])
			var rotation := absf(diagnostics[i] - 0.5) * 2.0
			structure = structure.lerp(Vector3(1.0, 0.0, 0.8), clampf(rotation, 0.0, 0.75))
			image.set_pixel(x, y + height, Color(structure.x, structure.y, structure.z))
	image.save_png(path)


func _global_cell_direction(cell: int) -> Vector3:
	var x := cell % GW
	var y := cell / GW
	var lat := PI * 0.5 - PI * (float(y) + 0.5) / float(GH)
	var lon := TAU * (float(x) + 0.5) / float(GW)
	return Vector3(cos(lat) * cos(lon), sin(lat), cos(lat) * sin(lon))


func _build_surface_fields() -> PackedFloat32Array:
	var packed := PackedFloat32Array()
	packed.resize(GW * GH * CHANNELS)
	for y in GH:
		var lat := PI * 0.5 - PI * (float(y) + 0.5) / float(GH)
		for x in GW:
			var lon := TAU * (float(x) + 0.5) / float(GW)
			var direction := Vector3(cos(lat) * cos(lon), sin(lat), cos(lat) * sin(lon))
			var offset := (x + y * GW) * CHANNELS
			var water := clampf(Planet.water_coverage(direction), 0.0, 1.0)
			var moisture := clampf(
				Planet.grid.sample_bilinear(Planet.fields.soil_moisture, direction), 0.0, 1.0)
			var color := Planet.surface_color(direction)
			var luminance := color.r * 0.2126 + color.g * 0.7152 + color.b * 0.0722
			packed[offset] = lerpf(Planet.macro_height(direction), Planet.water_height(direction), water)
			packed[offset + 1] = water
			packed[offset + 2] = moisture
			packed[offset + 3] = clampf(luminance, 0.04, 0.65)
	return packed


func _build_local_surface_fields(native: Object) -> PackedFloat32Array:
	var center: Vector3 = native.call(&"get_local_center")
	var east: Vector3 = native.call(&"get_local_east")
	var north: Vector3 = native.call(&"get_local_north")
	var span_m := float(native.call(&"get_local_span_m"))
	var radius_m := 3500000.0
	var cell_m := span_m / float(LW)
	var half := span_m * 0.5
	var packed := PackedFloat32Array()
	packed.resize(LW * LH * 7)
	for y in LH:
		for x in LW:
			var ex := (float(x) + 0.5) * cell_m - half
			var ny := (float(y) + 0.5) * cell_m - half
			var direction := (center + east * (ex / radius_m) + north * (ny / radius_m)).normalized()
			var water := clampf(Planet.water_coverage(direction), 0.0, 1.0)
			var moisture := clampf(
				Planet.grid.sample_bilinear(Planet.fields.soil_moisture, direction), 0.0, 1.0)
			var color := Planet.surface_color(direction)
			var luminance := color.r * 0.2126 + color.g * 0.7152 + color.b * 0.0722
			var offset := (x + y * LW) * 7
			packed[offset] = lerpf(Planet.macro_height(direction), Planet.water_height(direction), water)
			packed[offset + 1] = water
			packed[offset + 2] = moisture
			packed[offset + 3] = clampf(luminance, 0.04, 0.65)
			# Radial is a conservative fallback normal; the native nest derives
			# terrain slopes from supplied heights when a normal is absent.
			packed[offset + 4] = 0.0
			packed[offset + 5] = 0.0
			packed[offset + 6] = 0.0
	return packed


func _set_solar_forcing(native: Object, simulation_seconds: float) -> void:
	var angle := fmod(simulation_seconds * TAU / (11.5 * 3600.0), TAU)
	var declination := deg_to_rad(8.0)
	var sun_direction := Vector3(cos(declination) * cos(angle), sin(declination),
		cos(declination) * sin(angle))
	native.call(&"set_solar_forcing", sun_direction, 1420.0, deg_to_rad(0.266))


func _int_arg(name: String, fallback: int) -> int:
	var prefix := "--%s=" % name
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with(prefix):
			return int(arg.trim_prefix(prefix))
	return fallback


func _string_arg(name: String, fallback: String) -> String:
	var prefix := "--%s=" % name
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with(prefix):
			return arg.trim_prefix(prefix)
	return fallback


func _fail(message: String) -> void:
	push_error("WEATHER_SEVERE_PROBE_FAIL: %s" % message)
	get_tree().quit(1)
