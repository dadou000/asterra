extends Node
## Velocity-aware terrain height prefetcher.
##
## The spherical geometry clipmap already requests its complete visible L0-L14
## hierarchy. This node only predicts the local pages needed soon by feet, wheels
## and near-field rendering; it must not fan one future probe through all 15
## planet levels.

const ACTIVE_AGL_M := 4500.0
const PREFETCH_INTERVAL := 0.15
const LOOKAHEAD_SECONDS := 3.0
const MAX_LOOKAHEAD_M := 1800.0
const MIN_PREFETCH_SPEED_MPS := 6.0
const TELEPORT_SPEED_MPS := 2200.0
const VELOCITY_FILTER := 0.28
const PREFETCH_PRIORITY_NEAR := 20.0
const PREFETCH_PRIORITY_FAR := 32.0

var _have_position := false
var _last_planet_pos := Vector3.ZERO
var _surface_velocity := Vector3.ZERO
var _prefetch_left := 0.0
var _last_speed := 0.0
var _last_lookahead := 0.0


func _ready() -> void:
	process_priority = 6


func _process(dt: float) -> void:
	if not Planet.ready_state or Planet.cfg == null:
		_reset_motion()
		return

	var camera: Camera3D = get_viewport().get_camera_3d()
	if camera == null:
		_reset_motion()
		return

	var origin: Vector3 = Vector3(float(Frames.origin.x), float(Frames.origin.y), float(Frames.origin.z))
	var planet_pos: Vector3 = camera.global_position + origin
	if planet_pos.length_squared() <= 1.0:
		_reset_motion()
		return

	var observer_dir: Vector3 = planet_pos.normalized()
	_update_motion(planet_pos, observer_dir, dt)

	var agl: float = maxf(planet_pos.length() - Planet.cfg.planet_radius
		- Planet.macro_height(observer_dir), 0.0)
	if agl > ACTIVE_AGL_M:
		return

	_prefetch_left -= dt
	if _prefetch_left > 0.0:
		return
	_prefetch_left = PREFETCH_INTERVAL
	_prefetch_corridor(observer_dir)


func _update_motion(planet_pos: Vector3, observer_dir: Vector3, dt: float) -> void:
	if not _have_position:
		_last_planet_pos = planet_pos
		_have_position = true
		_surface_velocity = Vector3.ZERO
		_last_speed = 0.0
		return

	var safe_dt: float = maxf(dt, 1.0 / 240.0)
	var raw_velocity: Vector3 = (planet_pos - _last_planet_pos) / safe_dt
	_last_planet_pos = planet_pos

	if raw_velocity.length() > TELEPORT_SPEED_MPS:
		_surface_velocity = Vector3.ZERO
		_last_speed = 0.0
		return

	var tangent_velocity: Vector3 = raw_velocity - observer_dir * raw_velocity.dot(observer_dir)
	_surface_velocity = _surface_velocity.lerp(tangent_velocity, VELOCITY_FILTER)
	_last_speed = _surface_velocity.length()


func _prefetch_corridor(observer_dir: Vector3) -> void:
	var speed: float = _surface_velocity.length()
	if speed < MIN_PREFETCH_SPEED_MPS:
		_last_lookahead = 0.0
		return

	var radius: float = float(Planet.cfg.planet_radius)
	var forward: Vector3 = _surface_velocity / speed
	var side: Vector3 = observer_dir.cross(forward)
	if side.length_squared() < 1e-8:
		return
	side = side.normalized()

	var lookahead: float = minf(speed * LOOKAHEAD_SECONDS, MAX_LOOKAHEAD_M)
	_last_lookahead = lookahead
	_prefetch_station(observer_dir, forward, side, lookahead * 0.50, radius,
		PREFETCH_PRIORITY_NEAR)
	_prefetch_station(observer_dir, forward, side, lookahead, radius,
		PREFETCH_PRIORITY_FAR)


func _prefetch_station(observer_dir: Vector3, forward: Vector3, side: Vector3,
		ahead_m: float, radius: float, priority: float) -> void:
	# Explicit local levels. The spherical renderer owns coarser planet pages.
	var bands: Array[Vector2] = [
		Vector2(0.0, 36.0),
		Vector2(2.0, 180.0),
		Vector2(4.0, 620.0),
	]
	var lateral_factors := PackedFloat32Array([-1.0, 0.0, 1.0])
	for band: Vector2 in bands:
		var level: int = int(band.x)
		var half_width: float = band.y
		for lateral_factor: float in lateral_factors:
			var offset: Vector3 = forward * ahead_m + side * (half_width * lateral_factor)
			var d: Vector3 = (observer_dir + offset / radius).normalized()
			GroundHeightStore.request_sample(d, level, priority)


func stats() -> Dictionary:
	return {
		"speed_mps": _last_speed,
		"lookahead_m": _last_lookahead,
	}


func _reset_motion() -> void:
	_have_position = false
	_surface_velocity = Vector3.ZERO
	_last_speed = 0.0
	_last_lookahead = 0.0
	_prefetch_left = 0.0
