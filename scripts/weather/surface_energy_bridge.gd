extends Node
## Couples procedural terrain and live Helion forcing to WeatherNative.
##
## Global geography is uploaded once. The moving 2.2 km weather nest gets a
## separate high-resolution surface build from the same procedural terrain used
## by rendering/collision, including sparse player terrain edits. Native code owns
## all evolving temperature, moisture, snow and horizon state.

const GLOBAL_W := 1024
const GLOBAL_H := 512
const LOCAL_W := 192
const LOCAL_H := 192
const LOCAL_CELL_M := 2200.0
const LOCAL_NORMAL_SAMPLE_M := 600.0
const TEXTURE_INTERVAL := 0.50

var global_surface_texture: ImageTexture
var local_surface_texture: ImageTexture

var _native: Object = null
var _surface_fields_uploaded := false
var _global_building := false
var _global_task_id := -1
var _global_request := 0
var _texture_accum := 0.0
var _warned_stale_backend := false
var _local_request := 0
var _local_building := false
var _local_task_id := -1
var _local_uploaded_center := Vector3.ZERO
var _local_requested_center := Vector3.ZERO
var _terrain_ref: WeakRef


func _ready() -> void:
	# Celestial callbacks run synchronously from the -200 priority celestial node,
	# before WeatherSystem consumes the corresponding physical time increment.
	process_priority = 10
	_create_textures()
	_publish_fallback()
	if not Planet.world_ready.is_connected(_on_planet_world_ready):
		Planet.world_ready.connect(_on_planet_world_ready)
	if not CelestialSystem.state_updated.is_connected(_on_celestial_state_updated):
		CelestialSystem.state_updated.connect(_on_celestial_state_updated)
	if not Deltas.region_changed.is_connected(_on_terrain_region_changed):
		Deltas.region_changed.connect(_on_terrain_region_changed)
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

	_maybe_queue_local_surface()
	_texture_accum += delta
	if _texture_accum >= TEXTURE_INTERVAL:
		_texture_accum = fmod(_texture_accum, TEXTURE_INTERVAL)
		_publish_surface_textures()
	_sync_terrain_materials()


func _bind_native() -> void:
	var candidate: Variant = WeatherSystem.get("_native")
	if not (candidate is Object):
		_native = null
		return
	var object := candidate as Object
	for method: StringName in [
		&"set_global_surface_fields",
		&"set_local_surface_fields",
		&"set_solar_forcing",
		&"get_global_surface_rgba",
		&"get_local_surface_rgba",
	]:
		if not object.has_method(method):
			_native = null
			if not _warned_stale_backend:
				_warned_stale_backend = true
				push_warning(
					"SurfaceEnergy: WeatherNative is older than the completed surface model; "
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
		CelestialSystem.asterra_irradiance_w_m2(),
		deg_to_rad(CelestialSystem.helion_angular_diameter_deg() * 0.5))


func _on_planet_world_ready(_fields: PlanetFields) -> void:
	_surface_fields_uploaded = false
	_global_request += 1
	_local_uploaded_center = Vector3.ZERO
	_local_requested_center = Vector3.ZERO
	_local_request += 1
	call_deferred("_upload_global_surface_fields")


func _upload_global_surface_fields() -> void:
	if _native == null or not is_instance_valid(_native):
		_bind_native()
	if _native == null or not Planet.ready_state or Planet.grid == null or Planet.fields == null:
		return
	if _global_building:
		return
	_global_request += 1
	var request := _global_request
	_global_building = true
	var task := func() -> void:
		var packed := _build_global_surface_fields()
		call_deferred("_finish_global_surface_fields", request, packed)
	_global_task_id = WorkerThreadPool.add_task(task, true, "asterra_weather_surface_global")


static func _build_global_surface_fields() -> PackedFloat32Array:
	# Four floats per 21.5 km global meteorology cell:
	# elevation [m], free-water fraction, soil moisture, snow-free land albedo.
	var packed := PackedFloat32Array()
	packed.resize(GLOBAL_W * GLOBAL_H * 4)
	for y in GLOBAL_H:
		var lat := PI * 0.5 - PI * (float(y) + 0.5) / float(GLOBAL_H)
		var sin_lat := sin(lat)
		var cos_lat := cos(lat)
		for x in GLOBAL_W:
			var lon := TAU * (float(x) + 0.5) / float(GLOBAL_W)
			var direction := Vector3(cos_lat * cos(lon), sin_lat, cos_lat * sin(lon))
			var offset := (x + y * GLOBAL_W) * 4
			var water := clampf(Planet.water_coverage(direction), 0.0, 1.0)
			var moisture := clampf(Planet.grid.sample_bilinear(Planet.fields.soil_moisture, direction), 0.0, 1.0)
			var color := Planet.surface_color(direction)
			var luminance := color.r * 0.2126 + color.g * 0.7152 + color.b * 0.0722
			packed[offset + 0] = lerpf(Planet.macro_height(direction), Planet.water_height(direction), water)
			packed[offset + 1] = water
			packed[offset + 2] = moisture
			packed[offset + 3] = clampf(luminance, 0.04, 0.65)
	return packed


func _finish_global_surface_fields(request: int, packed: PackedFloat32Array) -> void:
	if _global_task_id >= 0:
		WorkerThreadPool.wait_for_task_completion(_global_task_id)
		_global_task_id = -1
	_global_building = false
	if request != _global_request or _native == null or not is_instance_valid(_native):
		return
	_native.call(&"set_global_surface_fields", packed)
	_surface_fields_uploaded = true
	_local_uploaded_center = Vector3.ZERO
	_local_requested_center = Vector3.ZERO
	_local_request += 1
	_push_solar_forcing()
	_publish_surface_textures()


func _has_local_observer() -> bool:
	var observer: Variant = WeatherSystem.get("_observer")
	return observer is Object and is_instance_valid(observer as Object)


func _maybe_queue_local_surface() -> void:
	if not _surface_fields_uploaded or _native == null or not Planet.ready_state or not _has_local_observer():
		return
	var center: Vector3 = WeatherSystem.local_center.normalized()
	if center.length_squared() < 0.5:
		return
	if _local_requested_center.length_squared() >= 0.5:
		var requested_m := acos(clampf(_local_requested_center.dot(center), -1.0, 1.0)) \
			* Planet.cfg.planet_radius
		if requested_m <= LOCAL_CELL_M * 0.25:
			return
	if _local_uploaded_center.length_squared() >= 0.5:
		var uploaded_m := acos(clampf(_local_uploaded_center.dot(center), -1.0, 1.0)) \
			* Planet.cfg.planet_radius
		if uploaded_m <= LOCAL_CELL_M * 0.25:
			return
	_queue_local_surface(false)


func _queue_local_surface(force_rebuild: bool = true) -> void:
	if not _has_local_observer() or Planet.cfg == null:
		return
	var center := WeatherSystem.local_center.normalized()
	if not force_rebuild and _local_requested_center.length_squared() >= 0.5:
		var duplicate_m := acos(clampf(_local_requested_center.dot(center), -1.0, 1.0)) \
			* Planet.cfg.planet_radius
		if duplicate_m <= LOCAL_CELL_M * 0.25:
			return
	_local_request += 1
	_local_requested_center = center
	if _local_building:
		return
	_start_local_surface_build()


func _start_local_surface_build() -> void:
	if not Planet.ready_state or Planet.cfg == null or not _has_local_observer():
		return
	var center := WeatherSystem.local_center.normalized()
	var east := WeatherSystem.local_east.normalized()
	var north := WeatherSystem.local_north.normalized()
	var radius := Planet.cfg.planet_radius
	var half_span := LOCAL_CELL_M * float(LOCAL_W) * 0.5
	var snapshot := Deltas.snapshot_for_bounds(center, half_span / radius * 1.55)
	var request := _local_request
	_local_building = true
	var task := func() -> void:
		var fields := _build_local_surface_fields(center, east, north, radius, snapshot)
		call_deferred("_finish_local_surface_build", request, center, fields)
	_local_task_id = WorkerThreadPool.add_task(task, true, "asterra_weather_surface_local")


static func _surface_height(direction: Vector3, detail: TerrainDetail, snapshot: Dictionary) -> float:
	var water := clampf(Planet.water_coverage(direction), 0.0, 1.0)
	var terrain_h := Planet.terrain_height(direction, detail, snapshot)
	return lerpf(terrain_h, Planet.water_height(direction), water)


static func _geodesic_offset(direction: Vector3, tangent: Vector3, distance_m: float, radius: float) -> Vector3:
	var angle := distance_m / maxf(radius, 1.0)
	return (direction * cos(angle) + tangent * sin(angle)).normalized()


static func _local_surface_normal(direction: Vector3, radius: float, detail: TerrainDetail,
		snapshot: Dictionary, water: float) -> Vector3:
	if water >= 0.98:
		return direction
	var basis := CubeSphere.tangent_basis(direction)
	var east: Vector3 = basis[0]
	var north: Vector3 = basis[1]
	var de := _geodesic_offset(direction, east, LOCAL_NORMAL_SAMPLE_M, radius)
	var dw := _geodesic_offset(direction, -east, LOCAL_NORMAL_SAMPLE_M, radius)
	var dn := _geodesic_offset(direction, north, LOCAL_NORMAL_SAMPLE_M, radius)
	var ds := _geodesic_offset(direction, -north, LOCAL_NORMAL_SAMPLE_M, radius)
	var pe := de * (radius + _surface_height(de, detail, snapshot))
	var pw := dw * (radius + _surface_height(dw, detail, snapshot))
	var pn := dn * (radius + _surface_height(dn, detail, snapshot))
	var ps := ds * (radius + _surface_height(ds, detail, snapshot))
	var normal := (pe - pw).cross(pn - ps)
	if normal.length_squared() < 1e-10:
		normal = direction
	else:
		normal = normal.normalized()
		if normal.dot(direction) < 0.0:
			normal = -normal
	return normal.lerp(direction, water).normalized()


static func _build_local_surface_fields(center: Vector3, east: Vector3, north: Vector3,
		radius: float, snapshot: Dictionary) -> PackedFloat32Array:
	# Seven floats/cell: physical surface elevation, water, soil moisture,
	# snow-free albedo, and a sub-kilometre terrain normal XYZ.
	var packed := PackedFloat32Array()
	packed.resize(LOCAL_W * LOCAL_H * 7)
	var detail := Planet.make_detail()
	var half := LOCAL_CELL_M * float(LOCAL_W) * 0.5
	for y in LOCAL_H:
		var ny := (float(y) + 0.5) * LOCAL_CELL_M - half
		for x in LOCAL_W:
			var ex := (float(x) + 0.5) * LOCAL_CELL_M - half
			var direction := (center + east * (ex / radius) + north * (ny / radius)).normalized()
			var water := clampf(Planet.water_coverage(direction), 0.0, 1.0)
			var moisture := clampf(
				Planet.grid.sample_bilinear(Planet.fields.soil_moisture, direction), 0.0, 1.0)
			var color := Planet.surface_color(direction)
			var luminance := color.r * 0.2126 + color.g * 0.7152 + color.b * 0.0722
			var normal := _local_surface_normal(direction, radius, detail, snapshot, water)
			var offset := (x + y * LOCAL_W) * 7
			packed[offset + 0] = _surface_height(direction, detail, snapshot)
			packed[offset + 1] = water
			packed[offset + 2] = moisture
			packed[offset + 3] = clampf(luminance, 0.04, 0.65)
			packed[offset + 4] = normal.x
			packed[offset + 5] = normal.y
			packed[offset + 6] = normal.z
	return packed


func _finish_local_surface_build(request: int, center: Vector3,
		fields: PackedFloat32Array) -> void:
	if _local_task_id >= 0:
		WorkerThreadPool.wait_for_task_completion(_local_task_id)
		_local_task_id = -1
	_local_building = false
	if request == _local_request and _native != null and is_instance_valid(_native):
		var current_center := WeatherSystem.local_center.normalized()
		var mismatch_m := acos(clampf(current_center.dot(center), -1.0, 1.0)) * Planet.cfg.planet_radius
		if mismatch_m <= LOCAL_CELL_M * 0.5:
			_native.call(&"set_local_surface_fields", fields)
			_local_uploaded_center = center
			_publish_surface_textures()
	if request != _local_request:
		_start_local_surface_build()


func _on_terrain_region_changed(center_dir: Vector3, radius_m: float) -> void:
	if _local_uploaded_center.length_squared() < 0.5 or Planet.cfg == null:
		return
	var distance := acos(clampf(_local_uploaded_center.dot(center_dir.normalized()), -1.0, 1.0)) \
		* Planet.cfg.planet_radius
	if distance <= WeatherSystem.local_span_m * 0.72 + radius_m:
		_queue_local_surface(true)


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
				GLOBAL_W, GLOBAL_H, false, Image.FORMAT_RGBAF, global_values.to_byte_array()))
	var local_result: Variant = _native.call(&"get_local_surface_rgba")
	if local_result is PackedFloat32Array:
		var local_values: PackedFloat32Array = local_result
		if local_values.size() == LOCAL_W * LOCAL_H * 4:
			local_surface_texture.update(Image.create_from_data(
				LOCAL_W, LOCAL_H, false, Image.FORMAT_RGBAF, local_values.to_byte_array()))


func _sync_terrain_materials() -> void:
	var terrain: Object = _terrain_ref.get_ref() if _terrain_ref != null else null
	if terrain == null or not is_instance_valid(terrain):
		terrain = get_node_or_null("/root/GroundGeometryClipmap")
		if terrain != null:
			_terrain_ref = weakref(terrain)
	if terrain == null or not terrain.has_method(&"debug_materials"):
		return
	var ready := 1.0 if _surface_fields_uploaded else 0.0
	for value: Variant in terrain.call(&"debug_materials"):
		if not (value is ShaderMaterial):
			continue
		var material := value as ShaderMaterial
		material.set_shader_parameter("u_surface_energy_ready", ready)
		material.set_shader_parameter("u_surface_energy_global", global_surface_texture)
		material.set_shader_parameter("u_surface_energy_local", local_surface_texture)
		material.set_shader_parameter("u_surface_energy_local_center", WeatherSystem.local_center)
		material.set_shader_parameter("u_surface_energy_local_east", WeatherSystem.local_east)
		material.set_shader_parameter("u_surface_energy_local_north", WeatherSystem.local_north)
		material.set_shader_parameter("u_surface_energy_local_span_m", WeatherSystem.local_span_m)
		if Planet.cfg != null:
			material.set_shader_parameter("u_surface_energy_planet_radius", Planet.cfg.planet_radius)


func global_texture() -> Texture2D:
	return global_surface_texture


func local_texture() -> Texture2D:
	return local_surface_texture


func _exit_tree() -> void:
	if _local_task_id >= 0:
		WorkerThreadPool.wait_for_task_completion(_local_task_id)
