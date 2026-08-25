extends Node
## Focused native weather calibration/API smoke test.
## Run from a project copy while the editor is closed, or from an isolated test
## project containing bin/asterra_weather.{dll,gdextension}.


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

	print("WEATHER_TUNING_SMOKE_OK mean cloud/storm/precip/pressure = %.4f %.4f %.4f %.4f" % [
		channel_mean[0], channel_mean[1], channel_mean[2], channel_mean[3]])
	get_tree().quit(0)


func fail_test(message: String) -> void:
	push_error("WEATHER_TUNING_SMOKE_FAILED: %s" % message)
	get_tree().quit(1)
