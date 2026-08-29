extends Node3D
## Isolated passive 19-body ragdoll validation scene.
##
## Deliberately contains no motors, animation authority, terrain contacts,
## balance controller or neural policy. This is the raw Jolt articulation gate.

const SPAWN_OFFSET := Vector3(0.0, 0.55, 0.0)
const FLOOR_SIZE := Vector3(12.0, 0.20, 12.0)
const EXPECTED_BODY_COUNT := 19
const EXPECTED_JOINT_COUNT := 18

var _bodies: Dictionary = {}
var _initial_positions: Dictionary = {}
var _joint_count := 0
var _camera: Camera3D
var _camera_target := Vector3(0.0, 1.05, 0.0)
var _camera_yaw := 0.55
var _camera_pitch := -0.08
var _camera_distance := 4.2
var _orbit_dragging := false


func _ready() -> void:
	_make_environment()
	_make_floor()
	_make_camera()
	_make_overlay()
	_build_ragdoll()
	_assert_articulation_contract()
	_reset_ragdoll()


func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventKey and event.pressed and not event.echo:
		if event.keycode == KEY_R:
			_reset_ragdoll()
		elif event.keycode == KEY_ESCAPE:
			_orbit_dragging = false

	if event is InputEventMouseButton:
		if event.button_index == MOUSE_BUTTON_RIGHT:
			_orbit_dragging = event.pressed
		elif event.pressed and event.button_index == MOUSE_BUTTON_WHEEL_UP:
			_camera_distance = maxf(1.8, _camera_distance - 0.30)
			_update_camera()
		elif event.pressed and event.button_index == MOUSE_BUTTON_WHEEL_DOWN:
			_camera_distance = minf(9.0, _camera_distance + 0.30)
			_update_camera()

	if event is InputEventMouseMotion and _orbit_dragging:
		_camera_yaw -= event.relative.x * 0.006
		_camera_pitch = clampf(_camera_pitch - event.relative.y * 0.006, -1.15, 0.95)
		_update_camera()


func _make_environment() -> void:
	var world_environment := WorldEnvironment.new()
	world_environment.name = "WorldEnvironment"
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color(0.025, 0.03, 0.04)
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.ambient_light_color = Color(0.55, 0.58, 0.64)
	environment.ambient_light_energy = 0.55
	world_environment.environment = environment
	add_child(world_environment)

	var sun := DirectionalLight3D.new()
	sun.name = "Sun"
	sun.rotation_degrees = Vector3(-48.0, -28.0, 0.0)
	sun.light_energy = 1.4
	sun.shadow_enabled = true
	add_child(sun)


func _make_floor() -> void:
	var floor := StaticBody3D.new()
	floor.name = "RigidFloor"
	floor.position = Vector3(0.0, -0.10, 0.0)
	add_child(floor)

	var collision := CollisionShape3D.new()
	var shape := BoxShape3D.new()
	shape.size = FLOOR_SIZE
	collision.shape = shape
	floor.add_child(collision)

	var visual := MeshInstance3D.new()
	var mesh := BoxMesh.new()
	mesh.size = FLOOR_SIZE
	visual.mesh = mesh
	var material := StandardMaterial3D.new()
	material.albedo_color = Color(0.14, 0.15, 0.17)
	material.roughness = 0.88
	visual.material_override = material
	floor.add_child(visual)

	var physics_material := PhysicsMaterial.new()
	physics_material.friction = 0.90
	physics_material.bounce = 0.02
	floor.physics_material_override = physics_material


func _make_camera() -> void:
	_camera = Camera3D.new()
	_camera.name = "OrbitCamera"
	_camera.fov = 48.0
	add_child(_camera)
	_update_camera()


func _update_camera() -> void:
	if _camera == null:
		return
	var horizontal := cos(_camera_pitch) * _camera_distance
	var offset := Vector3(
		sin(_camera_yaw) * horizontal,
		sin(_camera_pitch) * _camera_distance,
		cos(_camera_yaw) * horizontal
	)
	_camera.global_position = _camera_target + offset
	_camera.look_at(_camera_target, Vector3.UP)


func _make_overlay() -> void:
	var canvas := CanvasLayer.new()
	canvas.name = "Overlay"
	add_child(canvas)

	var label := Label.new()
	label.position = Vector2(18.0, 16.0)
	label.text = "19-BODY PASSIVE RAGDOLL / JOLT / 120 Hz\nR: reset   RMB drag: orbit   wheel: zoom\nNo motors • no animation • no balance • no neural policy"
	label.add_theme_font_size_override("font_size", 16)
	canvas.add_child(label)


func _build_ragdoll() -> void:
	for spec in _body_specs():
		_make_body(spec)

	# Disable all internal rigid-body collisions for this first articulation gate.
	# This isolates joint stability and floor contact from self-collision tuning.
	var names: Array = _bodies.keys()
	for i in range(names.size()):
		for j in range(i + 1, names.size()):
			var body_a: RigidBody3D = _bodies[names[i]]
			var body_b: RigidBody3D = _bodies[names[j]]
			body_a.add_collision_exception_with(body_b)
			body_b.add_collision_exception_with(body_a)

	for joint_spec in _joint_specs():
		_make_joint(joint_spec)


func _body_specs() -> Array:
	return [
		{"name":"pelvis",          "kind":"box",     "size":Vector3(0.30,0.20,0.20),  "pos":Vector3(0.00,1.00,0.00),   "mass":10.5},
		{"name":"spine",           "kind":"box",     "size":Vector3(0.27,0.24,0.18),  "pos":Vector3(0.00,1.22,0.00),   "mass":7.0},
		{"name":"chest",           "kind":"box",     "size":Vector3(0.42,0.28,0.20),  "pos":Vector3(0.00,1.48,0.00),   "mass":15.0},
		{"name":"neck",            "kind":"capsule", "size":Vector3(0.06,0.13,0.00),  "pos":Vector3(0.00,1.69,0.00),   "mass":1.0},
		{"name":"head",            "kind":"sphere",  "size":Vector3(0.12,0.00,0.00),  "pos":Vector3(0.00,1.88,0.00),   "mass":5.0},
		{"name":"left_clavicle",   "kind":"box",     "size":Vector3(0.18,0.07,0.08),  "pos":Vector3(-0.23,1.58,0.00),  "mass":0.7},
		{"name":"right_clavicle",  "kind":"box",     "size":Vector3(0.18,0.07,0.08),  "pos":Vector3(0.23,1.58,0.00),   "mass":0.7},
		{"name":"left_upper_arm",  "kind":"capsule", "size":Vector3(0.055,0.31,0.00), "pos":Vector3(-0.39,1.38,0.00),  "mass":2.2},
		{"name":"right_upper_arm", "kind":"capsule", "size":Vector3(0.055,0.31,0.00), "pos":Vector3(0.39,1.38,0.00),   "mass":2.2},
		{"name":"left_forearm",    "kind":"capsule", "size":Vector3(0.045,0.28,0.00), "pos":Vector3(-0.39,1.08,0.00),  "mass":1.3},
		{"name":"right_forearm",   "kind":"capsule", "size":Vector3(0.045,0.28,0.00), "pos":Vector3(0.39,1.08,0.00),   "mass":1.3},
		{"name":"left_hand",       "kind":"box",     "size":Vector3(0.10,0.16,0.07),  "pos":Vector3(-0.39,0.84,-0.01), "mass":0.55},
		{"name":"right_hand",      "kind":"box",     "size":Vector3(0.10,0.16,0.07),  "pos":Vector3(0.39,0.84,-0.01),  "mass":0.55},
		{"name":"left_thigh",      "kind":"capsule", "size":Vector3(0.085,0.40,0.00), "pos":Vector3(-0.105,0.73,0.00), "mass":7.2},
		{"name":"right_thigh",     "kind":"capsule", "size":Vector3(0.085,0.40,0.00), "pos":Vector3(0.105,0.73,0.00),  "mass":7.2},
		{"name":"left_shin",       "kind":"capsule", "size":Vector3(0.065,0.38,0.00), "pos":Vector3(-0.105,0.36,0.01), "mass":3.7},
		{"name":"right_shin",      "kind":"capsule", "size":Vector3(0.065,0.38,0.00), "pos":Vector3(0.105,0.36,0.01),  "mass":3.7},
		{"name":"left_foot",       "kind":"box",     "size":Vector3(0.13,0.09,0.27),  "pos":Vector3(-0.105,0.105,-0.075),"mass":1.1},
		{"name":"right_foot",      "kind":"box",     "size":Vector3(0.13,0.09,0.27),  "pos":Vector3(0.105,0.105,-0.075), "mass":1.1},
	]


func _make_body(spec: Dictionary) -> void:
	var body := RigidBody3D.new()
	body.name = String(spec["name"])
	body.mass = float(spec["mass"])
	body.position = Vector3(spec["pos"]) + SPAWN_OFFSET
	body.can_sleep = false
	body.continuous_cd = true
	body.linear_damp = 0.04
	body.angular_damp = 0.08
	body.freeze = true
	add_child(body)

	var physics_material := PhysicsMaterial.new()
	physics_material.friction = 0.75
	physics_material.bounce = 0.01
	body.physics_material_override = physics_material

	var collision := CollisionShape3D.new()
	var visual := MeshInstance3D.new()
	var size: Vector3 = spec["size"]
	var kind := String(spec["kind"])

	if kind == "box":
		var box_shape := BoxShape3D.new()
		box_shape.size = size
		collision.shape = box_shape
		var box_mesh := BoxMesh.new()
		box_mesh.size = size
		visual.mesh = box_mesh
	elif kind == "capsule":
		var capsule_shape := CapsuleShape3D.new()
		capsule_shape.radius = size.x
		capsule_shape.height = size.y
		collision.shape = capsule_shape
		var capsule_mesh := CapsuleMesh.new()
		capsule_mesh.radius = size.x
		capsule_mesh.height = size.y
		visual.mesh = capsule_mesh
	elif kind == "sphere":
		var sphere_shape := SphereShape3D.new()
		sphere_shape.radius = size.x
		collision.shape = sphere_shape
		var sphere_mesh := SphereMesh.new()
		sphere_mesh.radius = size.x
		sphere_mesh.height = size.x * 2.0
		visual.mesh = sphere_mesh
	else:
		push_error("Unknown ragdoll body shape: %s" % kind)
		body.queue_free()
		return

	body.add_child(collision)
	body.add_child(visual)

	var material := StandardMaterial3D.new()
	material.albedo_color = _body_color(String(spec["name"]))
	material.metallic = 0.08
	material.roughness = 0.48
	visual.material_override = material

	_bodies[String(spec["name"])] = body
	_initial_positions[String(spec["name"])] = Vector3(spec["pos"])


func _body_color(body_name: String) -> Color:
	if body_name.begins_with("left_"):
		return Color(0.92, 0.42, 0.24)
	if body_name.begins_with("right_"):
		return Color(0.22, 0.58, 0.94)
	return Color(0.78, 0.80, 0.84)


func _joint_specs() -> Array:
	return [
		_joint("pelvis_spine", "pelvis", "spine", Vector3(0,1.11,0), Vector3(-28,-24,-20), Vector3(35,24,20)),
		_joint("spine_chest", "spine", "chest", Vector3(0,1.35,0), Vector3(-22,-22,-18), Vector3(28,22,18)),
		_joint("chest_neck", "chest", "neck", Vector3(0,1.625,0), Vector3(-28,-35,-25), Vector3(32,35,25)),
		_joint("neck_head", "neck", "head", Vector3(0,1.77,0), Vector3(-35,-50,-30), Vector3(42,50,30)),
		_joint("chest_left_clavicle", "chest", "left_clavicle", Vector3(-0.145,1.58,0), Vector3(-25,-25,-35), Vector3(25,25,35)),
		_joint("chest_right_clavicle", "chest", "right_clavicle", Vector3(0.145,1.58,0), Vector3(-25,-25,-35), Vector3(25,25,35)),
		_joint("left_shoulder", "left_clavicle", "left_upper_arm", Vector3(-0.34,1.55,0), Vector3(-105,-70,-95), Vector3(105,70,95)),
		_joint("right_shoulder", "right_clavicle", "right_upper_arm", Vector3(0.34,1.55,0), Vector3(-105,-70,-95), Vector3(105,70,95)),
		_joint("left_elbow", "left_upper_arm", "left_forearm", Vector3(-0.39,1.225,0), Vector3(-145,-7,-7), Vector3(8,7,7)),
		_joint("right_elbow", "right_upper_arm", "right_forearm", Vector3(0.39,1.225,0), Vector3(-145,-7,-7), Vector3(8,7,7)),
		_joint("left_wrist", "left_forearm", "left_hand", Vector3(-0.39,0.925,-0.005), Vector3(-45,-25,-35), Vector3(45,25,35)),
		_joint("right_wrist", "right_forearm", "right_hand", Vector3(0.39,0.925,-0.005), Vector3(-45,-25,-35), Vector3(45,25,35)),
		_joint("left_hip", "pelvis", "left_thigh", Vector3(-0.105,0.90,0), Vector3(-105,-35,-40), Vector3(65,35,40)),
		_joint("right_hip", "pelvis", "right_thigh", Vector3(0.105,0.90,0), Vector3(-105,-35,-40), Vector3(65,35,40)),
		_joint("left_knee", "left_thigh", "left_shin", Vector3(-0.105,0.535,0.005), Vector3(-145,-6,-6), Vector3(6,6,6)),
		_joint("right_knee", "right_thigh", "right_shin", Vector3(0.105,0.535,0.005), Vector3(-145,-6,-6), Vector3(6,6,6)),
		_joint("left_ankle", "left_shin", "left_foot", Vector3(-0.105,0.175,-0.025), Vector3(-38,-16,-18), Vector3(32,16,18)),
		_joint("right_ankle", "right_shin", "right_foot", Vector3(0.105,0.175,-0.025), Vector3(-38,-16,-18), Vector3(32,16,18)),
	]


func _joint(name: String, parent_name: String, child_name: String, anchor: Vector3, lower_deg: Vector3, upper_deg: Vector3) -> Dictionary:
	return {
		"name": name,
		"parent": parent_name,
		"child": child_name,
		"anchor": anchor,
		"lower": lower_deg,
		"upper": upper_deg,
	}


func _make_joint(spec: Dictionary) -> void:
	var parent_body: RigidBody3D = _bodies[String(spec["parent"])]
	var child_body: RigidBody3D = _bodies[String(spec["child"])]
	var joint := Generic6DOFJoint3D.new()
	joint.name = String(spec["name"])
	joint.position = Vector3(spec["anchor"]) + SPAWN_OFFSET
	add_child(joint)
	joint.node_a = joint.get_path_to(parent_body)
	joint.node_b = joint.get_path_to(child_body)
	joint.exclude_nodes_from_collision = true
	joint.solver_priority = 2

	_lock_linear_axes(joint)
	var lower: Vector3 = spec["lower"]
	var upper: Vector3 = spec["upper"]
	_set_angular_limit(joint, "x", lower.x, upper.x)
	_set_angular_limit(joint, "y", lower.y, upper.y)
	_set_angular_limit(joint, "z", lower.z, upper.z)
	_joint_count += 1


func _lock_linear_axes(joint: Generic6DOFJoint3D) -> void:
	# Use the documented slash-named properties through Object.set(). This avoids
	# relying on editor-only inspector name flattening.
	for axis in ["x", "y", "z"]:
		joint.set("linear_limit_%s/enabled" % axis, true)
		joint.set("linear_limit_%s/lower_distance" % axis, 0.0)
		joint.set("linear_limit_%s/upper_distance" % axis, 0.0)


func _set_angular_limit(joint: Generic6DOFJoint3D, axis: String, lower_deg: float, upper_deg: float) -> void:
	joint.set("angular_limit_%s/enabled" % axis, true)
	joint.set("angular_limit_%s/lower_angle" % axis, deg_to_rad(lower_deg))
	joint.set("angular_limit_%s/upper_angle" % axis, deg_to_rad(upper_deg))


func _assert_articulation_contract() -> void:
	assert(_bodies.size() == EXPECTED_BODY_COUNT, "Ragdoll must contain exactly 19 rigid bodies")
	assert(_joint_count == EXPECTED_JOINT_COUNT, "19-body tree must contain exactly 18 joints")
	var total_mass := 0.0
	for body_name in _bodies:
		var body: RigidBody3D = _bodies[body_name]
		total_mass += body.mass
	print("Asterra passive ragdoll ready: %d bodies, %d joints, %.2f kg total" % [_bodies.size(), _joint_count, total_mass])
	print("3D physics engine setting: %s" % String(ProjectSettings.get_setting("physics/3d/physics_engine", "default")))


func _reset_ragdoll() -> void:
	for body_name in _bodies:
		var body: RigidBody3D = _bodies[body_name]
		body.freeze = true
		body.global_position = Vector3(_initial_positions[body_name]) + SPAWN_OFFSET
		body.global_rotation = Vector3.ZERO
		body.linear_velocity = Vector3.ZERO
		body.angular_velocity = Vector3.ZERO

	for body_name in _bodies:
		var body: RigidBody3D = _bodies[body_name]
		body.freeze = false
		body.sleeping = false

	# A tiny asymmetric angular kick makes constraint freedom obvious while staying
	# small enough that any explosion is a real articulation problem.
	var pelvis: RigidBody3D = _bodies["pelvis"]
	pelvis.angular_velocity = Vector3(0.15, 0.05, 0.28)
