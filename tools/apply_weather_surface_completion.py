from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read(path):
    return (ROOT / path).read_text(encoding="utf-8")


def write(path, text):
    p = ROOT / path
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(text, encoding="utf-8")


def replace_once(text, old, new, label):
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{label}: expected exactly one match, found {count}")
    return text.replace(old, new, 1)


# ---------------------------------------------------------------- weather clock
path = "scripts/weather/weather_system.gd"
s = read(path)
s = replace_once(s, '''const GLOBAL_REAL_INTERVAL := 0.50
const LOCAL_REAL_INTERVAL := 0.20
const TEXTURE_REAL_INTERVAL := 0.50
const GLOBAL_SIM_DT := 90.0
const LOCAL_SIM_DT := 20.0
const SIMULATION_SPEED_MIN := 0.0
const SIMULATION_SPEED_MAX := 128.0
const SIMULATION_SPEED_DEFAULT := 1.0
const MAX_CATCHUP_REAL_DELTA := 0.25
const MAX_GLOBAL_STEPS_PER_FRAME := 4
const MAX_LOCAL_STEPS_PER_FRAME := 10
''', '''const TEXTURE_REAL_INTERVAL := 0.50
const GLOBAL_SIM_DT := 90.0
const LOCAL_SIM_DT := 20.0
const SIMULATION_SPEED_MIN := 0.0
const SIMULATION_SPEED_MAX := 128.0
const SIMULATION_SPEED_DEFAULT := 1.0
# A pathological external celestial time jump is allowed to leave backlog rather
# than silently discarding physical time. Normal 0-128x operation never reaches
# this guard at ordinary frame rates.
const MAX_SOLVER_STEPS_PER_FRAME := 64
''', "weather constants")
s = replace_once(s, '''var _global_accum := 0.0
var _local_accum := 0.0
var _texture_accum := 0.0
''', '''var _global_sim_accum := 0.0
var _local_sim_accum := 0.0
var _texture_accum := 0.0
var _last_celestial_seconds := 0.0
''', "weather accumulators")
s = replace_once(s, '''func _ready() -> void:
	_create_textures()
	_try_create_native_backend()
''', '''func _ready() -> void:
	_last_celestial_seconds = CelestialSystem.simulation_seconds
	CelestialSystem.set_time_scale(simulation_speed)
	_create_textures()
	_try_create_native_backend()
''', "weather ready")
start = s.index("func _process(delta: float) -> void:\n")
end = s.index("\n\nfunc _try_bind_observer() -> void:\n", start)
new_process = '''func _process(delta: float) -> void:
	_try_bind_observer()

	# CelestialSystem is the authoritative physical clock. Weather no longer has
	# independent real-time cadence multipliers: both global and local solvers
	# consume exactly the same elapsed Asterra seconds, using their own fixed
	# stable integration timesteps.
	var celestial_now := CelestialSystem.simulation_seconds
	var simulated_delta := celestial_now - _last_celestial_seconds
	_last_celestial_seconds = celestial_now
	if simulated_delta < 0.0:
		# The native atmosphere is not reversible. A deliberate celestial rewind
		# therefore starts a fresh catch-up interval instead of integrating a
		# physically invalid negative weather timestep.
		_global_sim_accum = 0.0
		_local_sim_accum = 0.0
		simulated_delta = 0.0

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

	var global_steps := 0
	while _global_sim_accum >= GLOBAL_SIM_DT and global_steps < MAX_SOLVER_STEPS_PER_FRAME:
		_global_sim_accum -= GLOBAL_SIM_DT
		_native.call(&"step_global", GLOBAL_SIM_DT)
		global_steps += 1

	if _observer != null and is_instance_valid(_observer):
		_native.call(&"set_local_center", _observer.up_dir())
		var local_steps := 0
		while _local_sim_accum >= LOCAL_SIM_DT and local_steps < MAX_SOLVER_STEPS_PER_FRAME:
			_local_sim_accum -= LOCAL_SIM_DT
			_native.call(&"step_local", LOCAL_SIM_DT)
			local_steps += 1
		local_center = _native.call(&"get_local_center")
		local_east = _native.call(&"get_local_east")
		local_north = _native.call(&"get_local_north")
		local_span_m = float(_native.call(&"get_local_span_m"))

	if _texture_accum >= TEXTURE_REAL_INTERVAL:
		_texture_accum = fmod(_texture_accum, TEXTURE_REAL_INTERVAL)
		_publish_weather_textures()

	_sync_weather_map_material()
'''
s = s[:start] + new_process + s[end:]
s = replace_once(s, '''func set_simulation_speed(value: float) -> void:
	var sanitized := clampf(value, SIMULATION_SPEED_MIN, SIMULATION_SPEED_MAX)
	if is_equal_approx(simulation_speed, sanitized):
		return
	simulation_speed = sanitized
	simulation_speed_changed.emit(simulation_speed)
''', '''func set_simulation_speed(value: float) -> void:
	var sanitized := clampf(value, SIMULATION_SPEED_MIN, SIMULATION_SPEED_MAX)
	if is_equal_approx(simulation_speed, sanitized) and is_equal_approx(CelestialSystem.time_scale, sanitized):
		return
	simulation_speed = sanitized
	CelestialSystem.set_time_scale(simulation_speed)
	simulation_speed_changed.emit(simulation_speed)
''', "weather speed setter")
write(path, s)


# ----------------------------------------------------------- surface GDScript
surface_bridge = r'''extends Node
## Couples procedural terrain and live Helion forcing to WeatherNative.
##
## Global geography is uploaded once. The moving 2.2 km weather nest gets a
## separate high-resolution surface build from the same procedural terrain used
## by rendering/collision, including sparse player terrain edits. Native code owns
## all evolving temperature, moisture, snow and horizon state.

const GLOBAL_W := 256
const GLOBAL_H := 128
const LOCAL_W := 192
const LOCAL_H := 192
const LOCAL_CELL_M := 2200.0
const LOCAL_NORMAL_SAMPLE_M := 600.0
const TEXTURE_INTERVAL := 0.50

var global_surface_texture: ImageTexture
var local_surface_texture: ImageTexture

var _native: Object = null
var _surface_fields_uploaded := false
var _texture_accum := 0.0
var _warned_stale_backend := false
var _local_request := 0
var _local_building := false
var _local_task_id := -1
var _local_uploaded_center := Vector3.ZERO
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
	_local_uploaded_center = Vector3.ZERO
	_local_request += 1
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
			var direction := Vector3(cos_lat * cos(lon), sin_lat, cos_lat * sin(lon))
			var offset := (x + y * GLOBAL_W) * 4
			var water := clampf(Planet.water_coverage(direction), 0.0, 1.0)
			var moisture := clampf(
				Planet.grid.sample_bilinear(Planet.fields.soil_moisture, direction), 0.0, 1.0)
			var color := Planet.surface_color(direction)
			var luminance := color.r * 0.2126 + color.g * 0.7152 + color.b * 0.0722
			packed[offset + 0] = lerpf(Planet.macro_height(direction), Planet.water_height(direction), water)
			packed[offset + 1] = water
			packed[offset + 2] = moisture
			packed[offset + 3] = clampf(luminance, 0.04, 0.65)

	_native.call(&"set_global_surface_fields", packed)
	_surface_fields_uploaded = true
	_local_uploaded_center = Vector3.ZERO
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
	if _local_uploaded_center.length_squared() < 0.5:
		_queue_local_surface()
		return
	var moved_m := acos(clampf(_local_uploaded_center.dot(center), -1.0, 1.0)) * Planet.cfg.planet_radius
	if moved_m > LOCAL_CELL_M * 0.25:
		_queue_local_surface()


func _queue_local_surface() -> void:
	_local_request += 1
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
		_queue_local_surface()


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
'''
write("scripts/weather/surface_energy_bridge.gd", surface_bridge)


# --------------------------------------------------------------- native header
path = "native/weather/src/weather_native.h"
h = read(path)
h = replace_once(h, '''\tstatic constexpr int LAYERS = 6;\n\tstatic constexpr int INTERFACES = LAYERS - 1;\n''', '''\tstatic constexpr int LAYERS = 6;\n\tstatic constexpr int INTERFACES = LAYERS - 1;\n\tstatic constexpr int HORIZON_SECTORS = 24;\n''', "header horizon constant")
h = replace_once(h, '''\t\tstd::vector<float> ground_flux_w_m2;\n\n\t\tvoid resize''', '''\t\tstd::vector<float> ground_flux_w_m2;\n\t\t// Local terrain horizon, stored as tangent(elevation angle) per azimuth.\n\t\tstd::vector<float> horizon_tan;\n\t\tstd::vector<float> sky_view_factor;\n\t\tstd::vector<float> terrain_sun_visibility;\n\n\t\tvoid resize''', "header surface diagnostics")
h = replace_once(h, '''\tVector3 solar_direction_body = Vector3(-1, 0, 0);\n\tfloat solar_irradiance_w_m2 = 1420.0f;\n\tbool local_initialized = false;\n\tbool surface_fields_ready = false;\n''', '''\tVector3 solar_direction_body = Vector3(-1, 0, 0);\n\tfloat solar_irradiance_w_m2 = 1420.0f;\n\tfloat helion_angular_radius_rad = 0.00465f;\n\tbool local_initialized = false;\n\tbool surface_fields_ready = false;\n\tbool local_surface_fields_ready = false;\n''', "header solar state")
h = replace_once(h, '''\tvoid initialize_global_surface_state();\n\tvoid initialize_local_surface();\n''', '''\tvoid initialize_global_surface_state();\n\tvoid initialize_local_surface();\n\tvoid update_local_surface_geometry();\n\tvoid update_local_surface_horizon();\n''', "header local methods")
h = replace_once(h, '''\tvoid set_global_surface_fields(const PackedFloat32Array &fields);\n\tvoid set_solar_forcing(const Vector3 &sun_direction_body, float irradiance_w_m2);\n''', '''\tvoid set_global_surface_fields(const PackedFloat32Array &fields);\n\tvoid set_local_surface_fields(const PackedFloat32Array &fields);\n\tvoid set_solar_forcing(const Vector3 &sun_direction_body, float irradiance_w_m2,\n\t\tfloat angular_radius_rad);\n''', "header public setters")
write(path, h)


# ---------------------------------------------------------------- native cpp
path = "native/weather/src/weather_native.cpp"
c = read(path)
c = replace_once(c, '''static constexpr float MIN_EXCHANGE_WIND_MPS = 0.5f;\n''', '''static constexpr float MIN_EXCHANGE_WIND_MPS = 0.5f;\nstatic constexpr float FREE_CONVECTION_WIND_COEFF = 0.65f;\n''', "cpp free convection constant")
c = replace_once(c, '''static constexpr float SNOW_ALBEDO_AGE_TAU_S = 5.0f * 11.5f * 3600.0f;\n''', '''static constexpr float SNOW_ALBEDO_AGE_TAU_S = 5.0f * 11.5f * 3600.0f;\nstatic constexpr std::array<int, 10> HORIZON_MARCH_CELLS = {\n\t1, 2, 3, 5, 8, 12, 18, 27, 40, 55\n};\n''', "cpp horizon march")
c = replace_once(c, '''\tfor (auto *a : {&elevation_m, &water_fraction, &soil_moisture, &base_albedo,\n\t\t&dir_x, &dir_y, &dir_z, &normal_x, &normal_y, &normal_z,\n\t\t&temperature_k, &subsurface_temperature_k, &snow_swe_kg_m2,\n\t\t&snow_age_s, &snow_wetness, &albedo, &absorbed_solar_w_m2,\n\t\t&sensible_flux_w_m2, &latent_flux_w_m2, &ground_flux_w_m2}) {\n\t\ta->assign(cells, 0.0f);\n\t}\n}\n''', '''\tfor (auto *a : {&elevation_m, &water_fraction, &soil_moisture, &base_albedo,\n\t\t&dir_x, &dir_y, &dir_z, &normal_x, &normal_y, &normal_z,\n\t\t&temperature_k, &subsurface_temperature_k, &snow_swe_kg_m2,\n\t\t&snow_age_s, &snow_wetness, &albedo, &absorbed_solar_w_m2,\n\t\t&sensible_flux_w_m2, &latent_flux_w_m2, &ground_flux_w_m2}) {\n\t\ta->assign(cells, 0.0f);\n\t}\n\thorizon_tan.assign(size_t(cells) * HORIZON_SECTORS, 0.0f);\n\tsky_view_factor.assign(cells, 1.0f);\n\tterrain_sun_visibility.assign(cells, 1.0f);\n}\n''', "cpp surface resize")
c = replace_once(c, '''\tClassDB::bind_method(D_METHOD("set_global_surface_fields", "fields"), &WeatherNative::set_global_surface_fields);\n\tClassDB::bind_method(D_METHOD("set_solar_forcing", "sun_direction_body", "irradiance_w_m2"), &WeatherNative::set_solar_forcing);\n''', '''\tClassDB::bind_method(D_METHOD("set_global_surface_fields", "fields"), &WeatherNative::set_global_surface_fields);\n\tClassDB::bind_method(D_METHOD("set_local_surface_fields", "fields"), &WeatherNative::set_local_surface_fields);\n\tClassDB::bind_method(D_METHOD("set_solar_forcing", "sun_direction_body", "irradiance_w_m2", "angular_radius_rad"), &WeatherNative::set_solar_forcing);\n''', "cpp bindings")
c = replace_once(c, '''\tsurface_fields_ready = true;\n\t// Rebuild the nest so its lower boundary is sampled from the new geography\n''', '''\tsurface_fields_ready = true;\n\tlocal_surface_fields_ready = false;\n\t// Rebuild the nest so its lower boundary is sampled from the new geography\n''', "cpp global surface ready")
c = replace_once(c, '''void WeatherNative::set_solar_forcing(const Vector3 &sun_direction_body, float irradiance_w_m2) {\n\tif (sun_direction_body.length_squared() > 1e-10f) {\n\t\tsolar_direction_body = sun_direction_body.normalized();\n\t}\n\tsolar_irradiance_w_m2 = std::clamp(irradiance_w_m2, 0.0f, 5000.0f);\n}\n''', '''void WeatherNative::set_solar_forcing(const Vector3 &sun_direction_body, float irradiance_w_m2,\n\t\tfloat angular_radius_rad) {\n\tif (sun_direction_body.length_squared() > 1e-10f) {\n\t\tsolar_direction_body = sun_direction_body.normalized();\n\t}\n\tsolar_irradiance_w_m2 = std::clamp(irradiance_w_m2, 0.0f, 5000.0f);\n\thelion_angular_radius_rad = std::clamp(angular_radius_rad, 0.0001f, 0.03f);\n}\n''', "cpp solar setter")
c = c.replace("const float rho = 1.225f * std::pow(sigma, 0.82f);", "const float rho = 1.33f * std::pow(sigma, 0.82f);")
c = c.replace("pressure), 12000.0f, 105000.0f);", "pressure), 12000.0f, 115000.0f);")
c = replace_once(c, '''\t\tfloat wind = std::sqrt(a.nu[l0 + c] * a.nu[l0 + c] + a.nv[l0 + c] * a.nv[l0 + c]);\n\t\tfloat exchange_wind = std::max(wind, MIN_EXCHANGE_WIND_MPS);\n''', '''\t\tfloat wind = std::sqrt(a.nu[l0 + c] * a.nu[l0 + c] + a.nv[l0 + c] * a.nv[l0 + c]);\n\t\t// Free convection keeps strongly sun-heated ground coupled to the boundary\n\t\t// layer even in light winds; mechanical and buoyant exchange add in\n\t\t// quadrature rather than one arbitrarily replacing the other.\n\t\tfloat buoyant_wind = FREE_CONVECTION_WIND_COEFF\n\t\t\t* std::sqrt(std::max(ts - air_t, 0.0f));\n\t\tfloat exchange_wind = std::max(\n\t\t\tstd::sqrt(wind * wind + buoyant_wind * buoyant_wind), MIN_EXCHANGE_WIND_MPS);\n''', "cpp exchange wind")
c = replace_once(c, '''\t\tfloat radial_mu = std::max(d.dot(solar_direction_body), 0.0f);\n\t\tfloat slope_mu = std::max(n.dot(solar_direction_body), 0.0f);\n\t\t// Planetary horizon gates direct light; the local terrain normal then\n\t\t// gives slope/aspect heating. A later horizon map can multiply this term.\n\t\tfloat direct_mu = radial_mu > 0.0f ? slope_mu : 0.0f;\n\t\tfloat clear_transmission = 0.72f;\n\t\tfloat cloud_direct = std::exp(-2.0f * cloud);\n\t\tfloat direct_sw = solar_irradiance_w_m2 * direct_mu * clear_transmission * cloud_direct;\n\t\tfloat diffuse_sw = solar_irradiance_w_m2 * radial_mu\n\t\t\t* (0.10f + 0.13f * cloud) * (1.0f - 0.35f * cloud);\n\t\tfloat absorbed_sw = std::max((direct_sw + diffuse_sw) * (1.0f - albedo), 0.0f);\n''', '''\t\tfloat sun_dot = d.dot(solar_direction_body);\n\t\tfloat radial_mu = std::max(sun_dot, 0.0f);\n\t\tfloat slope_mu = std::max(n.dot(solar_direction_body), 0.0f);\n\t\tfloat terrain_visibility = 1.0f;\n\t\tfloat sky_view = is_global ? 1.0f : std::clamp(surface.sky_view_factor[c], 0.05f, 1.0f);\n\t\tif (!is_global && local_surface_fields_ready && radial_mu > 0.0f) {\n\t\t\tVector3 tangent_sun = solar_direction_body - d * sun_dot;\n\t\t\tif (tangent_sun.length_squared() > 1e-10f) {\n\t\t\t\ttangent_sun.normalize();\n\t\t\t\tfloat azimuth = std::atan2(tangent_sun.dot(local_north), tangent_sun.dot(local_east));\n\t\t\t\tif (azimuth < 0.0f) azimuth += TAU_F;\n\t\t\t\tfloat sector_f = azimuth / TAU_F * float(HORIZON_SECTORS) - 0.5f;\n\t\t\t\tint sector0 = int(std::floor(sector_f));\n\t\t\t\tfloat blend = sector_f - std::floor(sector_f);\n\t\t\t\tint s0 = wrap_x(sector0, HORIZON_SECTORS);\n\t\t\t\tint s1 = wrap_x(sector0 + 1, HORIZON_SECTORS);\n\t\t\t\tfloat horizon_tan = std::lerp(\n\t\t\t\t\tsurface.horizon_tan[size_t(c) * HORIZON_SECTORS + s0],\n\t\t\t\t\tsurface.horizon_tan[size_t(c) * HORIZON_SECTORS + s1], blend);\n\t\t\t\tfloat sun_elevation = std::asin(std::clamp(radial_mu, 0.0f, 1.0f));\n\t\t\t\tfloat horizon_angle = std::atan(std::max(horizon_tan, 0.0f));\n\t\t\t\tfloat radius = std::max(helion_angular_radius_rad, 0.0001f);\n\t\t\t\tterrain_visibility = smoothstep01(\n\t\t\t\t\t(sun_elevation - (horizon_angle - radius)) / (2.0f * radius));\n\t\t\t}\n\t\t}\n\t\tsurface.terrain_sun_visibility[c] = terrain_visibility;\n\t\tfloat direct_mu = radial_mu > 0.0f ? slope_mu * terrain_visibility : 0.0f;\n\t\tfloat clear_transmission = 0.72f;\n\t\tfloat cloud_direct = std::exp(-2.0f * cloud);\n\t\tfloat direct_sw = solar_irradiance_w_m2 * direct_mu * clear_transmission * cloud_direct;\n\t\tfloat diffuse_sw = solar_irradiance_w_m2 * radial_mu\n\t\t\t* (0.10f + 0.13f * cloud) * (1.0f - 0.35f * cloud) * sky_view;\n\t\tfloat absorbed_sw = std::max((direct_sw + diffuse_sw) * (1.0f - albedo), 0.0f);\n''', "cpp terrain horizon solar")
c = replace_once(c, '''\t\t\t__m256 convective_up = _mm256_mul_ps(_mm256_mul_ps(instability, moisture), _mm256_set1_ps(2.2e-4f * convection_weight));\n\t\t\t__m256 downdraft = _mm256_mul_ps(precip, _mm256_set1_ps(5.5e-5f));\n\t\t\t__m256 rate = clamp8(_mm256_sub_ps(_mm256_add_ps(convergence_up, convective_up), downdraft), -1.8e-4f, 6.0e-4f);\n''', '''\t\t\t__m256 convective_up = _mm256_mul_ps(_mm256_mul_ps(instability, moisture), _mm256_set1_ps(2.2e-4f * convection_weight));\n\t\t\t// Positive sensible heat at the lower boundary is an explicit buoyancy\n\t\t\t// source for interface 0. This turns differential slope heating into\n\t\t\t// resolved upslope/thermal circulation instead of relying only on delayed\n\t\t\t// static-instability feedback.\n\t\t\t__m256 surface_buoyancy = zero;\n\t\t\tif (interface_index == 0 && surface_fields_ready) {\n\t\t\t\tconst SurfaceState &surface = is_global ? global_surface : local_surface;\n\t\t\t\t__m256 sensible = _mm256_loadu_ps(&surface.sensible_flux_w_m2[c]);\n\t\t\t\tsurface_buoyancy = _mm256_mul_ps(\n\t\t\t\t\t_mm256_max_ps(sensible, zero),\n\t\t\t\t\t_mm256_set1_ps((1.45e-4f / 350.0f) * convection_weight));\n\t\t\t}\n\t\t\t__m256 downdraft = _mm256_mul_ps(precip, _mm256_set1_ps(5.5e-5f));\n\t\t\t__m256 rate = clamp8(_mm256_sub_ps(\n\t\t\t\t_mm256_add_ps(_mm256_add_ps(convergence_up, convective_up), surface_buoyancy),\n\t\t\t\tdowndraft), -1.8e-4f, 6.0e-4f);\n''', "cpp surface buoyancy")

# Replace local-surface initialization as a complete unit so its horizon state is explicit.
local_start = c.index("void WeatherNative::initialize_local_surface() {\n")
local_end = c.index("\nvoid WeatherNative::nudge_local_boundaries(float dt) {", local_start)
new_local = r'''void WeatherNative::initialize_local_surface() {
	float half = LOCAL_CELL_M * float(LOCAL_W) * 0.5f;
	for (int y = 0; y < LOCAL_H; ++y) {
		for (int x = 0; x < LOCAL_W; ++x) {
			float ex = (float(x) + 0.5f) * LOCAL_CELL_M - half;
			float ny = (float(y) + 0.5f) * LOCAL_CELL_M - half;
			Vector3 d = (local_center + local_east * (ex / PLANET_RADIUS_M)
				+ local_north * (ny / PLANET_RADIUS_M)).normalized();
			int c = x + y * LOCAL_W;
			local_surface.dir_x[c] = d.x;
			local_surface.dir_y[c] = d.y;
			local_surface.dir_z[c] = d.z;
			local_surface.elevation_m[c] = sample_global_surface_scalar(global_surface.elevation_m, d);
			local_surface.water_fraction[c] = std::clamp(
				sample_global_surface_scalar(global_surface.water_fraction, d), 0.0f, 1.0f);
			local_surface.soil_moisture[c] = std::clamp(
				sample_global_surface_scalar(global_surface.soil_moisture, d), 0.0f, 1.0f);
			local_surface.base_albedo[c] = std::clamp(
				sample_global_surface_scalar(global_surface.base_albedo, d), 0.04f, 0.65f);
			Vector3 n(
				sample_global_surface_scalar(global_surface.normal_x, d),
				sample_global_surface_scalar(global_surface.normal_y, d),
				sample_global_surface_scalar(global_surface.normal_z, d));
			if (n.length_squared() < 1e-8f) n = d;
			else n.normalize();
			local_surface.normal_x[c] = n.x;
			local_surface.normal_y[c] = n.y;
			local_surface.normal_z[c] = n.z;
			local_surface.temperature_k[c] = sample_global_surface_scalar(global_surface.temperature_k, d);
			local_surface.subsurface_temperature_k[c] = sample_global_surface_scalar(global_surface.subsurface_temperature_k, d);
			local_surface.snow_swe_kg_m2[c] = std::max(
				sample_global_surface_scalar(global_surface.snow_swe_kg_m2, d), 0.0f);
			local_surface.snow_age_s[c] = std::max(
				sample_global_surface_scalar(global_surface.snow_age_s, d), 0.0f);
			local_surface.snow_wetness[c] = std::clamp(
				sample_global_surface_scalar(global_surface.snow_wetness, d), 0.0f, 1.0f);
			local_surface.albedo[c] = std::clamp(
				sample_global_surface_scalar(global_surface.albedo, d), 0.03f, 0.94f);
			local_surface.absorbed_solar_w_m2[c] = 0.0f;
			local_surface.sensible_flux_w_m2[c] = 0.0f;
			local_surface.latent_flux_w_m2[c] = 0.0f;
			local_surface.ground_flux_w_m2[c] = 0.0f;
			local_surface.sky_view_factor[c] = 1.0f;
			local_surface.terrain_sun_visibility[c] = 1.0f;
			for (int sector = 0; sector < HORIZON_SECTORS; ++sector) {
				local_surface.horizon_tan[size_t(c) * HORIZON_SECTORS + sector] = 0.0f;
			}
		}
	}
	local_surface_fields_ready = false;
}

void WeatherNative::update_local_surface_geometry() {
	float half = LOCAL_CELL_M * float(LOCAL_W) * 0.5f;
	for (int y = 0; y < LOCAL_H; ++y) {
		for (int x = 0; x < LOCAL_W; ++x) {
			int c = x + y * LOCAL_W;
			float ex = (float(x) + 0.5f) * LOCAL_CELL_M - half;
			float ny = (float(y) + 0.5f) * LOCAL_CELL_M - half;
			Vector3 d = (local_center + local_east * (ex / PLANET_RADIUS_M)
				+ local_north * (ny / PLANET_RADIUS_M)).normalized();
			local_surface.dir_x[c] = d.x;
			local_surface.dir_y[c] = d.y;
			local_surface.dir_z[c] = d.z;

			Vector3 supplied(local_surface.normal_x[c], local_surface.normal_y[c], local_surface.normal_z[c]);
			if (supplied.length_squared() < 0.25f) {
				int xe = std::min(x + 1, LOCAL_W - 1);
				int xw = std::max(x - 1, 0);
				int yn = std::min(y + 1, LOCAL_H - 1);
				int ys = std::max(y - 1, 0);
				auto position_at = [&](int sx, int sy) {
					int sc = sx + sy * LOCAL_W;
					float sex = (float(sx) + 0.5f) * LOCAL_CELL_M - half;
					float sny = (float(sy) + 0.5f) * LOCAL_CELL_M - half;
					Vector3 sd = (local_center + local_east * (sex / PLANET_RADIUS_M)
						+ local_north * (sny / PLANET_RADIUS_M)).normalized();
					float water = std::clamp(local_surface.water_fraction[sc], 0.0f, 1.0f);
					float h = std::lerp(local_surface.elevation_m[sc], 0.0f, water);
					return sd * (PLANET_RADIUS_M + h);
				};
				Vector3 tangent_x = position_at(xe, y) - position_at(xw, y);
				Vector3 tangent_y = position_at(x, yn) - position_at(x, ys);
				supplied = tangent_x.cross(tangent_y);
				if (supplied.length_squared() < 1e-10f) supplied = d;
				else supplied.normalize();
			}
			if (supplied.dot(d) < 0.0f) supplied = -supplied;
			float water = std::clamp(local_surface.water_fraction[c], 0.0f, 1.0f);
			Vector3 n = supplied.lerp(d, water).normalized();
			local_surface.normal_x[c] = n.x;
			local_surface.normal_y[c] = n.y;
			local_surface.normal_z[c] = n.z;
		}
	}
}

void WeatherNative::update_local_surface_horizon() {
	for (int y = 0; y < LOCAL_H; ++y) {
		for (int x = 0; x < LOCAL_W; ++x) {
			int c = x + y * LOCAL_W;
			Vector3 d0(local_surface.dir_x[c], local_surface.dir_y[c], local_surface.dir_z[c]);
			d0.normalize();
			float water0 = std::clamp(local_surface.water_fraction[c], 0.0f, 1.0f);
			float h0 = std::lerp(local_surface.elevation_m[c], 0.0f, water0);
			Vector3 p0 = d0 * (PLANET_RADIUS_M + h0);
			float sky_sum = 0.0f;
			for (int sector = 0; sector < HORIZON_SECTORS; ++sector) {
				float angle = (float(sector) + 0.5f) * TAU_F / float(HORIZON_SECTORS);
				float sx_dir = std::cos(angle);
				float sy_dir = std::sin(angle);
				float max_tan = 0.0f;
				for (int step : HORIZON_MARCH_CELLS) {
					int sx = int(std::round(float(x) + sx_dir * float(step)));
					int sy = int(std::round(float(y) + sy_dir * float(step)));
					if (sx < 0 || sx >= LOCAL_W || sy < 0 || sy >= LOCAL_H) break;
					int sc = sx + sy * LOCAL_W;
					Vector3 d1(local_surface.dir_x[sc], local_surface.dir_y[sc], local_surface.dir_z[sc]);
					d1.normalize();
					float water1 = std::clamp(local_surface.water_fraction[sc], 0.0f, 1.0f);
					float h1 = std::lerp(local_surface.elevation_m[sc], 0.0f, water1);
					Vector3 rel = d1 * (PLANET_RADIUS_M + h1) - p0;
					float vertical = rel.dot(d0);
					float horizontal_sq = std::max(rel.length_squared() - vertical * vertical, 1.0f);
					float horizon_tan = vertical / std::sqrt(horizontal_sq);
					max_tan = std::max(max_tan, horizon_tan);
				}
				max_tan = std::max(max_tan, 0.0f);
				local_surface.horizon_tan[size_t(c) * HORIZON_SECTORS + sector] = max_tan;
				// Isotropic sky-view approximation: cos^2(horizon elevation), averaged
				// over azimuth. It attenuates diffuse short-wave and leaves open terrain 1.
				sky_sum += 1.0f / (1.0f + max_tan * max_tan);
			}
			local_surface.sky_view_factor[c] = std::clamp(
				sky_sum / float(HORIZON_SECTORS), 0.05f, 1.0f);
		}
	}
}

void WeatherNative::set_local_surface_fields(const PackedFloat32Array &fields) {
	constexpr int STRIDE = 7;
	if (fields.size() != LOCAL_W * LOCAL_H * STRIDE || !local_initialized) return;
	for (int c = 0; c < local_surface.cells; ++c) {
		local_surface.elevation_m[c] = fields[c * STRIDE + 0];
		local_surface.water_fraction[c] = std::clamp(fields[c * STRIDE + 1], 0.0f, 1.0f);
		local_surface.soil_moisture[c] = std::clamp(fields[c * STRIDE + 2], 0.0f, 1.0f);
		local_surface.base_albedo[c] = std::clamp(fields[c * STRIDE + 3], 0.04f, 0.65f);
		local_surface.normal_x[c] = fields[c * STRIDE + 4];
		local_surface.normal_y[c] = fields[c * STRIDE + 5];
		local_surface.normal_z[c] = fields[c * STRIDE + 6];
	}
	update_local_surface_geometry();
	update_local_surface_horizon();
	local_surface_fields_ready = true;
}
'''
c = c[:local_start] + new_local + c[local_end:]
write(path, c)


# ------------------------------------------------------------ shader include
weather_surface = r'''// Dynamic land-surface state published by WeatherNative.
// RGBA = normalized surface temperature, snow cover, dynamic albedo, wetness/water.
uniform sampler2D u_surface_energy_global : filter_linear, repeat_enable;
uniform sampler2D u_surface_energy_local : filter_linear, repeat_disable;
uniform float u_surface_energy_ready = 0.0;
uniform vec3 u_surface_energy_local_center = vec3(0.0, 1.0, 0.0);
uniform vec3 u_surface_energy_local_east = vec3(1.0, 0.0, 0.0);
uniform vec3 u_surface_energy_local_north = vec3(0.0, 0.0, 1.0);
uniform float u_surface_energy_local_span_m = 0.0;
uniform float u_surface_energy_planet_radius = 3500000.0;

vec2 surface_energy_global_uv(vec3 dir) {
	float lon = atan(dir.z, dir.x);
	if (lon < 0.0) lon += 6.28318530718;
	float lat = asin(clamp(dir.y, -1.0, 1.0));
	return vec2(lon / 6.28318530718, 0.5 - lat / 3.14159265359);
}

vec3 surface_energy_local_lookup(vec3 dir) {
	if (u_surface_energy_local_span_m <= 1.0) return vec3(0.0, 0.0, -1.0);
	vec3 center = normalize(u_surface_energy_local_center);
	float forward = dot(dir, center);
	if (forward <= 0.0) return vec3(0.0, 0.0, -1.0);
	float east_angle = atan(dot(dir, normalize(u_surface_energy_local_east)), forward);
	float north_angle = atan(dot(dir, normalize(u_surface_energy_local_north)), forward);
	vec2 local_uv = vec2(east_angle, north_angle)
		* (u_surface_energy_planet_radius / u_surface_energy_local_span_m) + vec2(0.5);
	vec2 edge_v = abs(local_uv - vec2(0.5)) * 2.0;
	float edge = max(edge_v.x, edge_v.y);
	if (edge >= 1.0) return vec3(local_uv, -1.0);
	return vec3(local_uv, 1.0 - smoothstep(0.86, 1.0, edge));
}

vec4 surface_energy_sample(vec3 planet_position) {
	if (u_surface_energy_ready < 0.5 || dot(planet_position, planet_position) < 1.0) {
		return vec4((288.0 - 220.0) / 110.0, 0.0, 0.18, 0.35);
	}
	vec3 dir = normalize(planet_position);
	vec4 coarse = texture(u_surface_energy_global, surface_energy_global_uv(dir));
	vec3 lookup = surface_energy_local_lookup(dir);
	if (lookup.z < 0.0) return coarse;
	return mix(coarse, texture(u_surface_energy_local, lookup.xy), lookup.z);
}
'''
write("shaders/weather_surface.gdshaderinc", weather_surface)


# --------------------------------------------------------- active terrain shader
path = "shaders/spherical_geometry_clipmap_procedural_uv.gdshader"
sh = read(path)
sh = replace_once(sh, '''#include "res://shaders/material_clipmap.gdshaderinc"\n#include "res://shaders/planet_surface_lookup.gdshaderinc"\n''', '''#include "res://shaders/material_clipmap.gdshaderinc"\n#include "res://shaders/weather_surface.gdshaderinc"\n#include "res://shaders/planet_surface_lookup.gdshaderinc"\n''', "terrain surface include")
sh = replace_once(sh, '''\t\talbedo = mix(albedo, sand, clamp(surf.b, 0.0, 1.0));\n\t\talbedo = mix(albedo, snow, clamp(surf.a, 0.0, 1.0));\n\t\talbedo = mix(albedo, rock, smoothstep(0.08, 0.34, slope));\n\n\t\tALBEDO = albedo;\n\t\tROUGHNESS = mix(0.94, 0.72, smoothstep(0.10, 0.42, slope));\n\t\tSPECULAR = 0.22;\n''', '''\t\talbedo = mix(albedo, sand, clamp(surf.b, 0.0, 1.0));\n\t\t// Generated frost remains a weak permanent/glacial tint. Current snow is\n\t\t// supplied by the physical surface reservoir below, so winter cover can\n\t\t// actually accumulate, age, wet and disappear.\n\t\talbedo = mix(albedo, snow, clamp(surf.a, 0.0, 1.0) * 0.20);\n\t\talbedo = mix(albedo, rock, smoothstep(0.08, 0.34, slope));\n\n\t\tvec4 dynamic_surface = surface_energy_sample(v_planet);\n\t\tfloat dynamic_snow = clamp(dynamic_surface.g, 0.0, 1.0)\n\t\t\t* (1.0 - smoothstep(0.16, 0.46, slope));\n\t\tfloat snow_optical = clamp((dynamic_surface.b - 0.56) / 0.34, 0.0, 1.0);\n\t\tvec3 current_snow = mix(vec3(0.61, 0.64, 0.66), vec3(0.94, 0.96, 0.98), snow_optical);\n\t\tfloat dynamic_wet = clamp(dynamic_surface.a, 0.0, 1.0) * (1.0 - dynamic_snow);\n\t\talbedo *= mix(1.0, 0.86, dynamic_wet * 0.55);\n\t\talbedo = mix(albedo, current_snow, dynamic_snow);\n\n\t\tALBEDO = albedo;\n\t\tfloat base_roughness = mix(0.94, 0.72, smoothstep(0.10, 0.42, slope));\n\t\tfloat surface_temperature_k = 220.0 + dynamic_surface.r * 110.0;\n\t\tfloat melting = smoothstep(270.5, 274.0, surface_temperature_k);\n\t\tROUGHNESS = mix(base_roughness, mix(0.92, 0.62, melting), dynamic_snow);\n\t\tSPECULAR = mix(0.22, mix(0.20, 0.38, melting), dynamic_snow);\n''', "terrain dynamic snow")
write(path, sh)

print("weather/surface completion patch applied")
