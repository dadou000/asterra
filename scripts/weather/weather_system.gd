extends Node
## Runtime bridge between the AVX2 native meteorology core and GPU/cloud/UI consumers.
## The native class is resolved dynamically so an unbuilt DLL never prevents the
## project from parsing or booting.

const GLOBAL_W := 256
const GLOBAL_H := 128
const LOCAL_W := 192
const LOCAL_H := 192
const LAYERS := 6
const GLOBAL_REAL_INTERVAL := 0.50
const LOCAL_REAL_INTERVAL := 0.20
const TEXTURE_REAL_INTERVAL := 0.50
const GLOBAL_SIM_DT := 90.0
const LOCAL_SIM_DT := 20.0
const SIMULATION_WEIGHT_MIN := 0.0
const SIMULATION_WEIGHT_MAX := 2.0
const SIMULATION_WEIGHT_DEFAULT := 1.0
const NEUTRAL_CLOUD := 0.16
const REQUIRED_NATIVE_METHODS := [
	&"initialize",
	&"step_global",
	&"set_local_center",
	&"step_local",
	&"get_global_weather_rgba",
	&"get_local_weather_rgba",
	&"get_global_diagnostics_rgba",
	&"get_local_diagnostics_rgba",
	&"get_layer_count",
	&"get_local_center",
	&"get_local_east",
	&"get_local_north",
	&"get_local_span_m",
]

signal simulation_weight_changed(weight: float)

var global_weather_texture: ImageTexture
var local_weather_texture: ImageTexture
var global_diagnostics_texture: ImageTexture
var local_diagnostics_texture: ImageTexture
var local_center := Vector3.UP
var local_east := Vector3.RIGHT
var local_north := Vector3.FORWARD
var local_span_m := 422400.0
var layer_count := LAYERS

var native_available := false
var backend_error := ""
## Runtime influence of simulated anomalies on weather consumers.
## 0 = neutral field, 1 = calibrated model, 2 = amplified display/renderer extremes.
var simulation_weight := SIMULATION_WEIGHT_DEFAULT

var _native: Object = null
var _observer: AsterraPlayer
var _global_accum := 0.0
var _local_accum := 0.0
var _texture_accum := 0.0


func _ready() -> void:
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

	_native = instance
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

	if not native_available or _native == null:
		if _observer != null and is_instance_valid(_observer):
			_update_fallback_basis(_observer.up_dir())
		_sync_weather_map_material()
		return

	_global_accum += delta
	_local_accum += delta
	_texture_accum += delta

	while _global_accum >= GLOBAL_REAL_INTERVAL:
		_global_accum -= GLOBAL_REAL_INTERVAL
		_native.call(&"step_global", GLOBAL_SIM_DT)

	if _observer != null and is_instance_valid(_observer):
		_native.call(&"set_local_center", _observer.up_dir())
		while _local_accum >= LOCAL_REAL_INTERVAL:
			_local_accum -= LOCAL_REAL_INTERVAL
			_native.call(&"step_local", LOCAL_SIM_DT)
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


func _publish_weather_textures() -> void:
	if _native == null:
		return

	var global_result: Variant = _native.call(&"get_global_weather_rgba")
	if global_result is PackedFloat32Array:
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


func _apply_simulation_weight(values: PackedFloat32Array) -> PackedFloat32Array:
	if is_equal_approx(simulation_weight, 1.0):
		return values
	var weighted := values.duplicate()
	for i in int(weighted.size() / 4):
		var offset := i * 4
		weighted[offset] = clampf(
			NEUTRAL_CLOUD + (weighted[offset] - NEUTRAL_CLOUD) * simulation_weight,
			0.0, 1.0)
		weighted[offset + 1] = clampf(weighted[offset + 1] * simulation_weight, 0.0, 1.0)
		weighted[offset + 2] = clampf(weighted[offset + 2] * simulation_weight, 0.0, 1.0)
		weighted[offset + 3] = clampf(
			0.5 + (weighted[offset + 3] - 0.5) * simulation_weight,
			0.0, 1.0)
	return weighted


func _apply_diagnostic_weight(values: PackedFloat32Array) -> PackedFloat32Array:
	if is_equal_approx(simulation_weight, 1.0):
		return values
	var weighted := values.duplicate()
	for i in int(weighted.size() / 4):
		var offset := i * 4
		# Vorticity/divergence/PV are signed channels encoded around neutral 0.5.
		for channel in 3:
			weighted[offset + channel] = clampf(
				0.5 + (weighted[offset + channel] - 0.5) * simulation_weight,
				0.0, 1.0)
		weighted[offset + 3] = clampf(weighted[offset + 3] * simulation_weight, 0.0, 1.0)
	return weighted


func _sync_weather_map_material() -> void:
	var weather_map := get_node_or_null("/root/WeatherMap")
	if weather_map == null or not weather_map.visible:
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
	material.set_shader_parameter("u_local_center", local_center)
	material.set_shader_parameter("u_local_east", local_east)
	material.set_shader_parameter("u_local_north", local_north)
	material.set_shader_parameter("u_local_span_m", local_span_m)
	if Planet.cfg != null:
		material.set_shader_parameter("u_planet_radius", Planet.cfg.planet_radius)


func global_texture() -> Texture2D:
	return global_weather_texture


func local_texture() -> Texture2D:
	return local_weather_texture
