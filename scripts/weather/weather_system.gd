extends Node
## Lightweight procedural weather: fakes regional cloud cover, storms and
## pressure systems with drifting 3D noise instead of the native AVX2
## atmospheric solver. No DLL, no GPU refinement, no worker-thread solver
## backlog -- just a periodic background noise pass that republishes the same
## global/local RGBA field contract consumers already expect.
##
## Noise is sampled directly on the unit sphere (get_noise_3d on the surface
## direction vector), so the field is seamless at the date line and has no
## pole singularity. "Wind" is faked by rotating the sample point around the
## polar axis over simulated time -- cheap, and warp-aware for free since it
## reads CelestialSystem.simulation_seconds.

const GLOBAL_W := 1024
const GLOBAL_H := 512
const LOCAL_W := 192
const LOCAL_H := 192
const HIGH_WARP_LOCAL_THRESHOLD := 256.0
const SIMULATION_SPEED_MIN := 0.0
const SIMULATION_SPEED_MAX := 8192.0
const SIMULATION_SPEED_DEFAULT := 1.0
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
const FALLBACK_PLANET_RADIUS_M := 6371000.0

## How often the CPU (re)builds each field on a background task. Weather
## drifts slowly, so there is no need to touch every pixel every frame.
const GLOBAL_REGEN_INTERVAL_S := 6.0
const LOCAL_REGEN_INTERVAL_S := 1.5

## Noise shaping. Frequencies are in cycles-per-unit-sphere-radius, so ~1.0
## spans roughly the whole globe and larger values add regional detail.
const COVERAGE_FREQUENCY := 1.05
const COVERAGE_OCTAVES := 4
const DETAIL_FREQUENCY := 3.1
const DETAIL_OCTAVES := 3
const STORM_FREQUENCY := 4.4
const PRESSURE_FREQUENCY := 0.6
const PRESSURE_OCTAVES := 3

## Radians/simulated-second the coverage field drifts around the polar axis.
## ~TAU over 6 simulated hours at simulation_speed 1.
const WIND_RATE := TAU / 21600.0

signal simulation_weight_changed(weight: float)
signal simulation_speed_changed(speed: float)
signal tuning_weight_changed(name: StringName, weight: float)
signal physics_tuning_reset
signal global_state_advanced(revision: int)
signal local_state_advanced(revision: int)
## Kept for API compatibility with PersistentHydrologySystem/HUD code that
## still branches on a native backend. Always false/failed now: there is no
## native backend to become ready, so consumers stay on their own
## climatology/fallback paths.
signal native_ready
signal native_failed(reason: String)

var global_weather_texture: ImageTexture
var local_weather_texture: ImageTexture
var global_diagnostics_texture: ImageTexture
var local_diagnostics_texture: ImageTexture
var global_products_texture: ImageTexture
var local_products_texture: ImageTexture
var global_weather_values := PackedFloat32Array()
var global_diagnostics_values := PackedFloat32Array()
var global_products_values := PackedFloat32Array()
var global_convective_values := PackedFloat32Array()
var global_state_revision: int = 0
var local_state_revision: int = 0
var local_center := Vector3.UP
var local_east := Vector3.RIGHT
var local_north := Vector3.FORWARD
var local_span_m := 422400.0

var native_available := false
var backend_error := "native weather simulation is disabled; using the procedural fallback"
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

var _observer: AsterraPlayer
var _coverage_noise: FastNoiseLite
var _detail_noise: FastNoiseLite
var _storm_noise: FastNoiseLite
var _pressure_noise: FastNoiseLite

var _global_regen_accum := 999.0
var _local_regen_accum := 999.0
var _global_regen_task_id: int = -1
var _local_regen_task_id: int = -1
var _pending_global_result: PackedFloat32Array
var _pending_local_result: PackedFloat32Array
var _heavy_publication_frame: int = -2


func _ready() -> void:
	CelestialSystem.set_time_scale(simulation_speed)
	_create_textures()
	_init_noise()
	_publish_flat_fallback()
	call_deferred("_announce_native_state")


func _announce_native_state() -> void:
	native_failed.emit(backend_error)


func _init_noise() -> void:
	var world := load("res://world.tres")
	var seed := 1
	if world is GenConfig:
		seed = int(world.world_seed)
	_coverage_noise = FastNoiseLite.new()
	_coverage_noise.seed = seed
	_coverage_noise.frequency = COVERAGE_FREQUENCY
	_coverage_noise.fractal_octaves = COVERAGE_OCTAVES
	_detail_noise = FastNoiseLite.new()
	_detail_noise.seed = seed + 101
	_detail_noise.frequency = DETAIL_FREQUENCY
	_detail_noise.fractal_octaves = DETAIL_OCTAVES
	_storm_noise = FastNoiseLite.new()
	_storm_noise.seed = seed + 202
	_storm_noise.frequency = STORM_FREQUENCY
	_storm_noise.fractal_octaves = 2
	_pressure_noise = FastNoiseLite.new()
	_pressure_noise.seed = seed + 303
	_pressure_noise.frequency = PRESSURE_FREQUENCY
	_pressure_noise.fractal_octaves = PRESSURE_OCTAVES


func _process(delta: float) -> void:
	_try_bind_observer()
	if _observer != null and is_instance_valid(_observer):
		_update_fallback_basis(_observer.up_dir())

	_poll_global_regen()
	_poll_local_regen()

	_global_regen_accum += delta
	_local_regen_accum += delta
	var sim_seconds := CelestialSystem.simulation_seconds
	if _global_regen_task_id < 0 and _global_regen_accum >= GLOBAL_REGEN_INTERVAL_S:
		_start_global_regen(sim_seconds)
	if _local_regen_task_id < 0 and _local_regen_accum >= LOCAL_REGEN_INTERVAL_S \
			and _observer != null and is_instance_valid(_observer):
		_start_local_regen(sim_seconds)

	_sync_weather_map_material()


## No simulation backlog exists anymore, so an absolute time jump (studio_time
## seek/date/advance) has nothing to reset. Kept so MCP/UI callers that still
## invoke this after a clock jump do not need a has_method guard.
func notify_time_jump() -> void:
	pass


func native_worker_busy() -> bool:
	return false


func heavy_publication_this_frame() -> bool:
	return _heavy_publication_frame == Engine.get_process_frames()


func claim_heavy_publication() -> bool:
	var frame := Engine.get_process_frames()
	if _heavy_publication_frame == frame:
		return false
	_heavy_publication_frame = frame
	return true


func solver_backlog_seconds() -> float:
	return 0.0


## direction: unit vector on the sphere. sim_seconds: CelestialSystem clock,
## already warp-scaled. Returns [coverage, storm, precip, pressure, temp,
## wind_u, wind_v], all channels used across the weather/diagnostics/products
## textures below.
func _sample_cell(direction: Vector3, sim_seconds: float) -> PackedFloat32Array:
	var circulation := float(tuning_weights[&"circulation"])
	var wind_angle := sim_seconds * WIND_RATE * circulation
	var base_dir := _rotate_around_y(direction, wind_angle)
	var detail_dir := _rotate_around_y(direction, wind_angle * 1.7)
	var pressure_dir := _rotate_around_y(direction, -wind_angle * 0.5)

	var base := _coverage_noise.get_noise_3d(base_dir.x, base_dir.y, base_dir.z) * 0.5 + 0.5
	var detail := _detail_noise.get_noise_3d(detail_dir.x, detail_dir.y, detail_dir.z) * 0.5 + 0.5
	var storm_variation := _storm_noise.get_noise_3d(
		detail_dir.x * 1.3, detail_dir.y * 1.3, detail_dir.z * 1.3) * 0.5 + 0.5
	var pressure_raw := _pressure_noise.get_noise_3d(
		pressure_dir.x, pressure_dir.y, pressure_dir.z)

	var humidity := float(tuning_weights[&"humidity"])
	var latitude_falloff := 1.0 - 0.3 * pow(absf(direction.y), 2.0)
	var coverage := clampf((base * 0.65 + detail * 0.35) * latitude_falloff * humidity, 0.0, 1.0)

	var microphysics := float(tuning_weights[&"cloud_microphysics"])
	var convection := float(tuning_weights[&"convection"])
	var storm := clampf(
		smoothstep(0.55, 0.85, coverage) * storm_variation * convection * microphysics, 0.0, 1.0)

	var precipitation := float(tuning_weights[&"precipitation"])
	var precip := clampf(storm * precipitation * 0.9, 0.0, 1.0)

	var temperature := float(tuning_weights[&"temperature"])
	var pressure := clampf(0.5 + pressure_raw * 0.35 * temperature, 0.0, 1.0)
	var temp_norm := clampf(1.0 - absf(direction.y) * 0.6 * temperature, 0.0, 1.0)
	var wind_u := sin(wind_angle * 0.3 + direction.y * 2.0) * 12.0 * circulation
	var wind_v := pressure_raw * 6.0

	return PackedFloat32Array([coverage, storm, precip, pressure, temp_norm, wind_u, wind_v])


static func _rotate_around_y(v: Vector3, angle: float) -> Vector3:
	var c := cos(angle)
	var s := sin(angle)
	return Vector3(v.x * c + v.z * s, v.y, -v.x * s + v.z * c)


func _build_global_field(sim_seconds: float) -> PackedFloat32Array:
	var packed := PackedFloat32Array()
	packed.resize(GLOBAL_W * GLOBAL_H * 4 * 3)
	for y in GLOBAL_H:
		var lat := PI * 0.5 - PI * (float(y) + 0.5) / float(GLOBAL_H)
		var sin_lat := sin(lat)
		var cos_lat := cos(lat)
		for x in GLOBAL_W:
			var lon := TAU * (float(x) + 0.5) / float(GLOBAL_W)
			var direction := Vector3(cos_lat * cos(lon), sin_lat, cos_lat * sin(lon))
			var cell := _sample_cell(direction, sim_seconds)
			var offset := (x + y * GLOBAL_W) * 4
			packed[offset + 0] = cell[0]
			packed[offset + 1] = cell[1]
			packed[offset + 2] = cell[2]
			packed[offset + 3] = cell[3]
			var diag_offset := GLOBAL_W * GLOBAL_H * 4 + offset
			packed[diag_offset + 0] = cell[4]
			packed[diag_offset + 1] = cell[0]
			packed[diag_offset + 2] = cell[1]
			packed[diag_offset + 3] = 0.0
			var prod_offset := GLOBAL_W * GLOBAL_H * 8 + offset
			packed[prod_offset + 0] = cell[4]
			packed[prod_offset + 1] = cell[5]
			packed[prod_offset + 2] = cell[6]
			packed[prod_offset + 3] = 0.0
	return packed


func _build_local_field(center: Vector3, east: Vector3, north: Vector3,
		radius: float, span: float, sim_seconds: float) -> PackedFloat32Array:
	var packed := PackedFloat32Array()
	packed.resize(LOCAL_W * LOCAL_H * 4)
	var half := span * 0.5
	for y in LOCAL_H:
		var ny := (float(y) + 0.5) / float(LOCAL_H) * span - half
		for x in LOCAL_W:
			var ex := (float(x) + 0.5) / float(LOCAL_W) * span - half
			var direction := (center + east * (ex / radius) + north * (ny / radius)).normalized()
			var cell := _sample_cell(direction, sim_seconds)
			var offset := (x + y * LOCAL_W) * 4
			packed[offset + 0] = cell[0]
			packed[offset + 1] = cell[1]
			packed[offset + 2] = cell[2]
			packed[offset + 3] = cell[3]
	return packed


func _start_global_regen(sim_seconds: float) -> void:
	_global_regen_accum = 0.0
	_global_regen_task_id = WorkerThreadPool.add_task(
		func() -> void: _pending_global_result = _build_global_field(sim_seconds),
		false, "asterra_weather_procedural_global")


func _poll_global_regen() -> void:
	if _global_regen_task_id < 0 or not WorkerThreadPool.is_task_completed(_global_regen_task_id):
		return
	WorkerThreadPool.wait_for_task_completion(_global_regen_task_id)
	_global_regen_task_id = -1
	_apply_global_regen(_pending_global_result)
	_pending_global_result = PackedFloat32Array()


func _apply_global_regen(packed: PackedFloat32Array) -> void:
	if packed.size() != GLOBAL_W * GLOBAL_H * 4 * 3:
		return
	var n := GLOBAL_W * GLOBAL_H * 4
	global_weather_values = packed.slice(0, n)
	global_diagnostics_values = packed.slice(n, n * 2)
	global_products_values = packed.slice(n * 2, n * 3)

	var weather_values := _apply_simulation_weight(global_weather_values)
	global_weather_texture.update(Image.create_from_data(
		GLOBAL_W, GLOBAL_H, false, Image.FORMAT_RGBAF, weather_values.to_byte_array()))
	var diag_values := _apply_diagnostic_weight(global_diagnostics_values)
	global_diagnostics_texture.update(Image.create_from_data(
		GLOBAL_W, GLOBAL_H, false, Image.FORMAT_RGBAF, diag_values.to_byte_array()))
	global_products_texture.update(Image.create_from_data(
		GLOBAL_W, GLOBAL_H, false, Image.FORMAT_RGBAF, global_products_values.to_byte_array()))

	global_state_revision += 1
	global_state_advanced.emit(global_state_revision)


func _start_local_regen(sim_seconds: float) -> void:
	_local_regen_accum = 0.0
	var radius := Planet.cfg.planet_radius if Planet.cfg != null else FALLBACK_PLANET_RADIUS_M
	_local_regen_task_id = WorkerThreadPool.add_task(
		func() -> void: _pending_local_result = _build_local_field(
			local_center, local_east, local_north, radius, local_span_m, sim_seconds),
		false, "asterra_weather_procedural_local")


func _poll_local_regen() -> void:
	if _local_regen_task_id < 0 or not WorkerThreadPool.is_task_completed(_local_regen_task_id):
		return
	WorkerThreadPool.wait_for_task_completion(_local_regen_task_id)
	_local_regen_task_id = -1
	_apply_local_regen(_pending_local_result)
	_pending_local_result = PackedFloat32Array()


func _apply_local_regen(packed: PackedFloat32Array) -> void:
	if packed.size() != LOCAL_W * LOCAL_H * 4:
		return
	var values := _apply_simulation_weight(packed)
	local_weather_texture.update(Image.create_from_data(
		LOCAL_W, LOCAL_H, false, Image.FORMAT_RGBAF, values.to_byte_array()))
	var diag_values := _apply_diagnostic_weight(packed)
	local_diagnostics_texture.update(Image.create_from_data(
		LOCAL_W, LOCAL_H, false, Image.FORMAT_RGBAF, diag_values.to_byte_array()))
	local_products_texture.update(Image.create_from_data(
		LOCAL_W, LOCAL_H, false, Image.FORMAT_RGBAF, packed.to_byte_array()))
	local_state_revision += 1
	local_state_advanced.emit(local_state_revision)


func _try_bind_observer() -> void:
	if _observer != null and is_instance_valid(_observer):
		return
	var root: Node = get_tree().current_scene
	if root == null:
		return
	_observer = _find_observer_recursive(root)


func _find_observer_recursive(node: Node) -> AsterraPlayer:
	if node is AsterraPlayer:
		return node as AsterraPlayer
	for child: Node in node.get_children():
		var found: AsterraPlayer = _find_observer_recursive(child)
		if found != null:
			return found
	return null


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


## Flat placeholder shown for the few seconds before the first background
## regen pass lands.
func _publish_flat_fallback() -> void:
	var global_image := Image.create(GLOBAL_W, GLOBAL_H, false, Image.FORMAT_RGBAF)
	global_image.fill(Color(NEUTRAL_CLOUD, 0.0, 0.0, 0.5))
	global_weather_texture.update(global_image)
	global_weather_values = global_image.get_data().to_float32_array()
	var local_image := Image.create(LOCAL_W, LOCAL_H, false, Image.FORMAT_RGBAF)
	local_image.fill(Color(NEUTRAL_CLOUD, 0.0, 0.0, 0.5))
	local_weather_texture.update(local_image)
	var global_diag := Image.create(GLOBAL_W, GLOBAL_H, false, Image.FORMAT_RGBAF)
	global_diag.fill(Color(0.5, 0.5, 0.5, 0.0))
	global_diagnostics_texture.update(global_diag)
	global_diagnostics_values = global_diag.get_data().to_float32_array()
	var local_diag := Image.create(LOCAL_W, LOCAL_H, false, Image.FORMAT_RGBAF)
	local_diag.fill(Color(0.5, 0.5, 0.5, 0.0))
	local_diagnostics_texture.update(local_diag)
	var global_products := Image.create(GLOBAL_W, GLOBAL_H, false, Image.FORMAT_RGBAF)
	global_products.fill(Color(0.5, 0.0, 0.0, 0.0))
	global_products_texture.update(global_products)
	global_products_values = global_products.get_data().to_float32_array()
	var local_products := Image.create(LOCAL_W, LOCAL_H, false, Image.FORMAT_RGBAF)
	local_products.fill(Color(0.5, 0.0, 0.0, 0.0))
	local_products_texture.update(local_products)


func set_simulation_weight(value: float) -> void:
	var sanitized := clampf(value, SIMULATION_WEIGHT_MIN, SIMULATION_WEIGHT_MAX)
	if is_equal_approx(simulation_weight, sanitized):
		return
	simulation_weight = sanitized
	# No cached raw-vs-weighted split is kept; the new weight lands on the
	# next regen pass, at most GLOBAL_REGEN_INTERVAL_S away.
	_global_regen_accum = maxf(_global_regen_accum, GLOBAL_REGEN_INTERVAL_S)
	_local_regen_accum = maxf(_local_regen_accum, LOCAL_REGEN_INTERVAL_S)
	_sync_weather_map_material()
	simulation_weight_changed.emit(simulation_weight)


func reset_simulation_weight() -> void:
	set_simulation_weight(SIMULATION_WEIGHT_DEFAULT)


func set_simulation_speed(value: float) -> void:
	var sanitized := clampf(value, SIMULATION_SPEED_MIN, SIMULATION_SPEED_MAX)
	if is_equal_approx(simulation_speed, sanitized) and is_equal_approx(CelestialSystem.time_scale, sanitized):
		return
	simulation_speed = sanitized
	CelestialSystem.set_time_scale(simulation_speed)
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
	tuning_weight_changed.emit(name, sanitized)


func reset_physics_tuning() -> void:
	for tuning_key: StringName in TUNING_KEYS:
		tuning_weights[tuning_key] = TUNING_WEIGHT_DEFAULT
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


func _exit_tree() -> void:
	if _global_regen_task_id >= 0:
		WorkerThreadPool.wait_for_task_completion(_global_regen_task_id)
	if _local_regen_task_id >= 0:
		WorkerThreadPool.wait_for_task_completion(_local_regen_task_id)
