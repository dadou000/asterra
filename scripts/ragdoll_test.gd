extends Node3D
## Isolated passive 19-body ragdoll validation scene.
##
## No active motors, animation authority, balance controller or neural policy.
## Joint behavior is passive: optional anatomical hard limits plus position-
## dependent ROM coupling. The mouse manipulator is also fully physical: it
## applies a damped spring at the exact grabbed point instead of teleporting.

const SPAWN_OFFSET := Vector3(0.0, 0.55, 0.0)
const FLOOR_SIZE := Vector3(12.0, 0.20, 12.0)
const EXPECTED_BODY_COUNT := 19
const EXPECTED_JOINT_COUNT := 18
const RAY_LENGTH := 100.0
const MIN_GRAB_DISTANCE := 0.45
const MAX_GRAB_DISTANCE := 12.0
const GRAB_DEPTH_STEP := 0.18
const GRAB_STIFFNESS_PER_KG := 175.0
const GRAB_DAMPING_PER_KG := 27.0
const GRAB_MAX_FORCE_PER_KG := 320.0

var _bodies: Dictionary = {}
var _initial_positions: Dictionary = {}
var _joints: Dictionary = {}
var _joint_specs_by_name: Dictionary = {}
var _joint_count := 0

var _camera: Camera3D
var _camera_target := Vector3(0.0, 1.05, 0.0)
var _camera_yaw := 0.55
var _camera_pitch := -0.08
var _camera_distance := 4.2
var _orbit_dragging := false

var _overlay_label: Label
var _anatomical_limits_enabled := true

var _mouse_position := Vector2.ZERO
var _grabbed_body: RigidBody3D
var _grabbed_role := ""
var _grab_local_point := Vector3.ZERO
var _grab_distance := 2.0
var _grab_target_world := Vector3.ZERO


func _ready() -> void:
	_make_environment()
	_make_floor()
	_make_camera()
	_make_overlay()
	_build_ragdoll()
	_assert_articulation_contract()
	_reset_ragdoll()
	_update_overlay()


func _physics_process(_delta: float) -> void:
	if _anatomical_limits_enabled:
		# Passive state-dependent envelopes. These are constraints, not motors.
		_update_knee_coupling("left_knee")
		_update_knee_coupling("right_knee")
		_update_hip_coupling("left_hip", -1.0)
		_update_hip_coupling("right_hip", 1.0)
		_update_shoulder_coupling("left_shoulder")
		_update_shoulder_coupling("right_shoulder")
		_update_ankle_coupling("left_ankle")
		_update_ankle_coupling("right_ankle")

	_update_physics_grab()


func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventMouse:
		_mouse_position = event.position

	if event is InputEventKey and event.pressed and not event.echo:
		if event.keycode == KEY_R:
			_release_grab()
			_reset_ragdoll()
		elif event.keycode == KEY_L:
			_set_anatomical_limits_enabled(not _anatomical_limits_enabled)
		elif event.keycode == KEY_ESCAPE:
			_orbit_dragging = false
			_release_grab()

	if event is InputEventMouseButton:
		if event.button_index == MOUSE_BUTTON_LEFT:
			if event.pressed:
				_begin_grab(event.position)
			else:
				_release_grab()
		elif event.button_index == MOUSE_BUTTON_RIGHT:
			_orbit_dragging = event.pressed
		elif event.pressed and event.button_index == MOUSE_BUTTON_WHEEL_UP:
			if _grabbed_body != null:
				_grab_distance = clampf(_grab_distance - GRAB_DEPTH_STEP, MIN_GRAB_DISTANCE, MAX_GRAB_DISTANCE)
			else:
				_camera_distance = maxf(1.8, _camera_distance - 0.30)
				_update_camera()
		elif event.pressed and event.button_index == MOUSE_BUTTON_WHEEL_DOWN:
			if _grabbed_body != null:
				_grab_distance = clampf(_grab_distance + GRAB_DEPTH_STEP, MIN_GRAB_DISTANCE, MAX_GRAB_DISTANCE)
			else:
				_camera_distance = minf(9.0, _camera_distance + 0.30)
				_update_camera()

	if event is InputEventMouseMotion and _orbit_dragging and _grabbed_body == null:
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

	_overlay_label = Label.new()
	_overlay_label.position = Vector2(18.0, 16.0)
	_overlay_label.add_theme_font_size_override("font_size", 16)
	canvas.add_child(_overlay_label)


func _update_overlay() -> void:
	if _overlay_label == null:
		return
	var limit_state := "ON" if _anatomical_limits_enabled else "OFF"
	var grab_state := "none"
	if _grabbed_body != null:
		grab_state = "%s  depth %.2f m" % [_grabbed_role, _grab_distance]
	_overlay_label.text = (
		"19-BODY PASSIVE RAGDOLL / JOLT / 120 Hz\n"
		+ "L: anatomical limits [%s]   R: reset\n" % limit_state
		+ "LMB drag body: physical grab   wheel while grabbed: depth\n"
		+ "RMB drag: orbit   wheel: zoom   ESC: release\n"
		+ "Grab: %s\n" % grab_state
		+ "No motors • no animation • no neural policy"
	)


func _build_ragdoll() -> void:
	for spec in _body_specs():
		_make_body(spec)

	# Keep self-collision disabled while validating joint behavior. This isolates
	# articulation constraints from body-body collision tuning.
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
	body.set_meta("ragdoll_role", String(spec["name"]))
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
	# Axis convention for this neutral mockup:
	# X = sagittal flexion/extension, Y = axial twist, Z = lateral/ab-adduction.
	return [
		_joint("pelvis_spine", "pelvis", "spine", Vector3(0,1.11,0), Vector3(-40,-15,-20), Vector3(25,15,20)),
		_joint("spine_chest", "spine", "chest", Vector3(0,1.35,0), Vector3(-30,-30,-22), Vector3(20,30,22)),
		_joint("chest_neck", "chest", "neck", Vector3(0,1.625,0), Vector3(-22,-32,-22), Vector3(18,32,22)),
		_joint("neck_head", "neck", "head", Vector3(0,1.77,0), Vector3(-35,-48,-28), Vector3(42,48,28)),
		_joint("chest_left_clavicle", "chest", "left_clavicle", Vector3(-0.145,1.58,0), Vector3(-25,-20,-18), Vector3(25,20,12)),
		_joint("chest_right_clavicle", "chest", "right_clavicle", Vector3(0.145,1.58,0), Vector3(-25,-20,-12), Vector3(25,20,18)),
		_joint("left_shoulder", "left_clavicle", "left_upper_arm", Vector3(-0.34,1.55,0), Vector3(-45,-80,-135), Vector3(135,80,30)),
		_joint("right_shoulder", "right_clavicle", "right_upper_arm", Vector3(0.34,1.55,0), Vector3(-45,-80,-30), Vector3(135,80,135)),
		_joint("left_elbow", "left_upper_arm", "left_forearm", Vector3(-0.39,1.225,0), Vector3(-5,-85,-4), Vector3(150,85,4)),
		_joint("right_elbow", "right_upper_arm", "right_forearm", Vector3(0.39,1.225,0), Vector3(-5,-85,-4), Vector3(150,85,4)),
		_joint("left_wrist", "left_forearm", "left_hand", Vector3(-0.39,0.925,-0.005), Vector3(-75,-5,-20), Vector3(70,5,35)),
		_joint("right_wrist", "right_forearm", "right_hand", Vector3(0.39,0.925,-0.005), Vector3(-75,-5,-35), Vector3(70,5,20)),
		_joint("left_hip", "pelvis", "left_thigh", Vector3(-0.105,0.90,0), Vector3(-30,-40,-45), Vector3(125,50,30)),
		_joint("right_hip", "pelvis", "right_thigh", Vector3(0.105,0.90,0), Vector3(-30,-50,-30), Vector3(125,40,45)),
		_joint("left_knee", "left_thigh", "left_shin", Vector3(-0.105,0.535,0.005), Vector3(-145,-2,-2), Vector3(5,2,2)),
		_joint("right_knee", "right_thigh", "right_shin", Vector3(0.105,0.535,0.005), Vector3(-145,-2,-2), Vector3(5,2,2)),
		_joint("left_ankle", "left_shin", "left_foot", Vector3(-0.105,0.175,-0.025), Vector3(-50,-12,-18), Vector3(25,12,32)),
		_joint("right_ankle", "right_shin", "right_foot", Vector3(0.105,0.175,-0.025), Vector3(-50,-12,-32), Vector3(25,12,18)),
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
	_validate_joint_spec(spec)
	var parent_body: RigidBody3D = _bodies[String(spec["parent"])]
	var child_body: RigidBody3D = _bodies[String(spec["child"])]
	var joint := Generic6DOFJoint3D.new()
	joint.name = String(spec["name"])
	joint.position = Vector3(spec["anchor"]) + SPAWN_OFFSET
	add_child(joint)
	joint.node_a = joint.get_path_to(parent_body)
	joint.node_b = joint.get_path_to(child_body)
	joint.exclude_nodes_from_collision = true

	_lock_linear_axes(joint)
	var lower: Vector3 = spec["lower"]
	var upper: Vector3 = spec["upper"]
	_set_angular_limit(joint, "x", lower.x, upper.x)
	_set_angular_limit(joint, "y", lower.y, upper.y)
	_set_angular_limit(joint, "z", lower.z, upper.z)

	var joint_name := String(spec["name"])
	_joints[joint_name] = joint
	_joint_specs_by_name[joint_name] = spec
	_joint_count += 1


func _validate_joint_spec(spec: Dictionary) -> void:
	var lower: Vector3 = spec["lower"]
	var upper: Vector3 = spec["upper"]
	assert(lower.x <= upper.x and lower.y <= upper.y and lower.z <= upper.z, "Invalid joint ROM: %s" % String(spec["name"]))
	assert(lower.x >= -179.0 and lower.y >= -179.0 and lower.z >= -179.0, "Joint lower ROM exceeds supported anatomical envelope: %s" % String(spec["name"]))
	assert(upper.x <= 179.0 and upper.y <= 179.0 and upper.z <= 179.0, "Joint upper ROM exceeds supported anatomical envelope: %s" % String(spec["name"]))


func _lock_linear_axes(joint: Generic6DOFJoint3D) -> void:
	for axis in ["x", "y", "z"]:
		joint.set("linear_limit_%s/enabled" % axis, true)
		joint.set("linear_limit_%s/lower_distance" % axis, 0.0)
		joint.set("linear_limit_%s/upper_distance" % axis, 0.0)


func _set_angular_limit(joint: Generic6DOFJoint3D, axis: String, lower_deg: float, upper_deg: float) -> void:
	joint.set("angular_limit_%s/enabled" % axis, true)
	_set_angular_range(joint, axis, lower_deg, upper_deg)


func _set_angular_range(joint: Generic6DOFJoint3D, axis: String, lower_deg: float, upper_deg: float) -> void:
	joint.set("angular_limit_%s/lower_angle" % axis, deg_to_rad(lower_deg))
	joint.set("angular_limit_%s/upper_angle" % axis, deg_to_rad(upper_deg))


func _restore_outer_joint_ranges() -> void:
	for joint_name in _joints:
		var joint: Generic6DOFJoint3D = _joints[joint_name]
		var spec: Dictionary = _joint_specs_by_name[joint_name]
		var lower: Vector3 = spec["lower"]
		var upper: Vector3 = spec["upper"]
		_set_angular_range(joint, "x", lower.x, upper.x)
		_set_angular_range(joint, "y", lower.y, upper.y)
		_set_angular_range(joint, "z", lower.z, upper.z)


func _set_anatomical_limits_enabled(enabled: bool) -> void:
	_anatomical_limits_enabled = enabled
	if enabled:
		_restore_outer_joint_ranges()
	for joint_name in _joints:
		var joint: Generic6DOFJoint3D = _joints[joint_name]
		for axis in ["x", "y", "z"]:
			joint.set("angular_limit_%s/enabled" % axis, enabled)
	_update_overlay()
	print("Anatomical angular limits: %s" % ("ON" if enabled else "OFF"))


func _relative_joint_euler(joint_name: String) -> Vector3:
	if not _joint_specs_by_name.has(joint_name):
		return Vector3.ZERO
	var spec: Dictionary = _joint_specs_by_name[joint_name]
	var parent_body: RigidBody3D = _bodies[String(spec["parent"])]
	var child_body: RigidBody3D = _bodies[String(spec["child"])]
	var relative_basis := parent_body.global_transform.basis.inverse() * child_body.global_transform.basis
	return relative_basis.get_euler(EULER_ORDER_XYZ)


func _update_knee_coupling(joint_name: String) -> void:
	if not _joints.has(joint_name):
		return
	var joint: Generic6DOFJoint3D = _joints[joint_name]
	var euler := _relative_joint_euler(joint_name)
	var flexion_deg := clampf(-rad_to_deg(euler.x), 0.0, 145.0)
	var unlock := smoothstep(8.0, 95.0, flexion_deg)
	var axial_deg := lerpf(2.0, 12.0, unlock)
	var frontal_deg := lerpf(2.0, 5.0, unlock)
	_set_angular_range(joint, "y", -axial_deg, axial_deg)
	_set_angular_range(joint, "z", -frontal_deg, frontal_deg)


func _update_hip_coupling(joint_name: String, side: float) -> void:
	if not _joints.has(joint_name):
		return
	var joint: Generic6DOFJoint3D = _joints[joint_name]
	var euler := _relative_joint_euler(joint_name)
	var flexion_deg := clampf(rad_to_deg(euler.x), 0.0, 125.0)
	var deep := smoothstep(70.0, 120.0, flexion_deg)
	var internal_deg := lerpf(40.0, 24.0, deep)
	var external_deg := lerpf(50.0, 28.0, deep)
	var abduction_deg := lerpf(45.0, 27.0, deep)
	var adduction_deg := lerpf(30.0, 18.0, deep)

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
	var euler := _relative_joint_euler(joint_name)
	var flexion_fraction := clampf(absf(rad_to_deg(euler.x)) / 135.0, 0.0, 1.0)
	var abduction_fraction := clampf(absf(rad_to_deg(euler.z)) / 135.0, 0.0, 1.0)
	var elevation := maxf(flexion_fraction, abduction_fraction)
	var tighten := smoothstep(0.62, 1.0, elevation)
	var axial_deg := lerpf(80.0, 48.0, tighten)
	_set_angular_range(joint, "y", -axial_deg, axial_deg)


func _update_ankle_coupling(joint_name: String) -> void:
	if not _joints.has(joint_name):
		return
	var joint: Generic6DOFJoint3D = _joints[joint_name]
	var spec: Dictionary = _joint_specs_by_name[joint_name]
	var euler := _relative_joint_euler(joint_name)
	var sagittal_deg := rad_to_deg(euler.x)
	var center_distance := absf((sagittal_deg + 12.5) / 37.5)
	var tighten := smoothstep(0.55, 1.0, clampf(center_distance, 0.0, 1.0))
	var lower: Vector3 = spec["lower"]
	var upper: Vector3 = spec["upper"]
	var y_scale := lerpf(1.0, 0.55, tighten)
	var z_scale := lerpf(1.0, 0.62, tighten)
	_set_angular_range(joint, "y", lower.y * y_scale, upper.y * y_scale)
	_set_angular_range(joint, "z", lower.z * z_scale, upper.z * z_scale)


func _begin_grab(screen_position: Vector2) -> void:
	if _camera == null:
		return
	var ray_origin := _camera.project_ray_origin(screen_position)
	var ray_direction := _camera.project_ray_normal(screen_position).normalized()
	var query := PhysicsRayQueryParameters3D.create(ray_origin, ray_origin + ray_direction * RAY_LENGTH)
	query.collide_with_areas = false
	query.collide_with_bodies = true
	var hit := get_world_3d().direct_space_state.intersect_ray(query)
	if hit.is_empty():
		return
	var collider: Object = hit.get("collider")
	if not (collider is RigidBody3D):
		return
	var body := collider as RigidBody3D
	if not body.has_meta("ragdoll_role"):
		return

	_grabbed_body = body
	_grabbed_role = String(body.get_meta("ragdoll_role"))
	var hit_position: Vector3 = hit.get("position")
	_grab_local_point = body.to_local(hit_position)
	_grab_distance = clampf(ray_origin.distance_to(hit_position), MIN_GRAB_DISTANCE, MAX_GRAB_DISTANCE)
	_grab_target_world = hit_position
	body.sleeping = false
	_update_overlay()


func _release_grab() -> void:
	_grabbed_body = null
	_grabbed_role = ""
	_grab_local_point = Vector3.ZERO
	_update_overlay()


func _update_physics_grab() -> void:
	if _grabbed_body == null or not is_instance_valid(_grabbed_body) or _camera == null:
		return

	var ray_origin := _camera.project_ray_origin(_mouse_position)
	var ray_direction := _camera.project_ray_normal(_mouse_position).normalized()
	_grab_target_world = ray_origin + ray_direction * _grab_distance

	var body := _grabbed_body
	body.sleeping = false
	var world_grab_point := body.global_transform * _grab_local_point
	var offset := world_grab_point - body.global_position
	var point_velocity := body.linear_velocity + body.angular_velocity.cross(offset)
	var error := _grab_target_world - world_grab_point

	# Mass-scaled spring gives all body parts roughly comparable acceleration.
	# Damping is close to critical for k/m ~= 175 s^-2 without feeling welded.
	var spring_force := error * (GRAB_STIFFNESS_PER_KG * body.mass)
	var damping_force := -point_velocity * (GRAB_DAMPING_PER_KG * body.mass)
	var force := spring_force + damping_force
	var max_force := GRAB_MAX_FORCE_PER_KG * body.mass
	if force.length() > max_force:
		force = force.normalized() * max_force
	body.apply_force(force, offset)


func _assert_articulation_contract() -> void:
	assert(_bodies.size() == EXPECTED_BODY_COUNT, "Ragdoll must contain exactly 19 rigid bodies")
	assert(_joint_count == EXPECTED_JOINT_COUNT, "19-body tree must contain exactly 18 joints")
	assert(_joints.size() == EXPECTED_JOINT_COUNT, "Every articulation joint must be registered")
	var total_mass := 0.0
	for body_name in _bodies:
		var body: RigidBody3D = _bodies[body_name]
		total_mass += body.mass
	print("Asterra passive anatomical ragdoll ready: %d bodies, %d joints, %.2f kg total" % [_bodies.size(), _joint_count, total_mass])
	print("L toggles anatomical angular limits; linear joint anchors remain constrained")
	print("LMB applies a physical spring at the clicked body point")
	print("3D physics engine setting: %s" % String(ProjectSettings.get_setting("physics/3d/physics_engine", "default")))


func _reset_ragdoll() -> void:
	_release_grab()
	for body_name in _bodies:
		var body: RigidBody3D = _bodies[body_name]
		body.freeze = true
		body.global_position = Vector3(_initial_positions[body_name]) + SPAWN_OFFSET
		body.global_rotation = Vector3.ZERO
		body.linear_velocity = Vector3.ZERO
		body.angular_velocity = Vector3.ZERO

	_restore_outer_joint_ranges()
	for joint_name in _joints:
		var joint: Generic6DOFJoint3D = _joints[joint_name]
		for axis in ["x", "y", "z"]:
			joint.set("angular_limit_%s/enabled" % axis, _anatomical_limits_enabled)

	for body_name in _bodies:
		var body: RigidBody3D = _bodies[body_name]
		body.freeze = false
		body.sleeping = false

	var pelvis: RigidBody3D = _bodies["pelvis"]
	pelvis.angular_velocity = Vector3(0.15, 0.05, 0.28)
	_update_overlay()
