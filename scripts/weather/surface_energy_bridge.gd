extends Node
## Couples the procedural surface and the live Helion solution to WeatherNative.
##
## The native solver owns the evolving thermal/snow state. This bridge only
## supplies static geography once the planet bake is ready, updates the physical
## solar vector/irradiance from CelestialSystem, and publishes debug textures.

const GLOBAL_W := 256
const GLOBAL_H := 128
const LOCAL_W := 192
const LOCAL_H := 192
const TEXTURE_INTERVAL := 0.50

var global_surface_texture: ImageTexture
var local_surface_texture: ImageTexture

var _native: Object = null
var _surface_fields_uploaded := false
var _texture_accum := 0.0
var _warned_stale_backend := false


func _ready() -> void:
	# CelestialSystem emits from its -200 priority process, so this callback updates
	# Helion forcing before WeatherSystem advances the atmosphere at normal priority.
	process_priority = 10
	_create_textures()
	_publish_fallback()
	if not Planet.world_ready.is_connected(_on_planet_world_ready):
		Planet.world_ready.connect(_on_planet_world_ready)
	if not CelestialSystem.state_updated.is_connected(_on_celestial_state_updated):
		CelestialSystem.state_updated.connect(_on_celestial_state_updated)
	_bind_native()
	_push_solar_forcing()
	if Planet.ready_state:
		call_deferred("_upload_global_surface_fields")


func _process(delta: float) -> void:
	if _native == null or not is_instance_valid(_native):
		_bind_native()
		if _native == null:
			return
		_push_solar_forcing()
		if Planet.ready_state and not _surface_fields_uploaded:
			_upload_global_surface_fields()

	_texture_accum += delta
	if _texture_accum >= TEXTURE_INTERVAL:
		_texture_accum = fmod(_texture_accum, TEXTURE_INTERVAL)
		_publish_surface_textures()


func _bind_native() -> void:
	var candidate: Variant = WeatherSystem.get("_native")
	if not (candidate is Object):
		_native = null
		return
	var object := candidate as Object
	for method: StringName in [
		&"set_global_surface_fields",
		&"set_solar_forcing",
		&"get_global_surface_rgba",
		&"get_local_surface_rgba",
	]:
		if not object.has_method(method):
			_native = null
			if not _warned_stale_backend:
				_warned_stale_backend = true
				push_warning(
					"SurfaceEnergy: WeatherNative is older than the coupled surface model; "
					+ "rebuild native/weather and restart Godot")
			return
	_native = object
	_warned_stale_backend = false


func _on_celestial_state_updated(_simulation_seconds: float) -> void:
	if _native == null or not is_instance_valid(_native):
		_bind_native()
	_push_solar_forcing()


func _push_solar_forcing() -> void:
	if _native == null:
		return
	_native.call(
		&"set_solar_forcing",
		CelestialSystem.sun_dir_body(),
		CelestialSystem.asterra_irradiance_w_m2())


func _on_planet_world_ready(_fields: PlanetFields) -> void:
	_surface_fields_uploaded = false
	call_deferred("_upload_global_surface_fields")


func _upload_global_surface_fields() -> void:
	if _native == null or not is_instance_valid(_native):
		_bind_native()
	if _native == null or not Planet.ready_state or Planet.grid == null or Planet.fields == null:
		return

	# Four floats per global meteorology cell:
	# elevation [m], free-water fraction, soil moisture, snow-free land albedo.
	var packed := PackedFloat32Array()
	packed.resize(GLOBAL_W * GLOBAL_H * 4)
	for y in GLOBAL_H:
		var lat := PI * 0.5 - PI * (float(y) + 0.5) / float(GLOBAL_H)
		var sin_lat := sin(lat)
		var cos_lat := cos(lat)
		for x in GLOBAL_W:
			var lon := TAU * (float(x) + 0.5) / float(GLOBAL_W)
			var direction := Vector3(
				cos_lat * cos(lon),
				sin_lat,
				cos_lat * sin(lon))
			var offset := (x + y * GLOBAL_W) * 4
			var water := clampf(Planet.water_coverage(direction), 0.0, 1.0)
			var moisture := clampf(
				Planet.grid.sample_bilinear(Planet.fields.soil_moisture, direction),
				0.0, 1.0)
			var color := Planet.surface_color(direction)
			var luminance := color.r * 0.2126 + color.g * 0.7152 + color.b * 0.0722
			packed[offset + 0] = Planet.macro_height(direction)
			packed[offset + 1] = water
			packed[offset + 2] = moisture
			packed[offset + 3] = clampf(luminance, 0.04, 0.65)

	_native.call(&"set_global_surface_fields", packed)
	_surface_fields_uploaded = true
	_push_solar_forcing()
	_publish_surface_textures()


func _create_textures() -> void:
	global_surface_texture = ImageTexture.create_from_image(
		Image.create(GLOBAL_W, GLOBAL_H, false, Image.FORMAT_RGBAF))
	local_surface_texture = ImageTexture.create_from_image(
		Image.create(LOCAL_W, LOCAL_H, false, Image.FORMAT_RGBAF))


func _publish_fallback() -> void:
	# R temperature (220..330 K), G snow cover, B albedo, A wetness/water.
	var neutral_temp := (288.0 - 220.0) / 110.0
	var global_image := Image.create(GLOBAL_W, GLOBAL_H, false, Image.FORMAT_RGBAF)
	global_image.fill(Color(neutral_temp, 0.0, 0.18, 0.5))
	global_surface_texture.update(global_image)
	var local_image := Image.create(LOCAL_W, LOCAL_H, false, Image.FORMAT_RGBAF)
	local_image.fill(Color(neutral_temp, 0.0, 0.18, 0.5))
	local_surface_texture.update(local_image)


func _publish_surface_textures() -> void:
	if _native == null or not _surface_fields_uploaded:
		return
	var global_result: Variant = _native.call(&"get_global_surface_rgba")
	if global_result is PackedFloat32Array:
		var global_values: PackedFloat32Array = global_result
		if global_values.size() == GLOBAL_W * GLOBAL_H * 4:
			global_surface_texture.update(Image.create_from_data(
				GLOBAL_W, GLOBAL_H, false, Image.FORMAT_RGBAF,
				global_values.to_byte_array()))
	var local_result: Variant = _native.call(&"get_local_surface_rgba")
	if local_result is PackedFloat32Array:
		var local_values: PackedFloat32Array = local_result
		if local_values.size() == LOCAL_W * LOCAL_H * 4:
			local_surface_texture.update(Image.create_from_data(
				LOCAL_W, LOCAL_H, false, Image.FORMAT_RGBAF,
				local_values.to_byte_array()))


func global_texture() -> Texture2D:
	return global_surface_texture


func local_texture() -> Texture2D:
	return local_surface_texture
