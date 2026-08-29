extends Node3D
## Isolated passive 19-body ragdoll validation scene.
##
## No motors, animation, balance controller, terrain system, or neural policy.
## The ragdoll uses joint-local anatomical frames, optional anatomical ROM,
## swing/twist coupling, and a physical mouse spring grabber.

const SPAWN_OFFSET: Vector3 = Vector3(0.0, 0.55, 0.0)
const FLOOR_SIZE: Vector3 = Vector3(12.0, 0.20, 12.0)
const EXPECTED_BODY_COUNT: int = 19
const EXPECTED_JOINT_COUNT: int = 18

const RAY_LENGTH: float = 100.0
const MIN_GRAB_DISTANCE: float = 0.45
const MAX_GRAB_DISTANCE: float = 12.0
const GRAB_DEPTH_STEP: float = 0.18
const GRAB_STIFFNESS_PER_KG: float = 175.0
const GRAB_DAMPING_PER_KG: float = 27.0
const GRAB_MAX_FORCE_PER_KG: float = 320.0

var _bodies: Dictionary = {}
var _initial_positions: Dictionary = {}
var _joints: Dictionary = {}
var _joint_specs_by_name: Dictionary = {}
var _joint_frame_a_local: Dictionary = {}
var _joint_frame_b_local: Dictionary = {}
var _joint_count: int = 0

var _camera: Camera3D
var _camera_target: Vector3 = Vector3(0.0, 1.05, 0.0)
var _camera_yaw: float = 0.55
var _camera_pitch: float = -0.08
var _camera_distance: float = 4.2
var _orbit_dragging: bool = false

var _overlay_label: Label
var _anatomical_limits_enabled: bool = true
var _axial_twist_enabled: bool = true

var _mouse_position: Vector2 = Vector2.ZERO
var _grabbed_body: RigidBody3D
var _grabbed_role: String = ""
var _grab_local_point: Vector3 = Vector3.ZERO
var _grab_distance: float = 2.0
var _grab_target_world: Vector3 = Vector3.ZERO


func _ready() -> void:
	_make_environment()
	_make_floor()
	_make_camera()
	_make_overlay()
	_build_ragdoll()
	_assert_articulation_contract()
	_mouse_position = get_viewport().get_visible_rect().size * 0.5
	_reset_ragdoll()
	_update_overlay()


func _physics_process(_delta: float) -> void:
	if _anatomical_limits_enabled:
		_apply_coupled_limits()
	_update_physics_grab()


func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventMouse:
		var mouse_event: InputEventMouse = event as InputEventMouse
		_mouse_position = mouse_event.position

	if event is InputEventKey:
		var key_event: InputEventKey = event as InputEventKey
		if key_event.pressed and not key_event.echo:
			if key_event.keycode == KEY_R:
				_release_grab()
				_reset_ragdoll()
			elif key_event.keycode == KEY_L:
				_set_anatomical_limits_enabled(not _anatomical_limits_enabled)
			elif key_event.keycode == KEY_T:
				_set_axial_twist_enabled(not _axial_twist_enabled)
			elif key_event.keycode == KEY_ESCAPE:
				_orbit_dragging = false
				_release_grab()

	if event is InputEventMouseButton:
		var button_event: InputEventMouseButton = event as InputEventMouseButton
		if button_event.button_index == MOUSE_BUTTON_LEFT:
			if button_event.pressed:
				_begin_grab(button_event.position)
			else:
				_release_grab()
		elif button_event.button_index == MOUSE_BUTTON_RIGHT:
			_orbit_dragging = button_event.pressed
		elif button_event.pressed and button_event.button_index == MOUSE_BUTTON_WHEEL_UP:
			if _grabbed_body != null:
				_grab_distance = clampf(
					_grab_distance - GRAB_DEPTH_STEP,
					MIN_GRAB_DISTANCE,
					MAX_GRAB_DISTANCE
				)
			else:
				_camera_distance = maxf(1.8, _camera_distance - 0.30)
				_update_camera()
		elif button_event.pressed and button_event.button_index == MOUSE_BUTTON_WHEEL_DOWN:
			if _grabbed_body != null:
				_grab_distance = clampf(
					_grab_distance + GRAB_DEPTH_STEP,
					MIN_GRAB_DISTANCE,
					MAX_GRAB_DISTANCE
				)
			else:
				_camera_distance = minf(9.0, _camera_distance + 0.30)
				_update_camera()

	if event is InputEventMouseMotion and _orbit_dragging and _grabbed_body == null:
		var motion_event: InputEventMouseMotion = event as InputEventMouseMotion
		_camera_yaw -= motion_event.relative.x * 0.006
		_camera_pitch = clampf(
			_camera_pitch - motion_event.relative.y * 0.006,
			-1.15,
			0.95
		)
		_update_camera()


func _make_environment() -> void:
	var world_environment: WorldEnvironment = WorldEnvironment.new()
	world_environment.name = "WorldEnvironment"

	var environment: Environment = Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color(0.025, 0.03, 0.04)
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.ambient_light_color = Color(0.55, 0.58, 0.64)
	environment.ambient_light_energy = 0.55
	world_environment.environment = environment
	add_child(world_environment)

	var sun: DirectionalLight3D = DirectionalLight3D.new()
	sun.name = "Sun"
	sun.rotation_degrees = Vector3(-48.0, -28.0, 0.0)
	sun.light_energy = 1.4
	sun.shadow_enabled = true
	add_child(sun)


func _make_floor() -> void:
	var floor: StaticBody3D = StaticBody3D.new()
	floor.name = "RigidFloor"
	floor.position = Vector3(0.0, -0.10, 0.0)
	add_child(floor)

	var collision: CollisionShape3D = CollisionShape3D.new()
	var shape: BoxShape3D = BoxShape3D.new()
	shape.size = FLOOR_SIZE
	collision.shape = shape
	floor.add_child(collision)

	var visual: MeshInstance3D = MeshInstance3D.new()
	var mesh: BoxMesh = BoxMesh.new()
	mesh.size = FLOOR_SIZE
	visual.mesh = mesh

	var material: StandardMaterial3D = StandardMaterial3D.new()
	material.albedo_color = Color(0.14, 0.15, 0.17)
	material.roughness = 0.88
	visual.material_override = material
	floor.add_child(visual)

	var physics_material: PhysicsMaterial = PhysicsMaterial.new()
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

	var horizontal: float = cos(_camera_pitch) * _camera_distance
	var offset: Vector3 = Vector3(
		sin(_camera_yaw) * horizontal,
		sin(_camera_pitch) * _camera_distance,
		cos(_camera_yaw) * horizontal
	)
	_camera.global_position = _camera_target + offset
	_camera.look_at(_camera_target, Vector3.UP)


func _make_overlay() -> void:
	var canvas: CanvasLayer = CanvasLayer.new()
	canvas.name = "Overlay"
	add_child(canvas)

	_overlay_label = Label.new()
	_overlay_label.position = Vector2(18.0, 16.0)
	_overlay_label.add_theme_font_size_override("font_size", 16)
	canvas.add_child(_overlay_label)


func _update_overlay() -> void:
	if _overlay_label == null:
		return

	var limits_text: String = "ON" if _anatomical_limits_enabled else "OFF"
	var twist_text: String = "ON" if _axial_twist_enabled else "LOCKED"
	var grabbed_text: String = _grabbed_role if _grabbed_body != null else "none"

	_overlay_label.text = (
		"19-BODY PASSIVE ANATOMICAL RAGDOLL / JOLT / 120 Hz\n"
		+ "L: limits %s   T: axial twist %s   R: reset\n" % [limits_text, twist_text]
		+ "LMB: physical grab   wheel while grabbed: depth   RMB: orbit\n"
		+ "grabbed: %s   joint-local anatomical frames + swing/twist coupling" % grabbed_text
	)


func _build_ragdoll() -> void:
	var body_specs: Array = _body_specs()
	for spec_variant: Variant in body_specs:
		var spec: Dictionary = spec_variant as Dictionary
		_make_body(spec)

	# Keep self collision disabled during the joint-isolation stage.
	var names: Array = _bodies.keys()
	for i: int in range(names.size()):
		for j: int in range(i + 1, names.size()):
			var body_a: RigidBody3D = _bodies[names[i]]
			var body_b: RigidBody3D = _bodies[names[j]]
			body_a.add_collision_exception_with(body_b)
			body_b.add_collision_exception_with(body_a)

	var joint_specs: Array = _joint_specs()
	for joint_spec_variant: Variant in joint_specs:
		var joint_spec: Dictionary = joint_spec_variant as Dictionary
		_make_joint(joint_spec)


func _body_specs() -> Array:
	return [
		{"name":"pelvis",          "kind":"box",     "size":Vector3(0.30,0.20,0.20),  "pos":Vector3(0.00,1.00,0.00),     "mass":10.5},
		{"name":"spine",           "kind":"box",     "size":Vector3(0.27,0.24,0.18),  "pos":Vector3(0.00,1.22,0.00),     "mass":7.0},
		{"name":"chest",           "kind":"box",     "size":Vector3(0.42,0.28,0.20),  "pos":Vector3(0.00,1.48,0.00),     "mass":15.0},
		{"name":"neck",            "kind":"capsule", "size":Vector3(0.06,0.13,0.00),  "pos":Vector3(0.00,1.69,0.00),     "mass":1.0},
		{"name":"head",            "kind":"sphere",  "size":Vector3(0.12,0.00,0.00),  "pos":Vector3(0.00,1.88,0.00),     "mass":5.0},
		{"name":"left_clavicle",   "kind":"box",     "size":Vector3(0.18,0.07,0.08),  "pos":Vector3(-0.23,1.58,0.00),    "mass":0.7},
		{"name":"right_clavicle",  "kind":"box",     "size":Vector3(0.18,0.07,0.08),  "pos":Vector3(0.23,1.58,0.00),     "mass":0.7},
		{"name":"left_upper_arm",  "kind":"capsule", "size":Vector3(0.055,0.31,0.00), "pos":Vector3(-0.39,1.38,0.00),    "mass":2.2},
		{"name":"right_upper_arm", "kind":"capsule", "size":Vector3(0.055,0.31,0.00), "pos":Vector3(0.39,1.38,0.00),     "mass":2.2},
		{"name":"left_forearm",    "kind":"capsule", "size":Vector3(0.045,0.28,0.00), "pos":Vector3(-0.39,1.08,0.00),    "mass":1.3},
		{"name":"right_forearm",   "kind":"capsule", "size":Vector3(0.045,0.28,0.00), "pos":Vector3(0.39,1.08,0.00),     "mass":1.3},
		{"name":"left_hand",       "kind":"box",     "size":Vector3(0.10,0.16,0.07),  "pos":Vector3(-0.39,0.84,-0.01),   "mass":0.55},
		{"name":"right_hand",      "kind":"box",     "size":Vector3(0.10,0.16,0.07),  "pos":Vector3(0.39,0.84,-0.01),    "mass":0.55},
		{"name":"left_thigh",      "kind":"capsule", "size":Vector3(0.085,0.40,0.00), "pos":Vector3(-0.105,0.73,0.00),   "mass":7.2},
		{"name":"right_thigh",     "kind":"capsule", "size":Vector3(0.085,0.40,0.00), "pos":Vector3(0.105,0.73,0.00),    "mass":7.2},
		{"name":"left_shin",       "kind":"capsule", "size":Vector3(0.065,0.38,0.00), "pos":Vector3(-0.105,0.36,0.01),   "mass":3.7},
		{"name":"right_shin",      "kind":"capsule", "size":Vector3(0.065,0.38,0.00), "pos":Vector3(0.105,0.36,0.01),    "mass":3.7},
		{"name":"left_foot",       "kind":"box",     "size":Vector3(0.13,0.09,0.27),  "pos":Vector3(-0.105,0.105,-0.075),"mass":1.1},
		{"name":"right_foot",      "kind":"box",     "size":Vector3(0.13,0.09,0.27),  "pos":Vector3(0.105,0.105,-0.075), "mass":1.1},
	]


func _make_body(spec: Dictionary) -> void:
	var body: RigidBody3D = RigidBody3D.new()
	body.name = String(spec["name"])
	body.mass = float(spec["mass"])
	body.position = Vector3(spec["pos"]) + SPAWN_OFFSET
	body.can_sleep = false
	body.continuous_cd = true
	body.linear_damp = 0.04
	body.angular_damp = 0.08
	body.freeze = true
	add_child(body)

	var physics_material: PhysicsMaterial = PhysicsMaterial.new()
	physics_material.friction = 0.75
	physics_material.bounce = 0.01
	body.physics_material_override = physics_material

	var collision: CollisionShape3D = CollisionShape3D.new()
	var visual: MeshInstance3D = MeshInstance3D.new()
	var size: Vector3 = Vector3(spec["size"])
	var kind: String = String(spec["kind"])

	if kind == "box":
		var box_shape: BoxShape3D = BoxShape3D.new()
		box_shape.size = size
		collision.shape = box_shape

		var box_mesh: BoxMesh = BoxMesh.new()
		box_mesh.size = size
		visual.mesh = box_mesh
	elif kind == "capsule":
		var capsule_shape: CapsuleShape3D = CapsuleShape3D.new()
		capsule_shape.radius = size.x
		capsule_shape.height = size.y
		collision.shape = capsule_shape

		var capsule_mesh: CapsuleMesh = CapsuleMesh.new()
		capsule_mesh.radius = size.x
		capsule_mesh.height = size.y
		visual.mesh = capsule_mesh
	elif kind == "sphere":
		var sphere_shape: SphereShape3D = SphereShape3D.new()
		sphere_shape.radius = size.x
		collision.shape = sphere_shape

		var sphere_mesh: SphereMesh = SphereMesh.new()
		sphere_mesh.radius = size.x
		sphere_mesh.height = size.x * 2.0
		visual.mesh = sphere_mesh
	else:
		push_error("Unknown ragdoll body shape: %s" % kind)
		body.queue_free()
		return

	body.add_child(collision)
	body.add_child(visual)

	var material: StandardMaterial3D = StandardMaterial3D.new()
	material.albedo_color = _body_color(String(spec["name"]))
	material.metallic = 0.08
	material.roughness = 0.48
	visual.material_override = material

	var body_name: String = String(spec["name"])
	_bodies[body_name] = body
	_initial_positions[body_name] = Vector3(spec["pos"])


func _body_color(body_name: String) -> Color:
	if body_name.begins_with("left_"):
		return Color(0.92, 0.42, 0.24)
	if body_name.begins_with("right_"):
		return Color(0.22, 0.58, 0.94)
	return Color(0.78, 0.80, 0.84)


func _joint_specs() -> Array:
	# Joint-local convention:
	# X = flexion/extension
	# Y = long-axis twist
	# Z = lateral swing / abduction / varus-valgus
	return [
		_joint("pelvis_spine", "pelvis", "spine",
			Vector3(0,1.11,0), Vector3(-40,-15,-20), Vector3(25,15,20)),
		_joint("spine_chest", "spine", "chest",
			Vector3(0,1.35,0), Vector3(-30,-30,-22), Vector3(20,30,22)),
		_joint("chest_neck", "chest", "neck",
			Vector3(0,1.625,0), Vector3(-22,-32,-22), Vector3(18,32,22)),
		_joint("neck_head", "neck", "head",
			Vector3(0,1.77,0), Vector3(-35,-48,-28), Vector3(42,48,28)),

		# Clavicle shafts are horizontal, so their joint frames are rotated.
		_joint("chest_left_clavicle", "chest", "left_clavicle",
			Vector3(-0.145,1.58,0), Vector3(-12,-30,-25), Vector3(25,35,25),
			Vector3.FORWARD, Vector3.LEFT),
		_joint("chest_right_clavicle", "chest", "right_clavicle",
			Vector3(0.145,1.58,0), Vector3(-12,-30,-25), Vector3(25,35,25),
			Vector3.FORWARD, Vector3.RIGHT),

		_joint("left_shoulder", "left_clavicle", "left_upper_arm",
			Vector3(-0.34,1.55,0), Vector3(-45,-80,-135), Vector3(135,80,30)),
		_joint("right_shoulder", "right_clavicle", "right_upper_arm",
			Vector3(0.34,1.55,0), Vector3(-45,-80,-30), Vector3(135,80,135)),

		_joint("left_elbow", "left_upper_arm", "left_forearm",
			Vector3(-0.39,1.225,0), Vector3(-5,-85,-4), Vector3(150,85,4)),
		_joint("right_elbow", "right_upper_arm", "right_forearm",
			Vector3(0.39,1.225,0), Vector3(-5,-85,-4), Vector3(150,85,4)),

		_joint("left_wrist", "left_forearm", "left_hand",
			Vector3(-0.39,0.925,-0.005), Vector3(-75,-5,-20), Vector3(70,5,35)),
		_joint("right_wrist", "right_forearm", "right_hand",
			Vector3(0.39,0.925,-0.005), Vector3(-75,-5,-35), Vector3(70,5,20)),

		_joint("left_hip", "pelvis", "left_thigh",
			Vector3(-0.105,0.90,0), Vector3(-30,-40,-45), Vector3(125,50,30)),
		_joint("right_hip", "pelvis", "right_thigh",
			Vector3(0.105,0.90,0), Vector3(-30,-50,-30), Vector3(125,40,45)),

		_joint("left_knee", "left_thigh", "left_shin",
			Vector3(-0.105,0.535,0.005), Vector3(-145,-2,-2), Vector3(5,2,2)),
		_joint("right_knee", "right_thigh", "right_shin",
			Vector3(0.105,0.535,0.005), Vector3(-145,-2,-2), Vector3(5,2,2)),

		_joint("left_ankle", "left_shin", "left_foot",
			Vector3(-0.105,0.175,-0.025), Vector3(-50,-12,-18), Vector3(25,12,32)),
		_joint("right_ankle", "right_shin", "right_foot",
			Vector3(0.105,0.175,-0.025), Vector3(-50,-12,-32), Vector3(25,12,18)),
	]


func _joint(
	name: String,
	parent_name: String,
	child_name: String,
	anchor: Vector3,
	lower_deg: Vector3,
	upper_deg: Vector3,
	flex_axis_world: Vector3 = Vector3.RIGHT,
	twist_axis_world: Vector3 = Vector3.UP
) -> Dictionary:
	return {
		"name": name,
		"parent": parent_name,
		"child": child_name,
		"anchor": anchor,
		"lower": lower_deg,
		"upper": upper_deg,
		"flex_axis_world": flex_axis_world,
		"twist_axis_world": twist_axis_world,
	}


func _make_joint(spec: Dictionary) -> void:
	_validate_joint_spec(spec)

	var parent_body: RigidBody3D = _bodies[String(spec["parent"])]
	var child_body: RigidBody3D = _bodies[String(spec["child"])]
	var joint: Generic6DOFJoint3D = Generic6DOFJoint3D.new()
	var joint_name: String = String(spec["name"])
	joint.name = joint_name

	var frame_basis: Basis = _make_anatomical_basis(
		Vector3(spec["flex_axis_world"]),
		Vector3(spec["twist_axis_world"])
	)
	joint.transform = Transform3D(
		frame_basis,
		Vector3(spec["anchor"]) + SPAWN_OFFSET
	)
	add_child(joint)

	joint.node_a = joint.get_path_to(parent_body)
	joint.node_b = joint.get_path_to(child_body)
	joint.exclude_nodes_from_collision = true

	_lock_linear_axes(joint)

	var lower: Vector3 = Vector3(spec["lower"])
	var upper: Vector3 = Vector3(spec["upper"])
	_set_angular_limit(joint, "x", lower.x, upper.x)
	_set_angular_limit(joint, "y", lower.y, upper.y)
	_set_angular_limit(joint, "z", lower.z, upper.z)

	# Neutral anatomical frame stored relative to each rigid body.
	var joint_world_basis: Basis = joint.global_transform.basis.orthonormalized()
	var parent_inverse: Basis = parent_body.global_transform.basis.inverse()
	var child_inverse: Basis = child_body.global_transform.basis.inverse()
	_joint_frame_a_local[joint_name] = (
		parent_inverse * joint_world_basis
	).orthonormalized()
	_joint_frame_b_local[joint_name] = (
		child_inverse * joint_world_basis
	).orthonormalized()

	_joints[joint_name] = joint
	_joint_specs_by_name[joint_name] = spec
	_joint_count += 1


func _make_anatomical_basis(
	flex_axis_world: Vector3,
	twist_axis_world: Vector3
) -> Basis:
	var y_axis: Vector3 = twist_axis_world.normalized()
	var x_axis: Vector3 = (
		flex_axis_world - y_axis * flex_axis_world.dot(y_axis)
	)

	if x_axis.length_squared() < 0.000001:
		x_axis = Vector3.RIGHT
		if absf(y_axis.dot(x_axis)) > 0.92:
			x_axis = Vector3.FORWARD
		x_axis -= y_axis * x_axis.dot(y_axis)

	x_axis = x_axis.normalized()

	# Basis columns are local X/Y/Z. X cross Y gives +Z.
	var z_axis: Vector3 = x_axis.cross(y_axis).normalized()
	return Basis(x_axis, y_axis, z_axis).orthonormalized()


func _validate_joint_spec(spec: Dictionary) -> void:
	var lower: Vector3 = Vector3(spec["lower"])
	var upper: Vector3 = Vector3(spec["upper"])
	var name: String = String(spec["name"])
	var flex_axis: Vector3 = Vector3(spec["flex_axis_world"])
	var twist_axis: Vector3 = Vector3(spec["twist_axis_world"])

	assert(
		lower.x <= upper.x and lower.y <= upper.y and lower.z <= upper.z,
		"Invalid joint ROM: %s" % name
	)
	assert(
		lower.x >= -179.0 and lower.y >= -179.0 and lower.z >= -179.0,
		"Joint lower ROM exceeds supported envelope: %s" % name
	)
	assert(
		upper.x <= 179.0 and upper.y <= 179.0 and upper.z <= 179.0,
		"Joint upper ROM exceeds supported envelope: %s" % name
	)
	assert(flex_axis.length_squared() > 0.5, "Invalid flex axis: %s" % name)
	assert(twist_axis.length_squared() > 0.5, "Invalid twist axis: %s" % name)
	assert(
		absf(flex_axis.normalized().dot(twist_axis.normalized())) < 0.98,
		"Flex and twist axes are nearly parallel: %s" % name
	)


func _lock_linear_axes(joint: Generic6DOFJoint3D) -> void:
	var axes: Array[String] = ["x", "y", "z"]
	for axis: String in axes:
		joint.set("linear_limit_%s/enabled" % axis, true)
		joint.set("linear_limit_%s/lower_distance" % axis, 0.0)
		joint.set("linear_limit_%s/upper_distance" % axis, 0.0)


func _set_angular_limit(
	joint: Generic6DOFJoint3D,
	axis: String,
	lower_deg: float,
	upper_deg: float
) -> void:
	joint.set("angular_limit_%s/enabled" % axis, true)
	_set_angular_range(joint, axis, lower_deg, upper_deg)


func _set_angular_range(
	joint: Generic6DOFJoint3D,
	axis: String,
	lower_deg: float,
	upper_deg: float
) -> void:
	joint.set(
		"angular_limit_%s/lower_angle" % axis,
		deg_to_rad(lower_deg)
	)
	joint.set(
		"angular_limit_%s/upper_angle" % axis,
		deg_to_rad(upper_deg)
	)


func _set_joint_angular_limits_enabled(
	joint: Generic6DOFJoint3D,
	enabled: bool
) -> void:
	var axes: Array[String] = ["x", "y", "z"]
	for axis: String in axes:
		joint.set("angular_limit_%s/enabled" % axis, enabled)


func _relative_joint_quaternion(joint_name: String) -> Quaternion:
	if not _joint_specs_by_name.has(joint_name):
		return Quaternion(0.0, 0.0, 0.0, 1.0)

	var spec: Dictionary = _joint_specs_by_name[joint_name]
	var parent_body: RigidBody3D = _bodies[String(spec["parent"])]
	var child_body: RigidBody3D = _bodies[String(spec["child"])]
	var frame_a_local: Basis = _joint_frame_a_local[joint_name]
	var frame_b_local: Basis = _joint_frame_b_local[joint_name]

	var frame_a_world: Basis = (
		parent_body.global_transform.basis * frame_a_local
	).orthonormalized()
	var frame_b_world: Basis = (
		child_body.global_transform.basis * frame_b_local
	).orthonormalized()
	var relative: Basis = (
		frame_a_world.inverse() * frame_b_world
	).orthonormalized()

	return relative.get_rotation_quaternion().normalized()


func _joint_swing_twist(joint_name: String) -> Dictionary:
	# Local +Y is the anatomical long-axis / twist axis.
	var q: Quaternion = _relative_joint_quaternion(joint_name)
	var twist_axis: Vector3 = Vector3.UP
	var q_vector: Vector3 = Vector3(q.x, q.y, q.z)
	var projected: Vector3 = twist_axis * q_vector.dot(twist_axis)
	var norm_sq: float = projected.length_squared() + q.w * q.w

	var twist: Quaternion = Quaternion(0.0, 0.0, 0.0, 1.0)
	if norm_sq > 0.00000001:
		var inv_norm: float = 1.0 / sqrt(norm_sq)
		twist = Quaternion(
			projected.x * inv_norm,
			projected.y * inv_norm,
			projected.z * inv_norm,
			q.w * inv_norm
		)

	var swing: Quaternion = (q * twist.inverse()).normalized()
	var swing_direction: Vector3 = swing * Vector3.UP

	# Around local X: Y rotates toward +/-Z.
	var flex_deg: float = rad_to_deg(
		atan2(swing_direction.z, swing_direction.y)
	)

	# Around local Z: Y rotates toward -/+X.
	var lateral_denominator: float = sqrt(
		maxf(
			0.0000001,
			swing_direction.y * swing_direction.y
			+ swing_direction.z * swing_direction.z
		)
	)
	var lateral_deg: float = rad_to_deg(
		atan2(-swing_direction.x, lateral_denominator)
	)

	var twist_vector: Vector3 = Vector3(twist.x, twist.y, twist.z)
	var twist_deg: float = rad_to_deg(
		2.0 * atan2(twist_vector.dot(twist_axis), twist.w)
	)
	twist_deg = rad_to_deg(
		wrapf(deg_to_rad(twist_deg), -PI, PI)
	)

	return {
		"flex_deg": flex_deg,
		"lateral_deg": lateral_deg,
		"twist_deg": twist_deg,
	}


func _apply_coupled_limits() -> void:
	_update_knee_coupling("left_knee")
	_update_knee_coupling("right_knee")
	_update_hip_coupling("left_hip", -1.0)
	_update_hip_coupling("right_hip", 1.0)
	_update_shoulder_coupling("left_shoulder")
	_update_shoulder_coupling("right_shoulder")
	_update_ankle_coupling("left_ankle")
	_update_ankle_coupling("right_ankle")

	if not _axial_twist_enabled:
		_lock_all_axial_twist()


func _update_knee_coupling(joint_name: String) -> void:
	if not _joints.has(joint_name):
		return

	var joint: Generic6DOFJoint3D = _joints[joint_name]
	var motion: Dictionary = _joint_swing_twist(joint_name)
	var flexion_deg: float = clampf(
		-float(motion["flex_deg"]),
		0.0,
		145.0
	)
	var unlock: float = smoothstep(8.0, 95.0, flexion_deg)
	var axial_deg: float = lerpf(1.5, 10.0, unlock)
	var frontal_deg: float = lerpf(1.5, 4.0, unlock)

	_set_angular_range(joint, "y", -axial_deg, axial_deg)
	_set_angular_range(joint, "z", -frontal_deg, frontal_deg)


func _update_hip_coupling(joint_name: String, side: float) -> void:
	if not _joints.has(joint_name):
		return

	var joint: Generic6DOFJoint3D = _joints[joint_name]
	var motion: Dictionary = _joint_swing_twist(joint_name)
	var flexion_deg: float = clampf(
		float(motion["flex_deg"]),
		0.0,
		125.0
	)
	var deep: float = smoothstep(65.0, 120.0, flexion_deg)
	var internal_deg: float = lerpf(40.0, 20.0, deep)
	var external_deg: float = lerpf(50.0, 26.0, deep)
	var abduction_deg: float = lerpf(45.0, 25.0, deep)
	var adduction_deg: float = lerpf(30.0, 16.0, deep)

	if side < 0.0:
		_set_angular_range(joint, "y", -internal_deg, external_deg)
		_set_angular_range(joint, "z", -abduction_deg, adduction_deg)
	else:
		_set_angular_range(joint, "y", -external_deg, internal_deg)
		_set_angular_range(joint, "z", -adduction_deg, abduction_deg)


func _update_shoulder_coupling(joint_name: String) -> void:
	if not _joints.has(joint_name):
		return

	var joint: Generic6DOFJoint3D = _joints[joint_name]
	var motion: Dictionary = _joint_swing_twist(joint_name)
	var flexion_fraction: float = clampf(
		absf(float(motion["flex_deg"])) / 135.0,
		0.0,
		1.0
	)
	var abduction_fraction: float = clampf(
		absf(float(motion["lateral_deg"])) / 90.0,
		0.0,
		1.0
	)
	var elevation: float = maxf(flexion_fraction, abduction_fraction)
	var tighten: float = smoothstep(0.48, 1.0, elevation)
	var axial_deg: float = lerpf(80.0, 32.0, tighten)

	_set_angular_range(joint, "y", -axial_deg, axial_deg)


func _update_ankle_coupling(joint_name: String) -> void:
	if not _joints.has(joint_name):
		return

	var joint: Generic6DOFJoint3D = _joints[joint_name]
	var spec: Dictionary = _joint_specs_by_name[joint_name]
	var motion: Dictionary = _joint_swing_twist(joint_name)
	var sagittal_deg: float = float(motion["flex_deg"])
	var center_distance: float = absf((sagittal_deg + 12.5) / 37.5)
	var tighten: float = smoothstep(
		0.48,
		1.0,
		clampf(center_distance, 0.0, 1.0)
	)

	var lower: Vector3 = Vector3(spec["lower"])
	var upper: Vector3 = Vector3(spec["upper"])
	var y_scale: float = lerpf(1.0, 0.45, tighten)
	var z_scale: float = lerpf(1.0, 0.58, tighten)

	_set_angular_range(
		joint, "y", lower.y * y_scale, upper.y * y_scale
	)
	_set_angular_range(
		joint, "z", lower.z * z_scale, upper.z * z_scale
	)


func _restore_static_anatomical_ranges() -> void:
	for joint_name_variant: Variant in _joints.keys():
		var joint_name: String = String(joint_name_variant)
		var joint: Generic6DOFJoint3D = _joints[joint_name]
		var spec: Dictionary = _joint_specs_by_name[joint_name]
		var lower: Vector3 = Vector3(spec["lower"])
		var upper: Vector3 = Vector3(spec["upper"])

		_set_joint_angular_limits_enabled(joint, true)
		_set_angular_range(joint, "x", lower.x, upper.x)
		_set_angular_range(joint, "y", lower.y, upper.y)
		_set_angular_range(joint, "z", lower.z, upper.z)


func _lock_all_axial_twist() -> void:
	for joint_name_variant: Variant in _joints.keys():
		var joint_name: String = String(joint_name_variant)
		var joint: Generic6DOFJoint3D = _joints[joint_name]
		_set_angular_range(joint, "y", 0.0, 0.0)


func _set_anatomical_limits_enabled(enabled: bool) -> void:
	_anatomical_limits_enabled = enabled

	if enabled:
		_restore_static_anatomical_ranges()
		_apply_coupled_limits()
	else:
		for joint_name_variant: Variant in _joints.keys():
			var joint_name: String = String(joint_name_variant)
			var joint: Generic6DOFJoint3D = _joints[joint_name]
			_set_joint_angular_limits_enabled(joint, false)

	_update_overlay()


func _set_axial_twist_enabled(enabled: bool) -> void:
	_axial_twist_enabled = enabled

	if _anatomical_limits_enabled:
		_restore_static_anatomical_ranges()
		_apply_coupled_limits()

	_update_overlay()


func _begin_grab(screen_position: Vector2) -> void:
	if _camera == null:
		return

	var ray_from: Vector3 = _camera.project_ray_origin(screen_position)
	var ray_direction: Vector3 = (
		_camera.project_ray_normal(screen_position).normalized()
	)
	var query: PhysicsRayQueryParameters3D = (
		PhysicsRayQueryParameters3D.create(
			ray_from,
			ray_from + ray_direction * RAY_LENGTH
		)
	)
	query.collide_with_areas = false
	query.collide_with_bodies = true

	var hit: Dictionary = (
		get_world_3d().direct_space_state.intersect_ray(query)
	)
	if hit.is_empty():
		return

	# Dictionary.get() is Variant. Cast explicitly so warnings-as-errors cannot
	# reject inferred Variant typing here.
	var collider: RigidBody3D = hit.get("collider") as RigidBody3D
	if collider == null:
		return
	if not _bodies.values().has(collider):
		return

	_grabbed_body = collider
	_grabbed_role = String(_grabbed_body.name)

	var hit_position: Vector3 = Vector3(hit["position"])
	_grab_local_point = _grabbed_body.to_local(hit_position)
	_grab_distance = clampf(
		ray_from.distance_to(hit_position),
		MIN_GRAB_DISTANCE,
		MAX_GRAB_DISTANCE
	)
	_grabbed_body.sleeping = false
	_update_overlay()


func _release_grab() -> void:
	_grabbed_body = null
	_grabbed_role = ""
	_update_overlay()


func _update_physics_grab() -> void:
	if (
		_grabbed_body == null
		or not is_instance_valid(_grabbed_body)
		or _camera == null
	):
		return

	var ray_origin: Vector3 = _camera.project_ray_origin(_mouse_position)
	var ray_direction: Vector3 = (
		_camera.project_ray_normal(_mouse_position).normalized()
	)
	_grab_target_world = ray_origin + ray_direction * _grab_distance

	var world_point: Vector3 = _grabbed_body.to_global(_grab_local_point)
	var offset: Vector3 = world_point - _grabbed_body.global_position
	var point_velocity: Vector3 = (
		_grabbed_body.linear_velocity
		+ _grabbed_body.angular_velocity.cross(offset)
	)
	var error: Vector3 = _grab_target_world - world_point

	var stiffness: float = GRAB_STIFFNESS_PER_KG * _grabbed_body.mass
	var damping: float = GRAB_DAMPING_PER_KG * _grabbed_body.mass
	var force: Vector3 = error * stiffness - point_velocity * damping
	var max_force: float = GRAB_MAX_FORCE_PER_KG * _grabbed_body.mass

	if force.length() > max_force:
		force = force.normalized() * max_force

	_grabbed_body.apply_force(force, offset)
	_grabbed_body.sleeping = false


func _assert_articulation_contract() -> void:
	assert(
		_bodies.size() == EXPECTED_BODY_COUNT,
		"Ragdoll must contain exactly 19 rigid bodies"
	)
	assert(
		_joint_count == EXPECTED_JOINT_COUNT,
		"19-body tree must contain exactly 18 joints"
	)
	assert(
		_joints.size() == EXPECTED_JOINT_COUNT,
		"Every articulation joint must be registered"
	)
	assert(
		_joint_frame_a_local.size() == EXPECTED_JOINT_COUNT,
		"Every joint needs a parent anatomical frame"
	)
	assert(
		_joint_frame_b_local.size() == EXPECTED_JOINT_COUNT,
		"Every joint needs a child anatomical frame"
	)

	var total_mass: float = 0.0
	for body_name_variant: Variant in _bodies.keys():
		var body_name: String = String(body_name_variant)
		var body: RigidBody3D = _bodies[body_name]
		total_mass += body.mass

	print(
		"Asterra passive anatomical ragdoll ready: %d bodies, %d joints, %.2f kg total"
		% [_bodies.size(), _joint_count, total_mass]
	)
	print("Joint model: anatomical local frames + quaternion swing/twist coupled ROM")
	print("L toggles anatomical ROM; T toggles axial twist")
	print(
		"3D physics engine setting: %s"
		% String(ProjectSettings.get_setting("physics/3d/physics_engine", "default"))
	)


func _reset_ragdoll() -> void:
	for body_name_variant: Variant in _bodies.keys():
		var body_name: String = String(body_name_variant)
		var body: RigidBody3D = _bodies[body_name]
		body.freeze = true
		body.global_position = (
			Vector3(_initial_positions[body_name]) + SPAWN_OFFSET
		)
		body.global_rotation = Vector3.ZERO
		body.linear_velocity = Vector3.ZERO
		body.angular_velocity = Vector3.ZERO

	if _anatomical_limits_enabled:
		_restore_static_anatomical_ranges()
		_apply_coupled_limits()
	else:
		for joint_name_variant: Variant in _joints.keys():
			var joint_name: String = String(joint_name_variant)
			var joint: Generic6DOFJoint3D = _joints[joint_name]
			_set_joint_angular_limits_enabled(joint, false)

	for body_name_variant: Variant in _bodies.keys():
		var body_name: String = String(body_name_variant)
		var body: RigidBody3D = _bodies[body_name]
		body.freeze = false
		body.sleeping = false

	var pelvis: RigidBody3D = _bodies["pelvis"]
	pelvis.angular_velocity = Vector3(0.15, 0.05, 0.28)
	_update_overlay()
