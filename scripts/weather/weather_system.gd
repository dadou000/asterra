extends Node
## Runtime bridge between the AVX2 native meteorology core and GPU/cloud/UI consumers.
## The native class is resolved dynamically so an unbuilt DLL never prevents the
## project from parsing or booting.

const GLOBAL_W := 1024
const GLOBAL_H := 512
const LOCAL_W := 192
const LOCAL_H := 192
const LAYERS := 30
const TEXTURE_REAL_INTERVAL := 0.50
const GLOBAL_SIM_DT := 90.0
const LOCAL_SIM_DT := 20.0
const SIMULATION_SPEED_MIN := 0.0
const SIMULATION_SPEED_MAX := 8192.0
const SIMULATION_SPEED_DEFAULT := 1.0
const HIGH_WARP_LOCAL_THRESHOLD := 256.0
const MAX_SOLVER_STEPS_PER_FRAME := 64
const SIMULATION_WEIGHT_MIN := 0.0
const SIMULATION_WEIGHT_MAX := 2.0
const SIMULATION_WEIGHT_DEFAULT := 1.0
const NEUTRAL_CLOUD := 0.16
const TUNING_WEIGHT_MIN := 0.0
const TUNING_WEIGHT_MAX := 2.0
const TUNING_WEIGHT_DEFAULT := 1.0
const TUNING_KEYS := [
	&"circulation",
	&"temperature",
	&"humidity",
	&"cloud_microphysics",
	&"convection",
	&"precipitation",
]
const DEFAULT_LAYER_WEIGHTS := [0.700583, 0.685757, 0.859964, 1.018795, 1.159806, 1.281069, 1.381213, 1.459443, 1.515535, 1.549812, 1.563101, 1.556674, 1.532182, 1.491570, 1.437001, 1.370766, 1.264000, 1.129507, 1.009324, 0.901929, 0.805962, 0.720205, 0.643574, 0.575096, 0.513904, 0.459223, 0.410361, 0.366697, 0.327680, 0.309267]
const REQUIRED_NATIVE_METHODS := [
	&"initialize",
	&"step_global",
	&"set_local_center",
	&"step_local",
	&"reset_local_from_global",
	&"get_global_weather_rgba",
	&"get_local_weather_rgba",
	&"get_global_diagnostics_rgba",
	&"get_local_diagnostics_rgba",
	&"get_global_products_rgba",
	&"get_local_products_rgba",
	&"set_tuning_weight",
	&"get_tuning_weight",
	&"reset_tuning_weights",
	&"set_layer_weight",
	&"get_layer_weights",
	&"get_layer_count",
	&"get_global_width",
	&"get_global_height",
	&"get_layer_height_m",
	&"get_local_center",
	&"get_local_east",
	&"get_local_north",
	&"get_local_span_m",
]

signal simulation_weight_changed(weight: float)
signal simulation_speed_changed(speed: float)
signal tuning_weight_changed(name: StringName, weight: float)
signal layer_weight_changed(layer: int, weight: float)
signal physics_tuning_reset

var global_weather_texture: ImageTexture
var local_weather_texture: ImageTexture
var global_diagnostics_texture: ImageTexture
var local_diagnostics_texture: ImageTexture
var global_products_texture: ImageTexture
var local_products_texture: ImageTexture
var global_weather_values := PackedFloat32Array()
var global_diagnostics_values := PackedFloat32Array()
var global_products_values := PackedFloat32Array()
var local_center := Vector3.UP
var local_east := Vector3.RIGHT
var local_north := Vector3.FORWARD
var local_span_m := 422400.0
var layer_count := LAYERS

var native_available := false
var backend_error := ""
var simulation_weight := SIMULATION_WEIGHT_DEFAULT
var simulation_speed := SIMULATION_SPEED_DEFAULT
var warped_ahead_seconds := 0.0
var tuning_weights := {
	&"circulation": TUNING_WEIGHT_DEFAULT,
	&"temperature": TUNING_WEIGHT_DEFAULT,
	&"humidity": TUNING_WEIGHT_DEFAULT,
	&"cloud_microphysics": TUNING_WEIGHT_DEFAULT,
	&"convection": TUNING_WEIGHT_DEFAULT,
	&"precipitation": TUNING_WEIGHT_DEFAULT,
}
var layer_weights := PackedFloat32Array(DEFAULT_LAYER_WEIGHTS)

var _native: Object = null
var _observer: AsterraPlayer
var _global_sim_accum := 0.0
var _local_sim_accum := 0.0
var _texture_accum := 0.0
var _last_celestial_seconds := 0.0


func _ready() -> void:
	_last_celestial_seconds = CelestialSystem.simulation_seconds
	CelestialSystem.set_time_scale(simulation_speed)
	_create_textures()
	_try_create_native_backend()
	if native_available:
		_publish_weather_textures()
	else:
		_publish_fallback_weather()


func _try_create_native_backend() -> void:
	if not ClassDB.class_exists(&"WeatherNative"):
		backend_error = "AVX2 weather extension is not loaded; build native/weather first"
		push_warning("WeatherSystem: %s" % backend_error)
		return
	var instance: Variant = ClassDB.instantiate(&"WeatherNative")
	if not (instance is Object):
		backend_error = "WeatherNative registered but could not be instantiated"
		push_error("WeatherSystem: %s" % backend_error)
		return
	var missing_methods := PackedStringArray()
	for method: StringName in REQUIRED_NATIVE_METHODS:
		if not instance.has_method(method):
			missing_methods.append(String(method))
	if not missing_methods.is_empty():
		backend_error = (
			"WeatherNative binary is out of date; rebuild native/weather and restart Godot. "
			+ "Missing methods: %s" % ", ".join(missing_methods)
		)
		push_warning("WeatherSystem: %s" % backend_error)
		return
	var native_w := int(instance.call(&"get_global_width"))
	var native_h := int(instance.call(&"get_global_height"))
	var native_layers := int(instance.call(&"get_layer_count"))
	if native_w != GLOBAL_W or native_h != GLOBAL_H or native_layers != LAYERS:
		backend_error = "WeatherNative grid mismatch: DLL reports %dx%dx%d, project expects %dx%dx%d; rebuild native/weather" % [native_w, native_h, native_layers, GLOBAL_W, GLOBAL_H, LAYERS]
		push_warning("WeatherSystem: %s" % backend_error)
		return
	_native = instance
	for tuning_key: StringName in TUNING_KEYS:
		_native.call(&"set_tuning_weight", tuning_key, float(tuning_weights[tuning_key]))
	for layer in LAYERS:
		_native.call(&"set_layer_weight", layer, layer_weights[layer])
	var world := load("res://world.tres")
	var seed := 1
	if world is GenConfig:
		seed = int(world.world_seed)
	_native.call(&"initialize", seed)
	var layers_result: Variant = _native.call(&"get_layer_count")
	if layers_result is int:
		layer_count = int(layers_result)
	native_available = true
	backend_error = ""


func _process(delta: float) -> void:
	_try_bind_observer()
	var celestial_now := CelestialSystem.simulation_seconds
	var simulated_delta := celestial_now - _last_celestial_seconds
	_last_celestial_seconds = celestial_now
	if simulated_delta < 0.0:
		_global_sim_accum = 0.0
		_local_sim_accum = 0.0
		simulated_delta = 0.0
	warped_ahead_seconds += maxf(simulated_delta - delta, 0.0)

	if not native_available or _native == null:
		if _observer != null and is_instance_valid(_observer):
			_update_fallback_basis(_observer.up_dir())
		_sync_weather_map_material()
		return

	_global_sim_accum += simulated_delta
	if _observer != null and is_instance_valid(_observer):
		_local_sim_accum += simulated_delta
	else:
		_local_sim_accum = 0.0
	_texture_accum += delta

	var global_dt := GLOBAL_SIM_DT
	var global_steps := 0
	while _global_sim_accum >= global_dt and global_steps < MAX_SOLVER_STEPS_PER_FRAME:
		_global_sim_accum -= global_dt
		_native.call(&"step_global", global_dt)
		global_steps += 1

	if _observer != null and is_instance_valid(_observer):
		_native.call(&"set_local_center", _observer.up_dir())
		if simulation_speed > HIGH_WARP_LOCAL_THRESHOLD:
			_local_sim_accum = 0.0
		else:
			var local_dt := 40.0 if simulation_speed >= 32.0 else LOCAL_SIM_DT
			var local_steps := 0
			while _local_sim_accum >= local_dt and local_steps < MAX_SOLVER_STEPS_PER_FRAME:
				_local_sim_accum -= local_dt
				_native.call(&"step_local", local_dt)
				local_steps += 1
		local_center = _native.call(&"get_local_center")
		local_east = _native.call(&"get_local_east")
		local_north = _native.call(&"get_local_north")
		local_span_m = float(_native.call(&"get_local_span_m"))

	if _texture_accum >= TEXTURE_REAL_INTERVAL:
		_texture_accum = fmod(_texture_accum, TEXTURE_REAL_INTERVAL)
		_publish_weather_textures()
	_sync_weather_map_material()


func _try_bind_observer() -> void:
	if _observer != null and is_instance_valid(_observer):
		return
	var root := get_tree().current_scene
	if root == null:
		return
	for child in root.get_children():
		if child is AsterraPlayer:
			_observer = child as AsterraPlayer
			return


func _update_fallback_basis(direction: Vector3) -> void:
	local_center = direction.normalized()
	var pole := Vector3.RIGHT if absf(local_center.y) > 0.92 else Vector3.UP
	local_east = pole.cross(local_center).normalized()
	local_north = local_center.cross(local_east).normalized()


func _create_textures() -> void:
	global_weather_texture = ImageTexture.create_from_image(
		Image.create(GLOBAL_W, GLOBAL_H, false, Image.FORMAT_RGBAF))
	local_weather_texture = ImageTexture.create_from_image(
		Image.create(LOCAL_W, LOCAL_H, false, Image.FORMAT_RGBAF))
	global_diagnostics_texture = ImageTexture.create_from_image(
		Image.create(GLOBAL_W, GLOBAL_H, false, Image.FORMAT_RGBAF))
	local_diagnostics_texture = ImageTexture.create_from_image(
		Image.create(LOCAL_W, LOCAL_H, false, Image.FORMAT_RGBAF))
	global_products_texture = ImageTexture.create_from_image(
		Image.create(GLOBAL_W, GLOBAL_H, false, Image.FORMAT_RGBAF))
	local_products_texture = ImageTexture.create_from_image(
		Image.create(LOCAL_W, LOCAL_H, false, Image.FORMAT_RGBAF))


func _publish_fallback_weather() -> void:
	var global_image := Image.create(GLOBAL_W, GLOBAL_H, false, Image.FORMAT_RGBAF)
	global_image.fill(Color(NEUTRAL_CLOUD, 0.0, 0.0, 0.5))
	global_weather_texture.update(global_image)
	var local_image := Image.create(LOCAL_W, LOCAL_H, false, Image.FORMAT_RGBAF)
	local_image.fill(Color(NEUTRAL_CLOUD, 0.0, 0.0, 0.5))
	local_weather_texture.update(local_image)
	var global_diag := Image.create(GLOBAL_W, GLOBAL_H, false, Image.FORMAT_RGBAF)
	global_diag.fill(Color(0.5, 0.5, 0.5, 0.0))
	global_diagnostics_texture.update(global_diag)
	var local_diag := Image.create(LOCAL_W, LOCAL_H, false, Image.FORMAT_RGBAF)
	local_diag.fill(Color(0.5, 0.5, 0.5, 0.0))
	local_diagnostics_texture.update(local_diag)
	var global_products := Image.create(GLOBAL_W, GLOBAL_H, false, Image.FORMAT_RGBAF)
	global_products.fill(Color(0.68, -1.0, 0.0, 0.0))
	global_products_texture.update(global_products)
	global_products_values = global_products.get_data().to_float32_array()
	var local_products := Image.create(LOCAL_W, LOCAL_H, false, Image.FORMAT_RGBAF)
	local_products.fill(Color(0.68, -1.0, 0.0, 0.0))
	local_products_texture.update(local_products)


func _publish_weather_textures() -> void:
	if _native == null:
		return
	var global_result: Variant = _native.call(&"get_global_weather_rgba")
	if global_result is PackedFloat32Array:
		global_weather_values = global_result
		var global_values := _apply_simulation_weight(global_result)
		if global_values.size() == GLOBAL_W * GLOBAL_H * 4:
			global_weather_texture.update(Image.create_from_data(
				GLOBAL_W, GLOBAL_H, false, Image.FORMAT_RGBAF, global_values.to_byte_array()))
	var local_result: Variant = _native.call(&"get_local_weather_rgba")
	if local_result is PackedFloat32Array:
		var local_values := _apply_simulation_weight(local_result)
		if local_values.size() == LOCAL_W * LOCAL_H * 4:
			local_weather_texture.update(Image.create_from_data(
				LOCAL_W, LOCAL_H, false, Image.FORMAT_RGBAF, local_values.to_byte_array()))
	var global_diag_result: Variant = _native.call(&"get_global_diagnostics_rgba")
	if global_diag_result is PackedFloat32Array:
		global_diagnostics_values = global_diag_result
		var global_diag_values := _apply_diagnostic_weight(global_diag_result)
		if global_diag_values.size() == GLOBAL_W * GLOBAL_H * 4:
			global_diagnostics_texture.update(Image.create_from_data(
				GLOBAL_W, GLOBAL_H, false, Image.FORMAT_RGBAF, global_diag_values.to_byte_array()))
	var local_diag_result: Variant = _native.call(&"get_local_diagnostics_rgba")
	if local_diag_result is PackedFloat32Array:
		var local_diag_values := _apply_diagnostic_weight(local_diag_result)
		if local_diag_values.size() == LOCAL_W * LOCAL_H * 4:
			local_diagnostics_texture.update(Image.create_from_data(
				LOCAL_W, LOCAL_H, false, Image.FORMAT_RGBAF, local_diag_values.to_byte_array()))
	var global_products_result: Variant = _native.call(&"get_global_products_rgba")
	if global_products_result is PackedFloat32Array:
		global_products_values = global_products_result
		if global_products_values.size() == GLOBAL_W * GLOBAL_H * 4:
			global_products_texture.update(Image.create_from_data(
				GLOBAL_W, GLOBAL_H, false, Image.FORMAT_RGBAF,
				global_products_values.to_byte_array()))
	var local_products_result: Variant = _native.call(&"get_local_products_rgba")
	if local_products_result is PackedFloat32Array:
		var local_products_values: PackedFloat32Array = local_products_result
		if local_products_values.size() == LOCAL_W * LOCAL_H * 4:
			local_products_texture.update(Image.create_from_data(
				LOCAL_W, LOCAL_H, false, Image.FORMAT_RGBAF,
				local_products_values.to_byte_array()))


func set_simulation_weight(value: float) -> void:
	var sanitized := clampf(value, SIMULATION_WEIGHT_MIN, SIMULATION_WEIGHT_MAX)
	if is_equal_approx(simulation_weight, sanitized):
		return
	simulation_weight = sanitized
	if native_available:
		_publish_weather_textures()
	else:
		_publish_fallback_weather()
	_sync_weather_map_material()
	simulation_weight_changed.emit(simulation_weight)


func reset_simulation_weight() -> void:
	set_simulation_weight(SIMULATION_WEIGHT_DEFAULT)


func set_simulation_speed(value: float) -> void:
	var sanitized := clampf(value, SIMULATION_SPEED_MIN, SIMULATION_SPEED_MAX)
	if is_equal_approx(simulation_speed, sanitized) and is_equal_approx(CelestialSystem.time_scale, sanitized):
		return
	var was_high_warp := simulation_speed > HIGH_WARP_LOCAL_THRESHOLD
	simulation_speed = sanitized
	CelestialSystem.set_time_scale(simulation_speed)
	if was_high_warp and simulation_speed <= HIGH_WARP_LOCAL_THRESHOLD \
			and native_available and _native != null:
		_local_sim_accum = 0.0
		_native.call(&"reset_local_from_global")
		_publish_weather_textures()
	simulation_speed_changed.emit(simulation_speed)


func reset_simulation_speed() -> void:
	set_simulation_speed(SIMULATION_SPEED_DEFAULT)


func set_tuning_weight(name: StringName, value: float) -> void:
	if not tuning_weights.has(name):
		return
	var sanitized := clampf(value, TUNING_WEIGHT_MIN, TUNING_WEIGHT_MAX)
	if is_equal_approx(float(tuning_weights[name]), sanitized):
		return
	tuning_weights[name] = sanitized
	if native_available and _native != null:
		_native.call(&"set_tuning_weight", name, sanitized)
	tuning_weight_changed.emit(name, sanitized)


func set_layer_weight(layer: int, value: float) -> void:
	if layer < 0 or layer >= layer_weights.size():
		return
	var sanitized := clampf(value, TUNING_WEIGHT_MIN, TUNING_WEIGHT_MAX)
	if is_equal_approx(layer_weights[layer], sanitized):
		return
	layer_weights[layer] = sanitized
	if native_available and _native != null:
		_native.call(&"set_layer_weight", layer, sanitized)
		_publish_weather_textures()
	layer_weight_changed.emit(layer, sanitized)


func reset_physics_tuning() -> void:
	for tuning_key: StringName in TUNING_KEYS:
		tuning_weights[tuning_key] = TUNING_WEIGHT_DEFAULT
	layer_weights = PackedFloat32Array(DEFAULT_LAYER_WEIGHTS)
	if native_available and _native != null:
		_native.call(&"reset_tuning_weights")
		_publish_weather_textures()
	physics_tuning_reset.emit()


func _apply_simulation_weight(values: PackedFloat32Array) -> PackedFloat32Array:
	if is_equal_approx(simulation_weight, 1.0):
		return values
	var weighted := values.duplicate()
	for i in int(weighted.size() / 4):
		var offset := i * 4
		weighted[offset] = clampf(
			NEUTRAL_CLOUD + (weighted[offset] - NEUTRAL_CLOUD) * simulation_weight, 0.0, 1.0)
		weighted[offset + 1] = clampf(weighted[offset + 1] * simulation_weight, 0.0, 1.0)
		weighted[offset + 2] = clampf(weighted[offset + 2] * simulation_weight, 0.0, 1.0)
		weighted[offset + 3] = clampf(
			0.5 + (weighted[offset + 3] - 0.5) * simulation_weight, 0.0, 1.0)
	return weighted


func _apply_diagnostic_weight(values: PackedFloat32Array) -> PackedFloat32Array:
	if is_equal_approx(simulation_weight, 1.0):
		return values
	var weighted := values.duplicate()
	for i in int(weighted.size() / 4):
		var offset := i * 4
		for channel in 3:
			weighted[offset + channel] = clampf(
				0.5 + (weighted[offset + channel] - 0.5) * simulation_weight, 0.0, 1.0)
		weighted[offset + 3] = clampf(weighted[offset + 3] * simulation_weight, 0.0, 1.0)
	return weighted


func _sync_weather_map_material() -> void:
	var weather_map := get_node_or_null("/root/WeatherMap")
	if weather_map == null or not weather_map.visible:
		return
	if weather_map.has_method(&"_sync_from_weather_system"):
		weather_map.call(&"_sync_from_weather_system")
		return
	var material_value: Variant = weather_map.get("_material")
	if not (material_value is ShaderMaterial):
		return
	var material := material_value as ShaderMaterial
	if global_weather_texture != null:
		material.set_shader_parameter("u_weather", global_weather_texture)
	if local_weather_texture != null:
		material.set_shader_parameter("u_local_weather", local_weather_texture)
	if global_diagnostics_texture != null:
		material.set_shader_parameter("u_diagnostics", global_diagnostics_texture)
	if local_diagnostics_texture != null:
		material.set_shader_parameter("u_local_diagnostics", local_diagnostics_texture)
	if global_products_texture != null:
		material.set_shader_parameter("u_products", global_products_texture)
	if local_products_texture != null:
		material.set_shader_parameter("u_local_products", local_products_texture)
	material.set_shader_parameter("u_local_center", local_center)
	material.set_shader_parameter("u_local_east", local_east)
	material.set_shader_parameter("u_local_north", local_north)
	material.set_shader_parameter(
		"u_local_span_m", 0.0 if simulation_speed > HIGH_WARP_LOCAL_THRESHOLD else local_span_m)
	if Planet.cfg != null:
		material.set_shader_parameter("u_planet_radius", Planet.cfg.planet_radius)


func global_texture() -> Texture2D:
	return global_weather_texture


func local_texture() -> Texture2D:
	return local_weather_texture


func solver_backlog_seconds() -> float:
	return maxf(_global_sim_accum - GLOBAL_SIM_DT, 0.0)
