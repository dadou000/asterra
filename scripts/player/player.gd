class_name AsterraPlayer
extends Node3D
## Observer + excavation tool. Precise ground contact comes from the asynchronous
## GPU terrain query through TerrainContactSampler; CPU queries remain coarse
## resident-map fallbacks only.
##
## Press P to toggle the experimental force/torque-driven physics walker. The
## legacy WALK/FLY controller remains intact so the test can be compared against
## the existing traversal without changing the authoritative terrain stack.

signal moved(world_pos: Vec3D)

enum Mode { WALK, FLY }

const EYE_HEIGHT := 1.72
const WALK_SPEED := 4.6
const RUN_SPEED := 11.0
const JUMP_SPEED := 5.2
const GRAVITY := 9.62
const MOUSE_SENS := 0.0022
const PHYSICS_CAMERA_BACK := 3.6
const PHYSICS_CAMERA_UP := 0.55

# Aim is deliberately two-stage. The broad ray uses the cheap resident macro field;
# only the final candidate requests centimetre-grade GPU terrain. The old path sent
# 50-90 GPU-backed height lookups per rendered frame and each lookup searched the
# query cache/pending queue, producing a strongly state-dependent CPU/compute load.
const AIM_COARSE_CAPTURE_M: float = 16.0
const AIM_COARSE_STEP_START_M: float = 0.5
const AIM_COARSE_STEP_MAX_M: float = 6.0
const AIM_COARSE_STEP_GROWTH: float = 1.12
const AIM_BINARY_STEPS: int = 10

@export var mode: Mode = Mode.FLY

var world_pos: Vec3D = Vec3D.new(0, 0, 0)
var yaw: float = 0.0
var pitch: float = 0.0
var vertical_speed: float = 0.0
var grounded: bool = false
var camera: Camera3D

var physics_walk_enabled := false
var physics_body: PhysicsWalkerBody
var physics_visual: PhysicsWalkerVisual

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
	call_deferred("_setup_physics_walker")


func _setup_physics_walker() -> void:
	if physics_body != null or get_parent() == null:
		return
	physics_body = PhysicsWalkerBody.new()
	physics_body.name = "PhysicsWalkerBody"
	get_parent().add_child(physics_body)

	physics_visual = PhysicsWalkerVisual.new()
	physics_visual.name = "PhysicsWalkerVisual"
	physics_visual.body = physics_body
	get_parent().add_child(physics_visual)
	physics_visual.visible = false


func spawn_at(dir: Vector3, spawn_altitude: float) -> void:
	if physics_walk_enabled:
		_disable_physics_walker()
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
	elif event is InputEventKey and event.pressed and not event.echo:
		if event.keycode == KEY_P:
			_toggle_physics_walker()
		elif event.keycode == KEY_F and not physics_walk_enabled:
			mode = Mode.WALK if mode == Mode.FLY else Mode.FLY
			vertical_speed = 0.0


func set_mouse_captured(v: bool) -> void:
	_mouse_captured = v
	Input.mouse_mode = Input.MOUSE_MODE_CAPTURED if v else Input.MOUSE_MODE_VISIBLE


func _toggle_physics_walker() -> void:
	if physics_body == null:
		_setup_physics_walker()
	if physics_body == null:
		push_warning("Physics walker is not ready yet")
		return
	if physics_walk_enabled:
		_disable_physics_walker()
	else:
		_enable_physics_walker()


func _enable_physics_walker() -> void:
	if physics_body == null:
		return
	var up := up_dir()
	var basis_local := _local_basis(up)
	var forward: Vector3 = basis_local[0]
	physics_body.activate_at(up, forward)
	physics_walk_enabled = true
	mode = Mode.WALK
	vertical_speed = 0.0
	grounded = false
	if physics_visual != null:
		physics_visual.visible = true
	_sync_from_physics_body()
	_sync_transform()


func _disable_physics_walker() -> void:
	if physics_body == null:
		physics_walk_enabled = false
		return
	if physics_body.active:
		_sync_from_physics_body()
	physics_body.deactivate()
	physics_walk_enabled = false
	mode = Mode.WALK
	vertical_speed = 0.0
	grounded = true
	if physics_visual != null:
		physics_visual.visible = false
	_sync_transform()


func _sync_from_physics_body() -> void:
	if physics_body == null or not physics_body.active:
		return
	var body_world := physics_body.world_position()
	if body_world.length_sq() <= 1.0:
		return
	var up := body_world.normalized().to_v3()
	world_pos = body_world.add(Vec3D.from_v3(up).mul(EYE_HEIGHT - physics_body.com_height))
	grounded = physics_body.grounded
	vertical_speed = physics_body.linear_velocity.dot(up)


func _physics_process(dt: float) -> void:
	if not Planet.ready_state or not input_enabled:
		return

	if physics_walk_enabled:
		_physics_walk_process()
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


func _physics_walk_process() -> void:
	if physics_body == null or not physics_body.active:
		physics_walk_enabled = false
		return
	_sync_from_physics_body()
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
	var speed := RUN_SPEED if Input.is_action_pressed("sprint") else WALK_SPEED
	var target_velocity := Vector3.ZERO
	if wish.length_squared() > 1e-6:
		target_velocity = wish.normalized() * speed
	physics_body.set_control(target_velocity, fwd)
	if Input.is_action_just_pressed("move_up"):
		physics_body.request_jump()

	Frames.maintain_origin(physics_body.global_position)
	_sync_from_physics_body()
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
	if physics_walk_enabled:
		camera.position = -fwd * PHYSICS_CAMERA_BACK + up * PHYSICS_CAMERA_UP
	else:
		camera.position = Vector3.ZERO
	var r_obs := Planet.cfg.planet_radius + maxf(altitude(), 0.0)
	var horizon_dist := sqrt(maxf(r_obs * r_obs - Planet.cfg.planet_radius * Planet.cfg.planet_radius, 0.0))
	camera.far = clampf(horizon_dist + 20000.0, 50000.0, 8.0e6)


func view_dir() -> Vector3:
	return -camera.global_transform.basis.z


func physics_walker_state() -> Dictionary:
	if physics_body == null:
		return {"enabled": false}
	var result := physics_body.debug_state()
	result["enabled"] = physics_walk_enabled
	return result


## Two-stage terrain raycast.
##
## Stage 1 walks the cheap resident macro field and uses a small capture envelope so
## fine procedural relief cannot easily be skipped. Stage 2 submits only the final
## candidate to the strict contact query, then applies one radial Newton correction.
## This keeps the editing cursor converging to the authoritative GPU/deformed terrain
## without flooding TerrainHeightQuery with dozens of unique probes every frame.
func aim(max_range: float = 220.0) -> Dictionary:
	if Planet.cfg == null or not Planet.ready_state:
		return {}
	var origin: Vec3D = world_pos
	var ray: Vector3 = view_dir()
	var step: float = AIM_COARSE_STEP_START_M
	var previous_t: float = 0.0
	var t: float = 0.0
	while t < max_range:
		previous_t = t
		t = minf(t + step, max_range)
		step = minf(step * AIM_COARSE_STEP_GROWTH, AIM_COARSE_STEP_MAX_M)
		if _coarse_gap_at(origin, ray, t) <= AIM_COARSE_CAPTURE_M:
			var lo: float = previous_t
			var hi: float = t
			for _i: int in AIM_BINARY_STEPS:
				var mid: float = (lo + hi) * 0.5
				if _coarse_gap_at(origin, ray, mid) <= AIM_COARSE_CAPTURE_M:
					hi = mid
				else:
					lo = mid

			var hit_t: float = hi
			var p: Vec3D = origin.add(Vec3D.from_v3(ray).mul(hit_t))
			var d: Vector3 = p.normalized().to_v3()
			var coarse_h: float = TerrainContactSampler.coarse_height(d)
			var h: float = TerrainContactSampler.contact_height(d, coarse_h)

			# Correct the conservative capture-shell hit onto the best terrain height
			# currently available. Once the asynchronous contact sample arrives this
			# converges to the exact procedural/edit/deformation surface on the next frame.
			var precise_gap: float = p.length() - (Planet.cfg.planet_radius + h)
			var radial_rate: float = ray.dot(d)
			if absf(radial_rate) > 0.08:
				hit_t = clampf(hit_t - precise_gap / radial_rate, 0.0, max_range)
				p = origin.add(Vec3D.from_v3(ray).mul(hit_t))
				d = p.normalized().to_v3()
				coarse_h = TerrainContactSampler.coarse_height(d)
				h = TerrainContactSampler.contact_height(d, coarse_h)

			var surface_p: Vec3D = Vec3D.from_v3(d).mul(Planet.cfg.planet_radius + h)
			return {"world": surface_p, "dir": d, "distance": hit_t, "height": h}
	return {}


func _coarse_gap_at(origin: Vec3D, ray: Vector3, t: float) -> float:
	var p: Vec3D = origin.add(Vec3D.from_v3(ray).mul(t))
	var d: Vector3 = p.normalized().to_v3()
	var h: float = TerrainContactSampler.coarse_height(d)
	return p.length() - (Planet.cfg.planet_radius + h)


func state() -> Dictionary:
	return {"x": world_pos.x, "y": world_pos.y, "z": world_pos.z,
		"yaw": yaw, "pitch": pitch, "mode": int(mode),
		"physics_walk": physics_walk_enabled}

func restore(s: Dictionary) -> void:
	if physics_walk_enabled:
		_disable_physics_walker()
	world_pos = Vec3D.new(s["x"], s["y"], s["z"])
	yaw = s.get("yaw", 0.0)
	pitch = s.get("pitch", 0.0)
	mode = int(s.get("mode", Mode.FLY)) as Mode
	Frames.rebase(world_pos)
	_sync_transform()
	TerrainContactSampler.request_surface(up_dir())
	TerrainContactSampler.request_contact_height(up_dir())
	moved.emit(world_pos)
