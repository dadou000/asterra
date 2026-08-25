extends Node
## Runtime bridge between the AVX2 native meteorology core and GPU cloud rendering.
##
## Important: this script deliberately never references WeatherNative as a static
## GDScript type. The native class only exists after the GDExtension has loaded;
## keeping the bridge dynamic means a missing/unbuilt DLL cannot make the whole
## project fail to parse.

const GLOBAL_W := 256
const GLOBAL_H := 128
const LOCAL_W := 192
const LOCAL_H := 192
const GLOBAL_REAL_INTERVAL := 0.50
const LOCAL_REAL_INTERVAL := 0.20
const TEXTURE_REAL_INTERVAL := 0.50
const GLOBAL_SIM_DT := 90.0
const LOCAL_SIM_DT := 20.0
const SIMULATION_WEIGHT_MIN := 0.0
const SIMULATION_WEIGHT_MAX := 2.0
const SIMULATION_WEIGHT_DEFAULT := 1.0
const NEUTRAL_CLOUD := 0.16

signal simulation_weight_changed(weight: float)

var global_weather_texture: ImageTexture
var local_weather_texture: ImageTexture
var local_center := Vector3.UP
var local_east := Vector3.RIGHT
var local_north := Vector3.FORWARD
var local_span_m := 422400.0

var native_available := false
var backend_error := ""
## Runtime influence of simulated anomalies on every weather consumer.
## 0 = neutral mostly-clear field, 1 = calibrated model, 2 = amplified extremes.
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

	_native = instance
	var world := load("res://world.tres")
	var seed := 1
	if world is GenConfig:
		seed = int(world.world_seed)
	_native.call(&"initialize", seed)
	native_available = true
	backend_error = ""


func _process(delta: float) -> void:
	_try_bind_observer()

	if not native_available or _native == null:
		# Keep the local-domain marker attached to the player even in the neutral
		# fallback state. This also keeps the weather-map UI fully usable while the
		# native DLL is being built.
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
	var global_image := Image.create(GLOBAL_W, GLOBAL_H, false, Image.FORMAT_RGBAF)
	global_weather_texture = ImageTexture.create_from_image(global_image)
	var local_image := Image.create(LOCAL_W, LOCAL_H, false, Image.FORMAT_RGBAF)
	local_weather_texture = ImageTexture.create_from_image(local_image)


func _publish_fallback_weather() -> void:
	# Neutral, mostly-clear state. It exists only to keep every renderer sampler
	# valid when the native backend has not been compiled yet.
	var global_image := Image.create(GLOBAL_W, GLOBAL_H, false, Image.FORMAT_RGBAF)
	global_image.fill(Color(0.16, 0.0, 0.0, 0.5))
	global_weather_texture.update(global_image)
	var local_image := Image.create(LOCAL_W, LOCAL_H, false, Image.FORMAT_RGBAF)
	local_image.fill(Color(0.16, 0.0, 0.0, 0.5))
	local_weather_texture.update(local_image)


func _publish_weather_textures() -> void:
	if _native == null:
		return

	var global_result: Variant = _native.call(&"get_global_weather_rgba")
	if global_result is PackedFloat32Array:
		var global_values := _apply_simulation_weight(global_result)
		if global_values.size() == GLOBAL_W * GLOBAL_H * 4:
			var global_image := Image.create_from_data(
				GLOBAL_W, GLOBAL_H, false, Image.FORMAT_RGBAF, global_values.to_byte_array())
			global_weather_texture.update(global_image)

	var local_result: Variant = _native.call(&"get_local_weather_rgba")
	if local_result is PackedFloat32Array:
		var local_values := _apply_simulation_weight(local_result)
		if local_values.size() == LOCAL_W * LOCAL_H * 4:
			var local_image := Image.create_from_data(
				LOCAL_W, LOCAL_H, false, Image.FORMAT_RGBAF, local_values.to_byte_array())
			local_weather_texture.update(local_image)


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
