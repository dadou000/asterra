class_name WindVehicleTest
extends RigidBody3D
## Lightweight boat/car/plane physics prototypes for validating terrain contact,
## altitude, floating origin and atmospheric wind against the current GPU terrain.

enum Kind { BOAT, CAR, PLANE }

const GRAVITY_M_S2 := 9.62
const SEA_LEVEL_DENSITY := 1.225
const DENSITY_SCALE_HEIGHT_M := 8500.0
const CAR_SUPPORT_POINTS: Array[Vector3] = [
	Vector3(-0.72, -0.56, -1.42), Vector3(0.72, -0.56, -1.42),
	Vector3(-0.72, -0.56, 1.42), Vector3(0.72, -0.56, 1.42)]
const PLANE_GEAR_POINTS: Array[Vector3] = [
	Vector3(0.0, -0.48, -2.10), Vector3(-2.55, -0.48, 1.05), Vector3(2.55, -0.48, 1.05)]

var kind: Kind = Kind.CAR
var controlled := false
var camera: Camera3D
var throttle := 0.0
var last_wind := Vector3.ZERO
var last_airspeed := 0.0
var last_agl := 0.0

var _area_m2 := 2.4
var _drag_coefficient := 0.35
var _max_wind_accel := 18.0
var _engine_force_n := 6000.0
var _alignment_gain := 7.0
var _alignment_damping := 2.8
var _wind_sampler: Object = null


func configure(value: Kind, direction: Vector3) -> void:
	kind = value
	gravity_scale = 0.0
	continuous_cd = true
	can_sleep = false
	contact_monitor = true
	max_contacts_reported = 8
	if ClassDB.class_exists(&"WeatherWindSampler"):
		var sampler_value: Variant = ClassDB.instantiate(&"WeatherWindSampler")
		if sampler_value is Object:
			_wind_sampler = sampler_value
	_build_shape_and_visual()
	_build_camera()
	_place(direction)
	if not Frames.origin_shifted.is_connected(_on_origin_shifted):
		Frames.origin_shifted.connect(_on_origin_shifted)


func _build_shape_and_visual() -> void:
	var shape_node := CollisionShape3D.new()
	var mesh_node := MeshInstance3D.new()
	var box := BoxShape3D.new()
	var mesh := BoxMesh.new()
	var material := StandardMaterial3D.new()
	match kind:
		Kind.BOAT:
			mass = 850.0; box.size = Vector3(2.1, 0.9, 4.8); mesh.size = box.size
			material.albedo_color = Color(0.18, 0.35, 0.52); _area_m2 = 7.0
			_drag_coefficient = 0.82; _engine_force_n = 2600.0
			_alignment_gain = 3.0; _alignment_damping = 2.4
		Kind.CAR:
			mass = 1250.0; box.size = Vector3(1.85, 1.15, 4.25); mesh.size = box.size
			material.albedo_color = Color(0.55, 0.16, 0.12); _area_m2 = 2.3
			_drag_coefficient = 0.34; _engine_force_n = 7200.0
		Kind.PLANE:
			mass = 930.0; box.size = Vector3(8.8, 0.75, 6.2); mesh.size = box.size
			material.albedo_color = Color(0.72, 0.74, 0.78); _area_m2 = 9.5
			_drag_coefficient = 0.18; _engine_force_n = 5200.0
	shape_node.shape = box
	mesh_node.mesh = mesh
	mesh_node.material_override = material
	add_child(shape_node)
	add_child(mesh_node)


func _build_camera() -> void:
	camera = Camera3D.new()
	camera.current = false
	camera.near = 0.12
	camera.fov = 72.0
	match kind:
		Kind.BOAT: camera.position = Vector3(0.0, 2.4, 6.5)
		Kind.CAR: camera.position = Vector3(0.0, 2.1, 6.4)
		Kind.PLANE: camera.position = Vector3(0.0, 2.8, 11.0)
	add_child(camera)


func _place(direction: Vector3) -> void:
	var d := direction.normalized()
	if kind == Kind.BOAT:
		var r := Planet.cfg.planet_radius + 0.45
		global_position = Frames.to_render(Vec3D.new(d.x * r, d.y * r, d.z * r))
	else:
		var clearance := 90.0 if kind == Kind.PLANE else 0.95
		global_position = Frames.to_render(TerrainContactSampler.surface_world(d, clearance))
	var tangent: Array = CubeSphere.tangent_basis(d)
	var right: Vector3 = tangent[0]
	var forward: Vector3 = tangent[1]
	global_basis = Basis(right, d, -forward).orthonormalized()
	linear_velocity = Vector3.ZERO
	angular_velocity = Vector3.ZERO
	if kind == Kind.PLANE:
		throttle = 0.72
		linear_velocity = forward * 46.0


func world_pos() -> Vec3D:
	return Frames.to_world(global_position)

func altitude_msl() -> float:
	return TerrainContactSampler.altitude_msl(world_pos())

func agl() -> float:
	return TerrainContactSampler.altitude_agl(world_pos())

func vehicle_name() -> String:
	match kind:
		Kind.BOAT: return "BOAT"
		Kind.CAR: return "CAR"
		_: return "PLANE"


func _physics_process(dt: float) -> void:
	if not Planet.ready_state:
		return
	var wp := world_pos()
	if wp.length_sq() <= 1.0:
		return
	var up := wp.normalized().to_v3()
	var altitude := TerrainContactSampler.altitude_msl(wp)
	TerrainContactSampler.request_surface(up)
	last_agl = TerrainContactSampler.altitude_agl(wp)
	last_wind = _sample_wind(up, altitude)
	apply_central_force(-up * mass * GRAVITY_M_S2)
	_apply_wind(last_wind, altitude)

	match kind:
		Kind.CAR:
			_apply_terrain_support(CAR_SUPPORT_POINTS, 0.42, 31000.0, 5200.0, mass * GRAVITY_M_S2 * 1.4)
			_apply_ground_vehicle(dt, up)
		Kind.BOAT:
			_apply_boat(dt, up, altitude)
		Kind.PLANE:
			_apply_terrain_support(PLANE_GEAR_POINTS, 0.30, 42000.0, 6500.0, mass * GRAVITY_M_S2 * 2.2)
			_apply_plane(dt, up, last_wind)
	if controlled:
		Frames.maintain_origin(Frames.to_render(wp))


## Height-field rigid-body support. Probe points are transformed to canonical
## planet directions, queried in a batch, then receive spring/damper forces along
## the sampled terrain normal. This replaces dependence on giant ConcavePolygon
## terrain tiles and works across floating-origin shifts.
func _apply_terrain_support(points: Array[Vector3], rest_length: float,
		stiffness: float, damping: float, max_force_per_point: float) -> void:
	var dirs: Array[Vector3] = []
	var probe_render: Array[Vector3] = []
	var probe_world: Array[Vec3D] = []
	for local_point: Vector3 in points:
		var rp := global_transform * local_point
		var wp := Frames.to_world(rp)
		if wp.length_sq() <= 1.0:
			continue
		probe_render.append(rp)
		probe_world.append(wp)
		dirs.append(wp.normalized().to_v3())
	TerrainContactSampler.request_surfaces(dirs)
	for i in dirs.size():
		var surface := TerrainContactSampler.surface(dirs[i])
		var terrain_r := Planet.cfg.planet_radius + float(surface["height"])
		var gap := probe_world[i].length() - terrain_r
		var compression := rest_length - gap
		if compression <= 0.0:
			continue
		var normal: Vector3 = surface["normal"]
		var offset := probe_render[i] - global_position
		var point_velocity := linear_velocity + angular_velocity.cross(offset)
		var normal_speed := point_velocity.dot(normal)
		var magnitude := clampf(stiffness * compression - damping * normal_speed,
			0.0, max_force_per_point)
		if magnitude > 0.0:
			apply_force(normal * magnitude, offset)


func _apply_wind(wind: Vector3, altitude: float) -> void:
	var relative := wind - linear_velocity
	var speed := relative.length()
	last_airspeed = speed
	if speed <= 0.01: return
	var rho := SEA_LEVEL_DENSITY * exp(-maxf(altitude, 0.0) / DENSITY_SCALE_HEIGHT_M)
	var force := relative * (0.5 * rho * _drag_coefficient * _area_m2 * speed)
	var max_force := mass * _max_wind_accel
	if force.length() > max_force: force = force.normalized() * max_force
	apply_central_force(force)


func _apply_ground_vehicle(_dt: float, up: Vector3) -> void:
	_apply_radial_alignment(up)
	var forward := -global_basis.z
	forward = (forward - up * forward.dot(up)).normalized()
	var right := global_basis.x
	right = (right - up * right.dot(up)).normalized()
	var drive := Input.get_action_strength("move_forward") - Input.get_action_strength("move_back") if controlled else 0.0
	var steer := Input.get_action_strength("move_left") - Input.get_action_strength("move_right") if controlled else 0.0
	if absf(drive) > 0.001: apply_central_force(forward * drive * _engine_force_n)
	if absf(steer) > 0.001:
		var steer_scale := clampf(linear_velocity.length() / 4.0, 0.25, 2.0)
		apply_torque(up * steer * mass * 1.8 * steer_scale)
	var lateral_speed := linear_velocity.dot(right)
	apply_central_force(-right * clampf(lateral_speed * mass * 4.5, -mass * 9.0, mass * 9.0))


func _apply_boat(_dt: float, up: Vector3, altitude: float) -> void:
	_apply_radial_alignment(up)
	var waterline := 0.45
	var immersion := clampf((waterline - altitude + 0.20) / 0.80, 0.0, 1.35)
	if immersion > 0.0:
		apply_central_force(up * mass * GRAVITY_M_S2 * immersion * 1.04)
		var radial_speed := linear_velocity.dot(up)
		apply_central_force(-up * radial_speed * mass * 2.6 * immersion)
		var tangent_velocity := linear_velocity - up * radial_speed
		apply_central_force(-tangent_velocity * mass * 0.24 * immersion)
	var forward := -global_basis.z
	forward = (forward - up * forward.dot(up)).normalized()
	var throttle_input := Input.get_action_strength("move_forward") - Input.get_action_strength("move_back") if controlled else 0.0
	var rudder := Input.get_action_strength("move_left") - Input.get_action_strength("move_right") if controlled else 0.0
	if absf(throttle_input) > 0.001: apply_central_force(forward * throttle_input * _engine_force_n)
	if absf(rudder) > 0.001:
		var water_authority := clampf(linear_velocity.length() / 3.0, 0.2, 2.0)
		apply_torque(up * rudder * mass * 1.25 * water_authority)


func _apply_plane(dt: float, _planet_up: Vector3, wind: Vector3) -> void:
	var forward := -global_basis.z.normalized()
	var wing_up := global_basis.y.normalized()
	var right := global_basis.x.normalized()
	if controlled:
		throttle += (Input.get_action_strength("move_up") - Input.get_action_strength("move_down")) * dt * 0.45
		throttle = clampf(throttle, 0.0, 1.0)
		var pitch_input := Input.get_action_strength("move_back") - Input.get_action_strength("move_forward")
		var roll_input := Input.get_action_strength("move_right") - Input.get_action_strength("move_left")
		apply_torque(right * pitch_input * mass * 3.4)
		apply_torque(forward * -roll_input * mass * 4.0)
	apply_central_force(forward * throttle * _engine_force_n)
	var air_velocity := linear_velocity - wind
	var forward_speed := maxf(air_velocity.dot(forward), 0.0)
	var altitude := maxf(altitude_msl(), 0.0)
	var rho := SEA_LEVEL_DENSITY * exp(-altitude / DENSITY_SCALE_HEIGHT_M)
	var wing_area := 16.2
	var lift_coefficient := clampf(0.52 + angular_velocity.dot(right) * 0.025, 0.18, 1.15)
	var lift := minf(0.5 * rho * wing_area * lift_coefficient * forward_speed * forward_speed,
		mass * GRAVITY_M_S2 * 4.5)
	apply_central_force(wing_up * lift)
	var damping := clampf(forward_speed / 55.0, 0.0, 1.8)
	apply_torque(-angular_velocity * mass * 0.75 * damping)


func _apply_radial_alignment(up: Vector3) -> void:
	var body_up := global_basis.y.normalized()
	var error_axis := body_up.cross(up)
	var torque := error_axis * mass * _alignment_gain - angular_velocity * mass * _alignment_damping
	var limit := mass * 18.0
	if torque.length() > limit: torque = torque.normalized() * limit
	apply_torque(torque)


func _sample_wind(direction: Vector3, altitude: float) -> Vector3:
	var weather := get_node_or_null("/root/WeatherSystem")
	if weather != null:
		var native_value: Variant = weather.get("_native")
		if _wind_sampler != null and native_value is Object and _wind_sampler.has_method(&"sample_wind"):
			var sampled: Variant = _wind_sampler.call(&"sample_wind", native_value, direction, altitude)
			if sampled is Vector3: return sampled
	var sample := Planet.sample_info(direction)
	var baked: Variant = sample.get("wind", Vector2.ZERO)
	if not (baked is Vector2): return Vector3.ZERO
	var n := direction.normalized()
	var east := Vector3(-n.z, 0.0, n.x)
	if east.length_squared() < 1e-8: east = Vector3.RIGHT
	else: east = east.normalized()
	var north := east.cross(n).normalized()
	return east * baked.x + north * baked.y


func _on_origin_shifted(delta_render: Vector3) -> void:
	global_position += delta_render
