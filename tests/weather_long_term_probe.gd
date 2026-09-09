extends Node
## Long-duration native-weather stability/structure probe.
##
## Run with:
##   godot --headless --path . tests/WeatherLongTermProbe.tscn -- --days=30
## Composite snapshots place rendered weather above dynamics diagnostics.

const WIDTH := 256
const HEIGHT := 128
const CHANNELS := 4
const STEP_SECONDS := 90.0
const STEPS_PER_DAY := int(86400.0 / STEP_SECONDS)


func _ready() -> void:
	await get_tree().process_frame
	var days := _int_arg("days", 30)
	var snapshot_every := maxi(_int_arg("snapshot-every", 3), 1)
	var seed := _int_arg("seed", 0x41535445)
	var output_arg := _string_arg("output", "res://tests/weather_probe_output")
	var output_dir := ProjectSettings.globalize_path(output_arg)
	if DirAccess.make_dir_recursive_absolute(output_dir) != OK:
		_fail("could not create output directory %s" % output_dir)
		return

	var load_status := GDExtensionManager.load_extension("res://bin/asterra_weather.gdextension")
	if load_status != GDExtensionManager.LOAD_STATUS_OK \
			and load_status != GDExtensionManager.LOAD_STATUS_ALREADY_LOADED:
		_fail("extension load failed with status %d" % load_status)
		return
	var native: Object = ClassDB.instantiate(&"WeatherNative")
	if native == null:
		_fail("WeatherNative could not be instantiated")
		return
	var cfg := GenConfig.new()
	cfg.face_res = 48
	cfg.erosion_iterations = 12
	cfg.world_seed = seed
	var fields := PlanetBake.new(cfg).bake(Callable(), false)
	Planet.adopt(fields)
	native.call(&"initialize", seed)
	native.call(&"set_global_surface_fields", _build_surface_fields())
	_set_solar_forcing(native, 0.0)

	var csv := FileAccess.open(output_dir.path_join("metrics.csv"), FileAccess.WRITE)
	if csv == null:
		_fail("could not create metrics.csv")
		return
	csv.store_csv_line(PackedStringArray([
		"day", "cloud_mean", "cloud_std", "storm_mean", "storm_std",
		"precip_mean", "precip_active", "pressure_std", "vorticity_abs",
		"divergence_abs", "cloud_gradient", "storm_gradient",
		"storm_components", "largest_storm", "storm_max", "precip_max",
	]))

	_record(native, csv, output_dir, 0, true)
	for day in range(1, days + 1):
		for step_in_day in STEPS_PER_DAY:
			var simulation_seconds := (float(day - 1) * STEPS_PER_DAY + step_in_day) * STEP_SECONDS
			_set_solar_forcing(native, simulation_seconds)
			native.call(&"step_global", STEP_SECONDS)
		_record(native, csv, output_dir, day, day % snapshot_every == 0 or day == days)

	csv.close()
	print("WEATHER_LONG_TERM_OK %d days; output=%s" % [days, output_dir])
	get_tree().quit(0)


func _build_surface_fields() -> PackedFloat32Array:
	var packed := PackedFloat32Array()
	packed.resize(WIDTH * HEIGHT * 4)
	for y in HEIGHT:
		var lat := PI * 0.5 - PI * (float(y) + 0.5) / float(HEIGHT)
		var sin_lat := sin(lat)
		var cos_lat := cos(lat)
		for x in WIDTH:
			var lon := TAU * (float(x) + 0.5) / float(WIDTH)
			var direction := Vector3(cos_lat * cos(lon), sin_lat, cos_lat * sin(lon))
			var offset := (x + y * WIDTH) * 4
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


func _set_solar_forcing(native: Object, simulation_seconds: float) -> void:
	# Asterra's body-fixed 11.5-hour day. A small fixed northern declination keeps
	# the probe deterministic while exercising the full land/ocean energy cycle.
	var angle := fmod(simulation_seconds * TAU / (11.5 * 3600.0), TAU)
	var declination := deg_to_rad(8.0)
	var sun_direction := Vector3(
		cos(declination) * cos(angle), sin(declination),
		cos(declination) * sin(angle))
	native.call(&"set_solar_forcing", sun_direction, 1420.0, deg_to_rad(0.266))


func _record(native: Object, csv: FileAccess, output_dir: String, day: int, save_snapshot: bool) -> void:
	var weather: PackedFloat32Array = native.call(&"get_global_weather_rgba")
	var diagnostics: PackedFloat32Array = native.call(&"get_global_diagnostics_rgba")
	if weather.size() != WIDTH * HEIGHT * CHANNELS or diagnostics.size() != weather.size():
		_fail("unexpected output size on day %d" % day)
		return
	var metrics := _measure(weather, diagnostics)
	csv.store_csv_line(PackedStringArray([
		str(day), _f(metrics.cloud_mean), _f(metrics.cloud_std),
		_f(metrics.storm_mean), _f(metrics.storm_std),
		_f(metrics.precip_mean), _f(metrics.precip_active),
		_f(metrics.pressure_std), _f(metrics.vorticity_abs),
		_f(metrics.divergence_abs), _f(metrics.cloud_gradient),
		_f(metrics.storm_gradient), str(metrics.storm_components),
		str(metrics.largest_storm), _f(metrics.storm_max), _f(metrics.precip_max),
	]))
	csv.flush()
	print("DAY %3d cloud %.3f±%.3f storm %.3f±%.3f rain %.4f active %.3f pstd %.4f vort %.4f comps %d largest %d polar p %.4f div %.4f" % [
		day, metrics.cloud_mean, metrics.cloud_std, metrics.storm_mean,
		metrics.storm_std, metrics.precip_mean, metrics.precip_active,
		metrics.pressure_std, metrics.vorticity_abs, metrics.storm_components,
		metrics.largest_storm, metrics.polar_pressure_span, metrics.polar_divergence_abs])
	if save_snapshot:
		_save_snapshot(weather, diagnostics, output_dir.path_join("day_%03d.png" % day))


func _measure(weather: PackedFloat32Array, diagnostics: PackedFloat32Array) -> Dictionary:
	var cells := WIDTH * HEIGHT
	var sums := Vector4.ZERO
	var sums_sq := Vector4.ZERO
	var cloud_gradient := 0.0
	var storm_gradient := 0.0
	var vorticity_abs := 0.0
	var divergence_abs := 0.0
	var precip_active := 0
	var storm_max := 0.0
	var precip_max := 0.0
	var polar_pressure_span := 0.0
	var polar_divergence_abs := 0.0
	var polar_cells := 0
	for y in HEIGHT:
		var polar_row_min := INF
		var polar_row_max := -INF
		for x in WIDTH:
			var cell := x + y * WIDTH
			var i := cell * CHANNELS
			var values := Vector4(weather[i], weather[i + 1], weather[i + 2], weather[i + 3])
			sums += values
			sums_sq += values * values
			storm_max = maxf(storm_max, values.y)
			precip_max = maxf(precip_max, values.z)
			if values.z > 0.03:
				precip_active += 1
			var east := ((x + 1) % WIDTH + y * WIDTH) * CHANNELS
			var south := (x + mini(y + 1, HEIGHT - 1) * WIDTH) * CHANNELS
			cloud_gradient += 0.5 * (absf(values.x - weather[east]) + absf(values.x - weather[south]))
			storm_gradient += 0.5 * (absf(values.y - weather[east + 1]) + absf(values.y - weather[south + 1]))
			vorticity_abs += absf(diagnostics[i] - 0.5)
			divergence_abs += absf(diagnostics[i + 1] - 0.5)
			if y < 8 or y >= HEIGHT - 8:
				polar_row_min = minf(polar_row_min, values.w)
				polar_row_max = maxf(polar_row_max, values.w)
				polar_divergence_abs += absf(diagnostics[i + 1] - 0.5)
				polar_cells += 1
		if y < 8 or y >= HEIGHT - 8:
			polar_pressure_span = maxf(polar_pressure_span, polar_row_max - polar_row_min)
	var mean := sums / float(cells)
	var variance := sums_sq / float(cells) - mean * mean
	var components := _storm_components(weather, 0.12)
	return {
		"cloud_mean": mean.x,
		"cloud_std": sqrt(maxf(variance.x, 0.0)),
		"storm_mean": mean.y,
		"storm_std": sqrt(maxf(variance.y, 0.0)),
		"precip_mean": mean.z,
		"precip_active": float(precip_active) / float(cells),
		"pressure_std": sqrt(maxf(variance.w, 0.0)),
		"vorticity_abs": vorticity_abs / float(cells),
		"divergence_abs": divergence_abs / float(cells),
		"cloud_gradient": cloud_gradient / float(cells),
		"storm_gradient": storm_gradient / float(cells),
		"storm_components": components.count,
		"largest_storm": components.largest,
		"storm_max": storm_max,
		"precip_max": precip_max,
		"polar_pressure_span": polar_pressure_span,
		"polar_divergence_abs": polar_divergence_abs / float(polar_cells),
	}


func _storm_components(weather: PackedFloat32Array, threshold: float) -> Dictionary:
	var visited := PackedByteArray()
	visited.resize(WIDTH * HEIGHT)
	var count := 0
	var largest := 0
	var queue: Array[int] = []
	for start in WIDTH * HEIGHT:
		if visited[start] != 0 or weather[start * CHANNELS + 1] < threshold:
			continue
		visited[start] = 1
		queue.clear()
		queue.push_back(start)
		var head := 0
		var size := 0
		while head < queue.size():
			var cell := queue[head]
			head += 1
			size += 1
			var x := cell % WIDTH
			var y := cell / WIDTH
			var neighbours: Array[int] = [
				(x + WIDTH - 1) % WIDTH + y * WIDTH,
				(x + 1) % WIDTH + y * WIDTH,
				x + maxi(y - 1, 0) * WIDTH,
				x + mini(y + 1, HEIGHT - 1) * WIDTH,
			]
			for neighbour in neighbours:
				if visited[neighbour] == 0 and weather[neighbour * CHANNELS + 1] >= threshold:
					visited[neighbour] = 1
					queue.push_back(neighbour)
		if size >= 4:
			count += 1
			largest = maxi(largest, size)
	return {"count": count, "largest": largest}


func _save_snapshot(weather: PackedFloat32Array, diagnostics: PackedFloat32Array, path: String) -> void:
	var image := Image.create(WIDTH, HEIGHT * 3, false, Image.FORMAT_RGB8)
	for y in HEIGHT:
		for x in WIDTH:
			var i := (x + y * WIDTH) * CHANNELS
			var cloud := weather[i]
			var storm := weather[i + 1]
			var rain := weather[i + 2]
			var weather_color := Vector3(0.025, 0.045, 0.075)
			weather_color += Vector3(0.62, 0.68, 0.74) * cloud
			weather_color = weather_color.lerp(Vector3(1.0, 0.17, 0.025), clampf(storm * 2.2, 0.0, 0.88))
			weather_color = weather_color.lerp(Vector3(0.02, 0.62, 1.0), clampf(rain, 0.0, 0.92))
			image.set_pixel(x, y, Color(weather_color.x, weather_color.y, weather_color.z))

			var pressure := clampf((weather[i + 3] - 0.5) * 5.0, -1.0, 1.0)
			var pressure_color := Vector3(0.08, 0.08, 0.10)
			pressure_color += Vector3(
				maxf(pressure, 0.0), 0.08 * absf(pressure), maxf(-pressure, 0.0)) * 0.82
			image.set_pixel(x, y + HEIGHT, Color(
				pressure_color.x, pressure_color.y, pressure_color.z))

			var vort := (diagnostics[i] - 0.5) * 2.0
			var div := (diagnostics[i + 1] - 0.5) * 2.0
			var shear := diagnostics[i + 3]
			var diagnostic_color := Vector3(0.08, 0.08, 0.10)
			diagnostic_color += Vector3(maxf(vort, 0.0), maxf(-div, 0.0) * 0.72, maxf(-vort, 0.0)) * 0.92
			diagnostic_color += Vector3(shear, shear, shear) * 0.22
			image.set_pixel(x, y + HEIGHT * 2, Color(
				diagnostic_color.x, diagnostic_color.y, diagnostic_color.z))
	image.resize(WIDTH * 3, HEIGHT * 9, Image.INTERPOLATE_NEAREST)
	var error := image.save_png(path)
	if error != OK:
		_fail("could not save snapshot %s (error %d)" % [path, error])


func _int_arg(name: String, fallback: int) -> int:
	return int(_string_arg(name, str(fallback)))


func _string_arg(name: String, fallback: String) -> String:
	var prefix := "--%s=" % name
	for argument in OS.get_cmdline_user_args():
		if argument.begins_with(prefix):
			return argument.trim_prefix(prefix)
	return fallback


func _f(value: float) -> String:
	return "%.8f" % value


func _fail(message: String) -> void:
	push_error("WEATHER_LONG_TERM_FAILED: %s" % message)
	get_tree().quit(1)
