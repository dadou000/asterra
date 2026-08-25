extends Node
## Focused native weather calibration/API smoke test.
## Run `godot --headless --path . tests/WeatherNativeTuningSmoke.tscn` while the
## editor is closed, or from an isolated project containing the native runtime.


func _ready() -> void:
	var load_status := GDExtensionManager.load_extension(
		"res://bin/asterra_weather.gdextension")
	if load_status != GDExtensionManager.LOAD_STATUS_OK \
			and load_status != GDExtensionManager.LOAD_STATUS_ALREADY_LOADED:
		fail_test("extension load failed with status %d" % load_status)
		return
	if not ClassDB.class_exists(&"WeatherNative"):
		fail_test("WeatherNative is not registered")
		return

	var native: Object = ClassDB.instantiate(&"WeatherNative")
	if native == null:
		fail_test("WeatherNative could not be instantiated")
		return

	native.call(&"set_tuning_weight", &"convection", 3.5)
	if not is_equal_approx(float(native.call(&"get_tuning_weight", &"convection")), 2.0):
		fail_test("process weight did not round-trip")
		return
	native.call(&"set_layer_weight", 5, 0.42)
	var edited_layers: PackedFloat32Array = native.call(&"get_layer_weights")
	if edited_layers.size() != 6 or not is_equal_approx(edited_layers[5], 0.42):
		fail_test("layer weight did not round-trip")
		return
	native.call(&"reset_tuning_weights")
	var reset_layers: PackedFloat32Array = native.call(&"get_layer_weights")
	if not is_equal_approx(float(native.call(&"get_tuning_weight", &"convection")), 1.0) \
			or not is_equal_approx(reset_layers[0], 0.77) \
			or not is_equal_approx(reset_layers[5], 1.19):
		fail_test("Earth-like defaults did not reset")
		return

	native.call(&"initialize", 0x4153544552524100)
	# Six simulated hours is long enough to exercise horizontal dynamics,
	# microphysics, vertical exchange, fallout, and diagnostics.
	for _step in 240:
		native.call(&"step_global", 90.0)

	var weather: PackedFloat32Array = native.call(&"get_global_weather_rgba")
	var diagnostics: PackedFloat32Array = native.call(&"get_global_diagnostics_rgba")
	if weather.size() != 256 * 128 * 4 or diagnostics.size() != weather.size():
		fail_test("unexpected weather texture dimensions")
		return

	var channel_mean := PackedFloat64Array([0.0, 0.0, 0.0, 0.0])
	for i in weather.size():
		if not is_finite(weather[i]) or weather[i] < 0.0 or weather[i] > 1.0:
			fail_test("non-finite or unbounded weather value at %d" % i)
			return
		if not is_finite(diagnostics[i]) or diagnostics[i] < 0.0 or diagnostics[i] > 1.0:
			fail_test("non-finite or unbounded diagnostic value at %d" % i)
			return
		channel_mean[i % 4] += weather[i]
	var cells := weather.size() / 4
	for channel in 4:
		channel_mean[channel] /= cells
	if channel_mean[0] <= 0.30 or channel_mean[0] >= 0.90:
		fail_test("cloud field collapsed to an implausible mean %.4f" % channel_mean[0])
		return
	var weighted_pressure_anomaly := 0.0
	var area_sum := 0.0
	for cell in cells:
		var y := cell / 256
		var latitude := PI * (0.5 - (float(y) + 0.5) / 128.0)
		var area_weight := cos(latitude)
		weighted_pressure_anomaly += (weather[cell * 4 + 3] - 0.5) * area_weight
		area_sum += area_weight
	weighted_pressure_anomaly /= area_sum
	if absf(weighted_pressure_anomaly) > 0.002:
		fail_test("global pressure anomaly is not mass-centered: %.6f" % weighted_pressure_anomaly)
		return

	# Longitude converges to one physical point at either pole. The outermost
	# cell-centred rows may retain a small resolved gradient, but a full-strength
	# zonal wave here signals the old equirectangular polar singularity.
	var polar_pressure_span := 0.0
	for y in [0, 127]:
		var row_min := INF
		var row_max := -INF
		for x in 256:
			var pressure := weather[(x + y * 256) * 4 + 3]
			row_min = minf(row_min, pressure)
			row_max = maxf(row_max, pressure)
		polar_pressure_span = maxf(polar_pressure_span, row_max - row_min)
	if polar_pressure_span > 0.02:
		fail_test("polar pressure is discontinuous across longitude: span %.6f" % polar_pressure_span)
		return

	# Crossing the old reference-axis threshold used to reverse both local patch
	# axes. A transported tangent basis must remain coherent across the rebuild.
	var y0 := 0.91
	var y1 := 0.93
	native.call(&"set_local_center", Vector3(sqrt(1.0 - y0 * y0), y0, 0.0))
	var east0: Vector3 = native.call(&"get_local_east")
	native.call(&"set_local_center", Vector3(sqrt(1.0 - y1 * y1), y1, 0.0))
	var east1: Vector3 = native.call(&"get_local_east")
	if east0.dot(east1) < 0.90:
		fail_test("local tangent frame flipped near the polar threshold")
		return

	# Exercise both polar local nests after the global-to-local wind rotation.
	for pole in [Vector3.UP, Vector3.DOWN]:
		native.call(&"set_local_center", pole)
		for _step in 24:
			native.call(&"step_local", 20.0)
		var local_weather: PackedFloat32Array = native.call(&"get_local_weather_rgba")
		var local_diagnostics: PackedFloat32Array = native.call(&"get_local_diagnostics_rgba")
		if local_weather.size() != 192 * 192 * 4 or local_diagnostics.size() != local_weather.size():
			fail_test("unexpected polar local texture dimensions")
			return
		for i in local_weather.size():
			if not is_finite(local_weather[i]) or not is_finite(local_diagnostics[i]):
				fail_test("non-finite polar local state at %d" % i)
				return

	var baseline_cloud := channel_mean[0]
	for layer in 6:
		native.call(&"set_layer_weight", layer, 0.0)
	var no_layer_weather: PackedFloat32Array = native.call(&"get_global_weather_rgba")
	var no_layer_cloud := 0.0
	for cell in cells:
		no_layer_cloud += no_layer_weather[cell * 4]
	no_layer_cloud /= cells
	if baseline_cloud - no_layer_cloud <= 0.005:
		fail_test("runtime layer weights did not materially affect column cloud")
		return

	var tuning_keys := [
		&"circulation", &"temperature", &"humidity",
		&"cloud_microphysics", &"convection", &"precipitation",
	]
	native.call(&"reset_tuning_weights")
	for tuning_key: StringName in tuning_keys:
		native.call(&"set_tuning_weight", tuning_key, 2.0)
	for _step in 120:
		native.call(&"step_global", 90.0)
	for tuning_key: StringName in tuning_keys:
		native.call(&"set_tuning_weight", tuning_key, 0.0)
	for _step in 120:
		native.call(&"step_global", 90.0)
	var extreme_weather: PackedFloat32Array = native.call(&"get_global_weather_rgba")
	for i in extreme_weather.size():
		if not is_finite(extreme_weather[i]) or extreme_weather[i] < 0.0 or extreme_weather[i] > 1.0:
			fail_test("bounded tuning extremes destabilized weather at %d" % i)
			return

	print("WEATHER_TUNING_SMOKE_OK mean cloud/storm/precip/pressure = %.4f %.4f %.4f %.4f; polar span %.6f" % [
		channel_mean[0], channel_mean[1], channel_mean[2], channel_mean[3], polar_pressure_span])
	get_tree().quit(0)


func fail_test(message: String) -> void:
	push_error("WEATHER_TUNING_SMOKE_FAILED: %s" % message)
	get_tree().quit(1)
