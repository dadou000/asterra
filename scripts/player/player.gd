class_name AsterraPlayer
extends Node3D
## Observer + excavation tool. Precise ground contact comes from the asynchronous
## GPU terrain query through TerrainContactSampler; CPU queries remain coarse
## resident-map fallbacks only.

signal moved(world_pos: Vec3D)

enum Mode { WALK, FLY }

const EYE_HEIGHT := 1.72
const WALK_SPEED := 4.6
const RUN_SPEED := 11.0
const JUMP_SPEED := 5.2
const GRAVITY := 9.62
const MOUSE_SENS := 0.0022

@export var mode: Mode = Mode.FLY

var world_pos: Vec3D = Vec3D.new(0, 0, 0)
var yaw: float = 0.0
var pitch: float = 0.0
var vertical_speed: float = 0.0
var grounded: bool = false
var camera: Camera3D

var _mouse_captured := false
var input_enabled := true


func _ready() -> void:
	camera = Camera3D.new()
	camera.near = 0.25
	camera.far = 400000.0
	camera.fov = 68.0
	add_child(camera)
	Frames.origin_shifted.connect(func(_d): _sync_transform())
	set_process_input(true)


func spawn_at(dir: Vector3, spawn_altitude: float) -> void:
	var d := dir.normalized()
	var h := TerrainContactSampler.height(d)
	var r := Planet.cfg.planet_radius + h + spawn_altitude
	world_pos = Vec3D.new(d.x * r, d.y * r, d.z * r)
	Frames.rebase(world_pos)
	_sync_transform()
	TerrainContactSampler.request_height(d)
	TerrainContactSampler.request_contact_height(d)
	moved.emit(world_pos)


func up_dir() -> Vector3:
	return world_pos.normalized().to_v3()

func altitude() -> float:
	return TerrainContactSampler.altitude_msl(world_pos)

func ground_height() -> float:
	var d: Vector3 = up_dir()
	var broad_height: float = TerrainContactSampler.height(d)
	return TerrainContactSampler.contact_height(d, broad_height)

func height_above_ground() -> float:
	return TerrainContactSampler.altitude_agl(world_pos)


func _input(event: InputEvent) -> void:
	if not input_enabled:
		return
	if event is InputEventMouseMotion and _mouse_captured:
		yaw += event.relative.x * MOUSE_SENS
		pitch = clampf(pitch - event.relative.y * MOUSE_SENS, -1.55, 1.55)
	elif event is InputEventKey and event.pressed and not event.echo and event.keycode == KEY_F:
		mode = Mode.WALK if mode == Mode.FLY else Mode.FLY
		vertical_speed = 0.0


func set_mouse_captured(v: bool) -> void:
	_mouse_captured = v
	Input.mouse_mode = Input.MOUSE_MODE_CAPTURED if v else Input.MOUSE_MODE_VISIBLE


func _physics_process(dt: float) -> void:
	if not Planet.ready_state or not input_enabled:
		return
	var up := up_dir()
	TerrainContactSampler.request_surface(up)
	TerrainContactSampler.request_contact_height(up)
	var basis_local := _local_basis(up)
	var fwd: Vector3 = basis_local[0]
	var right: Vector3 = basis_local[1]
	var wish := Vector3.ZERO
	if Input.is_action_pressed("move_forward"): wish += fwd
	if Input.is_action_pressed("move_back"): wish -= fwd
	if Input.is_action_pressed("move_right"): wish += right
	if Input.is_action_pressed("move_left"): wish -= right

	if mode == Mode.FLY:
		var alt := maxf(height_above_ground(), 1.0)
		var speed := clampf(alt * 0.55, 8.0, 90000.0)
		if Input.is_action_pressed("sprint"): speed *= 6.0
		if Input.is_action_pressed("move_up"): wish += up
		if Input.is_action_pressed("move_down"): wish -= up
		if wish.length() > 0.001:
			wish = wish.normalized()
			world_pos = world_pos.add(Vec3D.from_v3(wish).mul(speed * dt))
		grounded = false
		vertical_speed = 0.0
	else:
		var speed := RUN_SPEED if Input.is_action_pressed("sprint") else WALK_SPEED
		if wish.length() > 0.001:
			wish = wish.normalized()
			world_pos = world_pos.add(Vec3D.from_v3(wish).mul(speed * dt))
		up = world_pos.normalized().to_v3()
		var broad_height: float = TerrainContactSampler.height(up)
		var gh: float = TerrainContactSampler.contact_height(up, broad_height)
		var target_r := Planet.cfg.planet_radius + gh + EYE_HEIGHT
		var r := world_pos.length()
		vertical_speed -= GRAVITY * dt
		if Input.is_action_just_pressed("move_up") and grounded:
			vertical_speed = JUMP_SPEED
		r += vertical_speed * dt
		if r <= target_r:
			r = target_r
			vertical_speed = 0.0
			grounded = true
		else:
			grounded = false
		world_pos = Vec3D.from_v3(up).mul(r)

	Frames.maintain_origin(Frames.to_render(world_pos))
	_sync_transform()
	moved.emit(world_pos)


func _local_basis(up: Vector3) -> Array:
	var ref := Vector3(0, 1, 0)
	if absf(up.dot(ref)) > 0.995:
		ref = Vector3(1, 0, 0)
	var east := ref.cross(up).normalized()
	var north := up.cross(east).normalized()
	var fwd := (north * cos(yaw) + east * sin(yaw)).normalized()
	var right := fwd.cross(up).normalized()
	return [fwd, right]


func _sync_transform() -> void:
	position = Frames.to_render(world_pos)
	var up := up_dir()
	var basis_local := _local_basis(up)
	var fwd: Vector3 = basis_local[0]
	var flat_right: Vector3 = basis_local[1]
	var look := (fwd * cos(pitch) + up * sin(pitch)).normalized()
	var cam_right := look.cross(up).normalized()
	if cam_right.length() < 1e-4:
		cam_right = flat_right
	var true_up := cam_right.cross(look).normalized()
	camera.transform.basis = Basis(cam_right, true_up, -look)
	var r_obs := Planet.cfg.planet_radius + maxf(altitude(), 0.0)
	var horizon_dist := sqrt(maxf(r_obs * r_obs - Planet.cfg.planet_radius * Planet.cfg.planet_radius, 0.0))
	camera.far = clampf(horizon_dist + 20000.0, 50000.0, 8.0e6)


func view_dir() -> Vector3:
	return -camera.global_transform.basis.z


## GPU-backed asynchronous terrain raymarch. Every gap sample requests the same
## authoritative terrain field used for contact. While a sample is pending the
## resident coarse height is used as a conservative fallback; subsequent frames
## converge to the precise procedural/edit surface from the batched query cache.
func aim(max_range: float = 220.0) -> Dictionary:
	var origin := world_pos
	var ray := view_dir()
	var step := 0.35
	var t := 0.0
	while t < max_range:
		t += step
		step = minf(step * 1.08, 4.0)
		var gap := _gap_at(origin, ray, t)
		if gap <= 0.0:
			var lo := maxf(t - step, 0.0)
			var hi := t
			for _i in 18:
				var mid := (lo + hi) * 0.5
				if _gap_at(origin, ray, mid) <= 0.0:
					hi = mid
				else:
					lo = mid
			var p := origin.add(Vec3D.from_v3(ray).mul(hi))
			var d := p.normalized().to_v3()
			var h := TerrainContactSampler.height(d)
			var surface_p := Vec3D.from_v3(d).mul(Planet.cfg.planet_radius + h)
			return {"world": surface_p, "dir": d, "distance": hi, "height": h}
	return {}


func _gap_at(origin: Vec3D, ray: Vector3, t: float) -> float:
	var p := origin.add(Vec3D.from_v3(ray).mul(t))
	var d := p.normalized().to_v3()
	var h := TerrainContactSampler.height(d)
	return p.length() - (Planet.cfg.planet_radius + h)


func state() -> Dictionary:
	return {"x": world_pos.x, "y": world_pos.y, "z": world_pos.z,
		"yaw": yaw, "pitch": pitch, "mode": int(mode)}

func restore(s: Dictionary) -> void:
	world_pos = Vec3D.new(s["x"], s["y"], s["z"])
	yaw = s.get("yaw", 0.0)
	pitch = s.get("pitch", 0.0)
	mode = int(s.get("mode", Mode.FLY)) as Mode
	Frames.rebase(world_pos)
	_sync_transform()
	TerrainContactSampler.request_surface(up_dir())
	TerrainContactSampler.request_contact_height(up_dir())
	moved.emit(world_pos)
