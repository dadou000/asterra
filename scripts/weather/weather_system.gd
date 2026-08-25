extends Node
## Runtime bridge between the AVX2 native meteorology core and GPU cloud rendering.

const GLOBAL_W := 256
const GLOBAL_H := 128
const LOCAL_W := 192
const LOCAL_H := 192
const GLOBAL_REAL_INTERVAL := 0.50
const LOCAL_REAL_INTERVAL := 0.20
const TEXTURE_REAL_INTERVAL := 0.50
const GLOBAL_SIM_DT := 90.0
const LOCAL_SIM_DT := 20.0

var global_weather_texture: ImageTexture
var local_weather_texture: ImageTexture
var local_center := Vector3.UP
var local_east := Vector3.RIGHT
var local_north := Vector3.FORWARD
var local_span_m := 422400.0

var _native: WeatherNative
var _observer: AsterraPlayer
var _global_accum := 0.0
var _local_accum := 0.0
var _texture_accum := 0.0


func _ready() -> void:
	_native = WeatherNative.new()
	var world := load("res://world.tres")
	var seed := 1
	if world is GenConfig:
		seed = int(world.world_seed)
	_native.initialize(seed)
	_create_textures()
	_publish_weather_textures()


func _process(delta: float) -> void:
	_try_bind_observer()
	_global_accum += delta
	_local_accum += delta
	_texture_accum += delta

	while _global_accum >= GLOBAL_REAL_INTERVAL:
		_global_accum -= GLOBAL_REAL_INTERVAL
		_native.step_global(GLOBAL_SIM_DT)

	if _observer != null and is_instance_valid(_observer):
		_native.set_local_center(_observer.up_dir())
		while _local_accum >= LOCAL_REAL_INTERVAL:
			_local_accum -= LOCAL_REAL_INTERVAL
			_native.step_local(LOCAL_SIM_DT)
		local_center = _native.get_local_center()
		local_east = _native.get_local_east()
		local_north = _native.get_local_north()
		local_span_m = _native.get_local_span_m()

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


func _create_textures() -> void:
	var global_image := Image.create(GLOBAL_W, GLOBAL_H, false, Image.FORMAT_RGBAF)
	global_weather_texture = ImageTexture.create_from_image(global_image)
	var local_image := Image.create(LOCAL_W, LOCAL_H, false, Image.FORMAT_RGBAF)
	local_weather_texture = ImageTexture.create_from_image(local_image)


func _publish_weather_textures() -> void:
	if _native == null:
		return
	var global_values := _native.get_global_weather_rgba()
	if global_values.size() == GLOBAL_W * GLOBAL_H * 4:
		var global_image := Image.create_from_data(
			GLOBAL_W, GLOBAL_H, false, Image.FORMAT_RGBAF, global_values.to_byte_array())
		global_weather_texture.update(global_image)
	var local_values := _native.get_local_weather_rgba()
	if local_values.size() == LOCAL_W * LOCAL_H * 4:
		var local_image := Image.create_from_data(
			LOCAL_W, LOCAL_H, false, Image.FORMAT_RGBAF, local_values.to_byte_array())
		local_weather_texture.update(local_image)


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
