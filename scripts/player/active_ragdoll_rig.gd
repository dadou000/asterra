class_name ActiveRagdollRig
extends Node3D
## Articulated active-ragdoll layer for PhysicsWalkerBody.
##
## The pelvis is the existing PhysicsWalkerBody. Torso/head/arms/legs are separate
## RigidBody3D segments linked by Generic6DOFJoint3D constraints. A procedural gait
## produces relative joint targets; finite PD torques drive the bodies toward those
## targets. The two physical feet provide compliant contact against the authoritative
## GPU terrain through TerrainContactSampler, so no CPU terrain collider is needed.

const GRAVITY_MPS2 := 9.62
const WALK_REFERENCE_SPEED := 4.6
const RUN_REFERENCE_SPEED := 11.0

@export var pelvis: PhysicsWalkerBody
@export var enabled := true

@export_group("Terrain contact")
@export var foot_contact_probe := 0.055
@export var foot_contact_stiffness := 22000.0
@export var foot_contact_damping := 1150.0
@export var foot_friction := 0.95
@export var max_foot_support_g := 3.2

@export_group("Motor response")
@export var gait_strength := 1.0
@export var torso_kp := 520.0
@export var torso_kd := 62.0
@export var hip_kp := 390.0
@export var hip_kd := 42.0
@export var knee_kp := 330.0
@export var knee_kd := 36.0
@export var ankle_kp := 190.0
@export var ankle_kd := 24.0
@export var shoulder_kp := 150.0
@export var shoulder_kd := 20.0
@export var elbow_kp := 105.0
@export var elbow_kd := 15.0

var active := false
var grounded := false
var total_support_force := 0.0
var support_normal := Vector3.UP
var min_foot_agl := INF
var left_support_force := 0.0
var right_support_force := 0.0

var _segments: Dictionary = {}
var _joints: Dictionary = {}
var _motors: Array[Dictionary] = []
var _local_positions: Dictionary = {}
var _gait_phase := 0.0
var _gait_blend := 0.0
var _jump_release_left := 0.0


func _ready() -> void:
	process_physics_priority = 1
	_build_rig()
	Frames.origin_shifted.connect(_on_origin_shifted)


func activate() -> void:
	if pelvis == null or not enabled:
		return
	active = true
	grounded = false
	_gait_phase = 0.0
	_gait_blend = 0.0
	_jump_release_left = 0.0
	var base := pelvis.global_transform
	for role in _segments.keys():
		var body := _segments[role] as RigidBody3D
		body.freeze = false
		body.sleeping = false
		body.linear_velocity = pelvis.linear_velocity
		body.angular_velocity = pelvis.angular_velocity
		body.global_transform = base * Transform3D(Basis.IDENTITY, _local_positions[role] as Vector3)
	_reset_joint_frames()


func deactivate() -> void:
	active = false
	grounded = false
	total_support_force = 0.0
	left_support_force = 0.0
	right_support_force = 0.0
	for body_value in _segments.values():
		var body := body_value as RigidBody3D
		body.linear_velocity = Vector3.ZERO
		body.angular_velocity = Vector3.ZERO
		body.freeze = true


func begin_jump_release(duration: float) -> void:
	_jump_release_left = maxf(_jump_release_left, duration)


func segment(role: String) -> RigidBody3D:
	return _segments.get(role) as RigidBody3D


func relative_rotation(role: String) -> Quaternion:
	if not _segments.has(role):
		return Quaternion.IDENTITY
	var child := _segments[role] as RigidBody3D
	var parent_body: RigidBody3D = pelvis
	for motor in _motors:
		if String(motor["role"]) == role:
			parent_body = motor["parent"] as RigidBody3D
			break
	if parent_body == null:
		return Quaternion.IDENTITY
	return (parent_body.global_transform.basis.inverse() * child.global_transform.basis).orthonormalized().get_rotation_quaternion()


func debug_state() -> Dictionary:
	return {
		"active": active,
		"grounded": grounded,
		"support_n": total_support_force,
		"left_support_n": left_support_force,
		"right_support_n": right_support_force,
		"foot_agl": min_foot_agl,
		"segments": _segments.size(),
	}


func apply_drive_force(force: Vector3) -> void:
	if not active or total_support_force <= 1e-4:
		return
	var left := _segments.get("left_foot") as RigidBody3D
	var right := _segments.get("right_foot") as RigidBody3D
	if left != null and left_support_force > 0.0:
		left.apply_central_force(force * (left_support_force / total_support_force))
	if right != null and right_support_force > 0.0:
		right.apply_central_force(force * (right_support_force / total_support_force))


func _physics_process(dt: float) -> void:
	if not active or pelvis == null or not pelvis.active or Planet.cfg == null or not Planet.ready_state:
		return
	_jump_release_left = maxf(0.0, _jump_release_left - dt)
	_apply_segment_gravity()
	_update_terrain_contacts()
	_update_gait_targets(dt)
	_apply_joint_motors()


func _build_rig() -> void:
	if pelvis == null:
		call_deferred("_build_rig")
		return
	if not _segments.is_empty():
		return

	# Positions are pelvis-local. The pelvis COM sits about 0.90 m above the sole.
	_local_positions = {
		"torso": Vector3(0.0, 0.27, 0.0),
		"head": Vector3(0.0, 0.67, -0.005),
		"left_upper_leg": Vector3(-0.13, -0.25, 0.0),
		"right_upper_leg": Vector3(0.13, -0.25, 0.0),
		"left_lower_leg": Vector3(-0.13, -0.62, 0.015),
		"right_lower_leg": Vector3(0.13, -0.62, 0.015),
		"left_foot": Vector3(-0.13, -0.85, -0.075),
		"right_foot": Vector3(0.13, -0.85, -0.075),
		"left_upper_arm": Vector3(-0.31, 0.28, 0.0),
		"right_upper_arm": Vector3(0.31, 0.28, 0.0),
		"left_lower_arm": Vector3(-0.31, 0.02, -0.005),
		"right_lower_arm": Vector3(0.31, 0.02, -0.005),
	}

	_segments["torso"] = _make_box_body("Torso", 18.0, Vector3(0.39, 0.43, 0.22))
	_segments["head"] = _make_sphere_body("Head", 5.0, 0.125)
	_segments["left_upper_leg"] = _make_capsule_body("LeftUpperLeg", 7.0, 0.085, 0.38)
	_segments["right_upper_leg"] = _make_capsule_body("RightUpperLeg", 7.0, 0.085, 0.38)
	_segments["left_lower_leg"] = _make_capsule_body("LeftLowerLeg", 4.0, 0.065, 0.35)
	_segments["right_lower_leg"] = _make_capsule_body("RightLowerLeg", 4.0, 0.065, 0.35)
	_segments["left_foot"] = _make_box_body("LeftFoot", 1.0, Vector3(0.14, 0.09, 0.26))
	_segments["right_foot"] = _make_box_body("RightFoot", 1.0, Vector3(0.14, 0.09, 0.26))
	_segments["left_upper_arm"] = _make_capsule_body("LeftUpperArm", 2.0, 0.055, 0.27)
	_segments["right_upper_arm"] = _make_capsule_body("RightUpperArm", 2.0, 0.055, 0.27)
	_segments["left_lower_arm"] = _make_capsule_body("LeftLowerArm", 1.25, 0.045, 0.24)
	_segments["right_lower_arm"] = _make_capsule_body("RightLowerArm", 1.25, 0.045, 0.24)

	for role in _segments.keys():
		var body := _segments[role] as RigidBody3D
		body.freeze = true
		body.gravity_scale = 0.0
		body.can_sleep = false
		body.linear_damp = 0.035
		body.angular_damp = 0.07
		add_child(body)

	_disable_internal_collisions()

	_add_motor("torso", pelvis, _segments["torso"], Vector3(0.0, 0.105, 0.0),
		Vector3(-24, -28, -20), Vector3(32, 28, 20), torso_kp, torso_kd, 520.0)
	_add_motor("head", _segments["torso"], _segments["head"], Vector3(0.0, 0.525, -0.005),
		Vector3(-35, -55, -28), Vector3(45, 55, 28), 120.0, 16.0, 75.0)
	_add_motor("left_upper_leg", pelvis, _segments["left_upper_leg"], Vector3(-0.13, -0.07, 0.0),
		Vector3(-75, -28, -32), Vector3(95, 28, 32), hip_kp, hip_kd, 240.0)
	_add_motor("right_upper_leg", pelvis, _segments["right_upper_leg"], Vector3(0.13, -0.07, 0.0),
		Vector3(-75, -28, -32), Vector3(95, 28, 32), hip_kp, hip_kd, 240.0)
	_add_motor("left_lower_leg", _segments["left_upper_leg"], _segments["left_lower_leg"], Vector3(-0.13, -0.445, 0.005),
		Vector3(-145, -6, -6), Vector3(5, 6, 6), knee_kp, knee_kd, 190.0)
	_add_motor("right_lower_leg", _segments["right_upper_leg"], _segments["right_lower_leg"], Vector3(0.13, -0.445, 0.005),
		Vector3(-145, -6, -6), Vector3(5, 6, 6), knee_kp, knee_kd, 190.0)
	_add_motor("left_foot", _segments["left_lower_leg"], _segments["left_foot"], Vector3(-0.13, -0.80, -0.025),
		Vector3(-38, -15, -16), Vector3(32, 15, 16), ankle_kp, ankle_kd, 95.0)
	_add_motor("right_foot", _segments["right_lower_leg"], _segments["right_foot"], Vector3(0.13, -0.80, -0.025),
		Vector3(-38, -15, -16), Vector3(32, 15, 16), ankle_kp, ankle_kd, 95.0)
	_add_motor("left_upper_arm", _segments["torso"], _segments["left_upper_arm"], Vector3(-0.255, 0.43, 0.0),
		Vector3(-100, -55, -75), Vector3(100, 55, 75), shoulder_kp, shoulder_kd, 85.0)
	_add_motor("right_upper_arm", _segments["torso"], _segments["right_upper_arm"], Vector3(0.255, 0.43, 0.0),
		Vector3(-100, -55, -75), Vector3(100, 55, 75), shoulder_kp, shoulder_kd, 85.0)
	_add_motor("left_lower_arm", _segments["left_upper_arm"], _segments["left_lower_arm"], Vector3(-0.31, 0.135, 0.0),
		Vector3(-145, -8, -8), Vector3(8, 8, 8), elbow_kp, elbow_kd, 55.0)
	_add_motor("right_lower_arm", _segments["right_upper_arm"], _segments["right_lower_arm"], Vector3(0.31, 0.135, 0.0),
		Vector3(-145, -8, -8), Vector3(8, 8, 8), elbow_kp, elbow_kd, 55.0)


func _make_box_body(name_value: String, mass_value: float, size: Vector3) -> RigidBody3D:
	var body := RigidBody3D.new()
	body.name = name_value
	body.mass = mass_value
	var collision := CollisionShape3D.new()
	var shape := BoxShape3D.new()
	shape.size = size
	collision.shape = shape
	body.add_child(collision)
	return body


func _make_capsule_body(name_value: String, mass_value: float, radius: float, height: float) -> RigidBody3D:
	var body := RigidBody3D.new()
	body.name = name_value
	body.mass = mass_value
	var collision := CollisionShape3D.new()
	var shape := CapsuleShape3D.new()
	shape.radius = radius
	shape.height = height
	collision.shape = shape
	body.add_child(collision)
	return body


func _make_sphere_body(name_value: String, mass_value: float, radius: float) -> RigidBody3D:
	var body := RigidBody3D.new()
	body.name = name_value
	body.mass = mass_value
	var collision := CollisionShape3D.new()
	var shape := SphereShape3D.new()
	shape.radius = radius
	collision.shape = shape
	body.add_child(collision)
	return body


func _disable_internal_collisions() -> void:
	var bodies: Array[RigidBody3D] = [pelvis]
	for value in _segments.values():
		bodies.append(value as RigidBody3D)
	for i in bodies.size():
		for j in range(i + 1, bodies.size()):
			bodies[i].add_collision_exception_with(bodies[j])
			bodies[j].add_collision_exception_with(bodies[i])


func _add_motor(
		role: String,
		parent_body: RigidBody3D,
		child_body: RigidBody3D,
		anchor_local: Vector3,
		lower_deg: Vector3,
		upper_deg: Vector3,
		kp: float,
		kd: float,
		max_torque: float
	) -> void:
	var joint := Generic6DOFJoint3D.new()
	joint.name = "%sJoint" % role.capitalize().replace(" ", "")
	joint.exclude_nodes_from_collision = true
	add_child(joint)
	joint.node_a = joint.get_path_to(parent_body)
	joint.node_b = joint.get_path_to(child_body)
	_set_joint_limits(joint, lower_deg, upper_deg)
	_joints[role] = joint
	_motors.append({
		"role": role,
		"parent": parent_body,
		"child": child_body,
		"anchor_local": anchor_local,
		"target": Quaternion.IDENTITY,
		"kp": kp,
		"kd": kd,
		"max_torque": max_torque,
	})


func _set_joint_limits(joint: Generic6DOFJoint3D, lower_deg: Vector3, upper_deg: Vector3) -> void:
	joint.set("linear_limit_x/enabled", true)
	joint.set("linear_limit_y/enabled", true)
	joint.set("linear_limit_z/enabled", true)
	joint.set("linear_limit_x/lower_distance", 0.0)
	joint.set("linear_limit_x/upper_distance", 0.0)
	joint.set("linear_limit_y/lower_distance", 0.0)
	joint.set("linear_limit_y/upper_distance", 0.0)
	joint.set("linear_limit_z/lower_distance", 0.0)
	joint.set("linear_limit_z/upper_distance", 0.0)
	joint.set("angular_limit_x/enabled", true)
	joint.set("angular_limit_y/enabled", true)
	joint.set("angular_limit_z/enabled", true)
	joint.set("angular_limit_x/lower_angle", deg_to_rad(lower_deg.x))
	joint.set("angular_limit_x/upper_angle", deg_to_rad(upper_deg.x))
	joint.set("angular_limit_y/lower_angle", deg_to_rad(lower_deg.y))
	joint.set("angular_limit_y/upper_angle", deg_to_rad(upper_deg.y))
	joint.set("angular_limit_z/lower_angle", deg_to_rad(lower_deg.z))
	joint.set("angular_limit_z/upper_angle", deg_to_rad(upper_deg.z))


func _reset_joint_frames() -> void:
	if pelvis == null:
		return
	for motor in _motors:
		var joint := _joints[String(motor["role"])] as Generic6DOFJoint3D
		var anchor := pelvis.global_transform * (motor["anchor_local"] as Vector3)
		joint.global_transform = Transform3D(pelvis.global_transform.basis, anchor)


func _apply_segment_gravity() -> void:
	var world := pelvis.world_position()
	if world.length_sq() <= 1.0:
		return
	var up := world.normalized().to_v3()
	for value in _segments.values():
		var body := value as RigidBody3D
		body.apply_central_force(-up * body.mass * GRAVITY_MPS2)


func _update_terrain_contacts() -> void:
	left_support_force = 0.0
	right_support_force = 0.0
	total_support_force = 0.0
	min_foot_agl = INF
	var weighted_normal := Vector3.ZERO
	if _jump_release_left > 0.0:
		grounded = false
		return

	var left := _sample_and_apply_foot("left_foot")
	var right := _sample_and_apply_foot("right_foot")
	left_support_force = float(left["support"])
	right_support_force = float(right["support"])
	total_support_force = left_support_force + right_support_force
	min_foot_agl = minf(float(left["agl"]), float(right["agl"]))
	if left_support_force > 0.0:
		weighted_normal += (left["normal"] as Vector3) * left_support_force
	if right_support_force > 0.0:
		weighted_normal += (right["normal"] as Vector3) * right_support_force
	var radial_up := pelvis.world_position().normalized().to_v3()
	support_normal = weighted_normal.normalized() if weighted_normal.length_squared() > 1e-8 else radial_up
	grounded = total_support_force > pelvis.body_mass * GRAVITY_MPS2 * 0.16


func _sample_and_apply_foot(role: String) -> Dictionary:
	var foot := _segments[role] as RigidBody3D
	var sole_offset := foot.global_transform.basis * Vector3(0.0, -0.047, -0.025)
	var sole_render := foot.global_position + sole_offset
	var sole_world := Frames.to_world(sole_render)
	var direction := sole_world.normalized().to_v3()
	TerrainContactSampler.request_surface(direction)
	TerrainContactSampler.request_contact_height(direction)
	var broad := TerrainContactSampler.height(direction)
	var surface := TerrainContactSampler.surface(direction)
	var height := float(surface.get("height", broad))
	var precise := TerrainContactSampler.contact_height(direction, height)
	if is_finite(precise):
		height = precise
	var normal := direction
	var normal_value: Variant = surface.get("normal", direction)
	if normal_value is Vector3:
		normal = (normal_value as Vector3).normalized()
	if normal.dot(direction) < 0.0:
		normal = -normal
	var agl := sole_world.length() - Planet.cfg.planet_radius - height
	var support := 0.0
	if agl <= foot_contact_probe:
		var point_velocity := foot.linear_velocity + foot.angular_velocity.cross(sole_offset)
		var normal_velocity := point_velocity.dot(normal)
		var proximity := 1.0 - clampf(maxf(agl, 0.0) / maxf(foot_contact_probe, 0.001), 0.0, 1.0)
		var feedforward := pelvis.body_mass * GRAVITY_MPS2 * 0.5 * proximity
		var spring := foot_contact_stiffness * maxf(-agl, 0.0)
		var damping := -foot_contact_damping * normal_velocity
		var maximum := pelvis.body_mass * GRAVITY_MPS2 * max_foot_support_g * 0.5
		support = clampf(feedforward + spring + damping, 0.0, maximum)
		if support > 0.0:
			foot.apply_force(normal * support, sole_offset)
			var tangent_velocity := point_velocity - normal * normal_velocity
			var friction_force := -tangent_velocity * 340.0
			friction_force = _clamp_length(friction_force, support * foot_friction)
			foot.apply_force(friction_force, sole_offset)
	return {"agl": agl, "normal": normal, "support": support}


func _update_gait_targets(dt: float) -> void:
	var up := pelvis.world_position().normalized().to_v3()
	var planar_velocity := pelvis.linear_velocity - up * pelvis.linear_velocity.dot(up)
	var speed := planar_velocity.length()
	var movement_blend := clampf(speed / 0.65, 0.0, 1.0)
	if not grounded:
		movement_blend *= 0.48
	_gait_blend = move_toward(_gait_blend, movement_blend, dt * 7.0)
	var speed_norm := clampf(speed / WALK_REFERENCE_SPEED, 0.0, 1.0)
	var run_norm := clampf((speed - WALK_REFERENCE_SPEED) / maxf(RUN_REFERENCE_SPEED - WALK_REFERENCE_SPEED, 0.1), 0.0, 1.0)
	if speed > 0.08:
		var cadence_hz := lerpf(1.0, 1.72, speed_norm) + run_norm * 0.48
		_gait_phase = fmod(_gait_phase + dt * TAU * cadence_hz, TAU)

	var s := sin(_gait_phase)
	var c := cos(_gait_phase)
	var left_swing := maxf(s, 0.0)
	var right_swing := maxf(-s, 0.0)
	var hip := deg_to_rad(lerpf(17.0, 33.0, speed_norm) + run_norm * 13.0) * s * _gait_blend * gait_strength
	var knee := deg_to_rad(lerpf(28.0, 50.0, speed_norm) + run_norm * 15.0) * _gait_blend * gait_strength
	var ankle := deg_to_rad(lerpf(8.0, 15.0, speed_norm)) * _gait_blend * gait_strength
	var arm := deg_to_rad(lerpf(12.0, 29.0, speed_norm) + run_norm * 11.0) * s * _gait_blend * gait_strength
	var elbow := deg_to_rad(8.0 + 18.0 * speed_norm + 15.0 * run_norm) * _gait_blend * gait_strength
	var torso_yaw := deg_to_rad(4.5 + 3.0 * run_norm) * s * _gait_blend * gait_strength
	var torso_side := deg_to_rad(1.6) * c * _gait_blend * gait_strength

	_set_target("torso", _q(Vector3.UP, -torso_yaw) * _q(Vector3.BACK, torso_side))
	_set_target("head", _q(Vector3.UP, torso_yaw * 0.45))
	_set_target("left_upper_leg", _q(Vector3.RIGHT, -hip))
	_set_target("right_upper_leg", _q(Vector3.RIGHT, hip))
	_set_target("left_lower_leg", _q(Vector3.RIGHT, -knee * left_swing - deg_to_rad(2.5)))
	_set_target("right_lower_leg", _q(Vector3.RIGHT, -knee * right_swing - deg_to_rad(2.5)))
	_set_target("left_foot", _q(Vector3.RIGHT, ankle * (s * 0.55 - left_swing * 0.65)))
	_set_target("right_foot", _q(Vector3.RIGHT, ankle * (-s * 0.55 - right_swing * 0.65)))
	_set_target("left_upper_arm", _q(Vector3.RIGHT, arm))
	_set_target("right_upper_arm", _q(Vector3.RIGHT, -arm))
	_set_target("left_lower_arm", _q(Vector3.RIGHT, -deg_to_rad(7.0) - elbow * (0.72 + 0.28 * right_swing)))
	_set_target("right_lower_arm", _q(Vector3.RIGHT, -deg_to_rad(7.0) - elbow * (0.72 + 0.28 * left_swing)))


func _set_target(role: String, target: Quaternion) -> void:
	for motor in _motors:
		if String(motor["role"]) == role:
			motor["target"] = target.normalized()
			return


func _apply_joint_motors() -> void:
	for motor in _motors:
		var parent_body := motor["parent"] as RigidBody3D
		var child_body := motor["child"] as RigidBody3D
		if parent_body == null or child_body == null:
			continue
		var parent_basis := parent_body.global_transform.basis.orthonormalized()
		var current := (parent_basis.inverse() * child_body.global_transform.basis.orthonormalized()).get_rotation_quaternion()
		var target := motor["target"] as Quaternion
		var error := (target * current.inverse()).normalized()
		if error.w < 0.0:
			error = Quaternion(-error.x, -error.y, -error.z, -error.w)
		var angle := 2.0 * acos(clampf(error.w, -1.0, 1.0))
		var sin_half := sqrt(maxf(1.0 - error.w * error.w, 0.0))
		var local_axis := Vector3.ZERO
		if sin_half > 1e-5:
			local_axis = Vector3(error.x, error.y, error.z) / sin_half
		var world_axis := parent_basis * local_axis
		var relative_omega := child_body.angular_velocity - parent_body.angular_velocity
		var torque := world_axis * angle * float(motor["kp"]) - relative_omega * float(motor["kd"])
		torque = _clamp_length(torque, float(motor["max_torque"]))
		child_body.apply_torque(torque)
		parent_body.apply_torque(-torque)


func _q(axis: Vector3, angle: float) -> Quaternion:
	if absf(angle) <= 1e-7:
		return Quaternion.IDENTITY
	return Quaternion(axis.normalized(), angle)


func _clamp_length(value: Vector3, maximum: float) -> Vector3:
	var length_sq := value.length_squared()
	if maximum <= 0.0 or length_sq <= maximum * maximum:
		return value
	return value * (maximum / sqrt(length_sq))


func _on_origin_shifted(delta_render: Vector3) -> void:
	if not active:
		return
	for value in _segments.values():
		var body := value as RigidBody3D
		body.global_position += delta_render
		body.sleeping = false
	for value in _joints.values():
		var joint := value as Joint3D
		joint.global_position += delta_render
