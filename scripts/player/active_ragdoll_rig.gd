class_name ActiveRagdollRig
extends Node3D
## Stable articulated active-ragdoll layer for PhysicsWalkerBody.
##
## The pelvis is the existing PhysicsWalkerBody. The remaining body segments are
## real RigidBody3D nodes connected by positional PinJoint3D constraints. Joint
## orientation is controlled only by our finite PD torques; the physics solver is
## no longer asked to enforce hard 6-DOF angular limits while the PD controller is
## simultaneously fighting those limits.
##
## During this stabilization stage, load-bearing terrain support is intentionally
## applied through two pelvis-relative virtual soles. The articulated feet still
## exist and move physically, but they do not receive the very large contact spring
## impulses that made the 1 kg foot bodies explode at 60 Hz. Once the joint rig is
## stable we can migrate support back to the physical feet with substepping.

const GRAVITY_MPS2 := 9.62
const WALK_REFERENCE_SPEED := 4.6
const RUN_REFERENCE_SPEED := 11.0

@export var pelvis: PhysicsWalkerBody
@export var enabled := true

@export_group("Stable support")
@export var virtual_foot_separation := 0.26
@export var virtual_foot_fore_aft := 0.035
@export var virtual_sole_clearance := 0.025
@export var support_probe := 0.18
@export var support_stiffness := 7000.0
@export var support_damping := 900.0
@export var support_force_limit_g := 1.9

@export_group("Motor startup")
@export var motor_ramp_seconds := 1.20
@export var gait_delay_seconds := 0.65
@export var max_motor_error_deg := 42.0
@export var max_segment_angular_speed := 9.0

@export_group("Motor response")
@export var gait_strength := 1.0
@export var torso_kp := 180.0
@export var torso_kd := 32.0
@export var hip_kp := 150.0
@export var hip_kd := 26.0
@export var knee_kp := 120.0
@export var knee_kd := 22.0
@export var ankle_kp := 70.0
@export var ankle_kd := 14.0
@export var shoulder_kp := 60.0
@export var shoulder_kd := 12.0
@export var elbow_kp := 45.0
@export var elbow_kd := 9.0

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
var _startup_elapsed := 0.0
var _motor_blend := 0.0
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
	if _segments.is_empty():
		_build_rig()
	if _segments.is_empty():
		return

	active = true
	grounded = false
	_startup_elapsed = 0.0
	_motor_blend = 0.0
	_gait_phase = 0.0
	_gait_blend = 0.0
	_jump_release_left = 0.0
	total_support_force = 0.0
	left_support_force = 0.0
	right_support_force = 0.0

	# Place every body in a constraint-satisfying neutral pose BEFORE unfreezing.
	# Physics cannot step halfway through this function, but this ordering also makes
	# the intended startup state explicit and avoids a frame with bodies at origin.
	var base := pelvis.global_transform
	for role in _segments.keys():
		var body := _segments[role] as RigidBody3D
		body.freeze = true
		body.global_transform = base * Transform3D(Basis.IDENTITY, _local_positions[role] as Vector3)
		body.linear_velocity = pelvis.linear_velocity
		body.angular_velocity = pelvis.angular_velocity
	_reset_joint_frames()
	for motor in _motors:
		motor["target"] = Quaternion.IDENTITY
	for value in _segments.values():
		var body := value as RigidBody3D
		body.freeze = false
		body.sleeping = false


func deactivate() -> void:
	active = false
	grounded = false
	total_support_force = 0.0
	left_support_force = 0.0
	right_support_force = 0.0
	_motor_blend = 0.0
	for value in _segments.values():
		var body := value as RigidBody3D
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
	if parent_body == null or child == null:
		return Quaternion.IDENTITY
	var parent_basis := parent_body.global_transform.basis.orthonormalized()
	var child_basis := child.global_transform.basis.orthonormalized()
	return (parent_basis.inverse() * child_basis).get_rotation_quaternion().normalized()


func debug_state() -> Dictionary:
	return {
		"active": active,
		"grounded": grounded,
		"support_n": total_support_force,
		"left_support_n": left_support_force,
		"right_support_n": right_support_force,
		"foot_agl": min_foot_agl,
		"segments": _segments.size(),
		"motor_blend": _motor_blend,
		"startup_s": _startup_elapsed,
	}


func apply_drive_force(force: Vector3) -> void:
	if not active or pelvis == null:
		return
	# The high-level controller is already traction-limited using total_support_force.
	# Apply its result at the pelvis while the articulated joint layer is stabilized.
	pelvis.apply_central_force(force)


func _physics_process(dt: float) -> void:
	if not active or pelvis == null or not pelvis.active or Planet.cfg == null or not Planet.ready_state:
		return

	_startup_elapsed += dt
	_jump_release_left = maxf(0.0, _jump_release_left - dt)
	_motor_blend = clampf(
		(_startup_elapsed - 0.08) / maxf(motor_ramp_seconds, 0.05),
		0.0,
		1.0
	)

	_apply_segment_gravity()
	_update_stable_support()
	_update_gait_targets(dt)
	_apply_joint_motors()
	_limit_segment_angular_velocity()


func _build_rig() -> void:
	if pelvis == null:
		call_deferred("_build_rig")
		return
	if not _segments.is_empty():
		return

	# All positions are pelvis-local. All rigid bodies begin with the pelvis basis,
	# which means identity relative rotation is a valid, calm neutral motor target.
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
		body.linear_damp = 0.10
		body.angular_damp = 1.15
		add_child(body)

	_disable_internal_collisions()

	_add_motor("torso", pelvis, _segments["torso"], Vector3(0.0, 0.105, 0.0), torso_kp, torso_kd, 160.0)
	_add_motor("head", _segments["torso"], _segments["head"], Vector3(0.0, 0.525, -0.005), 48.0, 10.0, 32.0)
	_add_motor("left_upper_leg", pelvis, _segments["left_upper_leg"], Vector3(-0.13, -0.07, 0.0), hip_kp, hip_kd, 130.0)
	_add_motor("right_upper_leg", pelvis, _segments["right_upper_leg"], Vector3(0.13, -0.07, 0.0), hip_kp, hip_kd, 130.0)
	_add_motor("left_lower_leg", _segments["left_upper_leg"], _segments["left_lower_leg"], Vector3(-0.13, -0.445, 0.005), knee_kp, knee_kd, 100.0)
	_add_motor("right_lower_leg", _segments["right_upper_leg"], _segments["right_lower_leg"], Vector3(0.13, -0.445, 0.005), knee_kp, knee_kd, 100.0)
	_add_motor("left_foot", _segments["left_lower_leg"], _segments["left_foot"], Vector3(-0.13, -0.80, -0.025), ankle_kp, ankle_kd, 55.0)
	_add_motor("right_foot", _segments["right_lower_leg"], _segments["right_foot"], Vector3(0.13, -0.80, -0.025), ankle_kp, ankle_kd, 55.0)
	_add_motor("left_upper_arm", _segments["torso"], _segments["left_upper_arm"], Vector3(-0.255, 0.43, 0.0), shoulder_kp, shoulder_kd, 40.0)
	_add_motor("right_upper_arm", _segments["torso"], _segments["right_upper_arm"], Vector3(0.255, 0.43, 0.0), shoulder_kp, shoulder_kd, 40.0)
	_add_motor("left_lower_arm", _segments["left_upper_arm"], _segments["left_lower_arm"], Vector3(-0.31, 0.135, 0.0), elbow_kp, elbow_kd, 25.0)
	_add_motor("right_lower_arm", _segments["right_upper_arm"], _segments["right_lower_arm"], Vector3(0.31, 0.135, 0.0), elbow_kp, elbow_kd, 25.0)


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
		kp: float,
		kd: float,
		max_torque: float
	) -> void:
	# PinJoint3D owns only the positional articulation. Angular behavior belongs to
	# the explicit PD motor below, avoiding two angular solvers fighting each other.
	var joint := PinJoint3D.new()
	joint.name = "%sJoint" % role.capitalize().replace(" ", "")
	joint.exclude_nodes_from_collision = true
	joint.solver_priority = 2
	add_child(joint)
	joint.node_a = joint.get_path_to(parent_body)
	joint.node_b = joint.get_path_to(child_body)
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


func _reset_joint_frames() -> void:
	if pelvis == null:
		return
	for motor in _motors:
		var role := String(motor["role"])
		var joint := _joints[role] as PinJoint3D
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


func _update_stable_support() -> void:
	left_support_force = 0.0
	right_support_force = 0.0
	total_support_force = 0.0
	min_foot_agl = INF
	if _jump_release_left > 0.0:
		grounded = false
		return

	var world := pelvis.world_position()
	if world.length_sq() <= 1.0:
		return
	var radial_up := world.normalized().to_v3()
	var forward := -pelvis.global_transform.basis.z
	forward -= radial_up * forward.dot(radial_up)
	if forward.length_squared() < 1e-8:
		var tangent := CubeSphere.tangent_basis(radial_up)
		forward = (tangent[1] as Vector3).normalized()
	else:
		forward = forward.normalized()
	var right := forward.cross(radial_up).normalized()

	var left := _sample_virtual_sole(radial_up, right, forward, -1.0)
	var right_sole := _sample_virtual_sole(radial_up, right, forward, 1.0)
	left_support_force = float(left["support"])
	right_support_force = float(right_sole["support"])
	total_support_force = left_support_force + right_support_force
	min_foot_agl = minf(float(left["agl"]), float(right_sole["agl"]))

	var combined_force := Vector3.ZERO
	if left_support_force > 0.0:
		combined_force += (left["normal"] as Vector3) * left_support_force
	if right_support_force > 0.0:
		combined_force += (right_sole["normal"] as Vector3) * right_support_force
	if combined_force.length_squared() > 1e-8:
		support_normal = combined_force.normalized()
		pelvis.apply_central_force(combined_force)
	else:
		support_normal = radial_up
	grounded = total_support_force > pelvis.body_mass * GRAVITY_MPS2 * 0.16


func _sample_virtual_sole(
		radial_up: Vector3,
		right: Vector3,
		forward: Vector3,
		side: float
	) -> Dictionary:
	var offset := right * (side * virtual_foot_separation * 0.5) \
		+ forward * virtual_foot_fore_aft - radial_up * pelvis.com_height
	var probe_render := pelvis.global_position + offset
	var probe_world := Frames.to_world(probe_render)
	var direction := probe_world.normalized().to_v3()
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

	var agl := probe_world.length() - Planet.cfg.planet_radius - height
	var support := 0.0
	if agl <= support_probe and normal.dot(radial_up) > 0.05:
		var point_velocity := pelvis.linear_velocity + pelvis.angular_velocity.cross(offset)
		var normal_velocity := point_velocity.dot(normal)
		var gravity_share := pelvis.body_mass * GRAVITY_MPS2 * maxf(radial_up.dot(normal), 0.0) * 0.5
		var spring := support_stiffness * (virtual_sole_clearance - agl) * 0.5
		var damping := -support_damping * normal_velocity * 0.5
		var maximum := pelvis.body_mass * GRAVITY_MPS2 * support_force_limit_g * 0.5
		support = clampf(gravity_share + spring + damping, 0.0, maximum)
	return {"agl": agl, "normal": normal, "support": support}


func _update_gait_targets(dt: float) -> void:
	# Keep every joint at neutral during the settle window. This is deliberately
	# separate from motor ramp-up: constraints settle first, then gait begins.
	if _startup_elapsed < gait_delay_seconds:
		_gait_blend = move_toward(_gait_blend, 0.0, dt * 8.0)
		_set_neutral_targets()
		return

	var up := pelvis.world_position().normalized().to_v3()
	var planar_velocity := pelvis.linear_velocity - up * pelvis.linear_velocity.dot(up)
	var speed := planar_velocity.length()
	var movement_blend := clampf(speed / 0.65, 0.0, 1.0)
	if not grounded:
		movement_blend *= 0.42
	_gait_blend = move_toward(_gait_blend, movement_blend, dt * 5.0)
	var speed_norm := clampf(speed / WALK_REFERENCE_SPEED, 0.0, 1.0)
	var run_norm := clampf(
		(speed - WALK_REFERENCE_SPEED) / maxf(RUN_REFERENCE_SPEED - WALK_REFERENCE_SPEED, 0.1),
		0.0,
		1.0
	)
	if speed > 0.08:
		var cadence_hz := lerpf(0.95, 1.65, speed_norm) + run_norm * 0.42
		_gait_phase = fmod(_gait_phase + dt * TAU * cadence_hz, TAU)

	var s := sin(_gait_phase)
	var c := cos(_gait_phase)
	var left_swing := maxf(s, 0.0)
	var right_swing := maxf(-s, 0.0)
	var hip := deg_to_rad(lerpf(14.0, 28.0, speed_norm) + run_norm * 10.0) * s * _gait_blend * gait_strength
	var knee := deg_to_rad(lerpf(24.0, 43.0, speed_norm) + run_norm * 12.0) * _gait_blend * gait_strength
	var ankle := deg_to_rad(lerpf(6.0, 12.0, speed_norm)) * _gait_blend * gait_strength
	var arm := deg_to_rad(lerpf(10.0, 24.0, speed_norm) + run_norm * 8.0) * s * _gait_blend * gait_strength
	var elbow := deg_to_rad(6.0 + 13.0 * speed_norm + 10.0 * run_norm) * _gait_blend * gait_strength
	var torso_yaw := deg_to_rad(3.5 + 2.0 * run_norm) * s * _gait_blend * gait_strength
	var torso_side := deg_to_rad(1.2) * c * _gait_blend * gait_strength

	_set_target("torso", _q(Vector3.UP, -torso_yaw) * _q(Vector3.BACK, torso_side))
	_set_target("head", _q(Vector3.UP, torso_yaw * 0.35))
	_set_target("left_upper_leg", _q(Vector3.RIGHT, -hip))
	_set_target("right_upper_leg", _q(Vector3.RIGHT, hip))
	_set_target("left_lower_leg", _q(Vector3.RIGHT, -knee * left_swing - deg_to_rad(2.0)))
	_set_target("right_lower_leg", _q(Vector3.RIGHT, -knee * right_swing - deg_to_rad(2.0)))
	_set_target("left_foot", _q(Vector3.RIGHT, ankle * (s * 0.50 - left_swing * 0.55)))
	_set_target("right_foot", _q(Vector3.RIGHT, ankle * (-s * 0.50 - right_swing * 0.55)))
	_set_target("left_upper_arm", _q(Vector3.RIGHT, arm))
	_set_target("right_upper_arm", _q(Vector3.RIGHT, -arm))
	_set_target("left_lower_arm", _q(Vector3.RIGHT, -deg_to_rad(6.0) - elbow * (0.70 + 0.30 * right_swing)))
	_set_target("right_lower_arm", _q(Vector3.RIGHT, -deg_to_rad(6.0) - elbow * (0.70 + 0.30 * left_swing)))


func _set_neutral_targets() -> void:
	for motor in _motors:
		motor["target"] = Quaternion.IDENTITY


func _set_target(role: String, target: Quaternion) -> void:
	for motor in _motors:
		if String(motor["role"]) == role:
			motor["target"] = target.normalized()
			return


func _apply_joint_motors() -> void:
	if _motor_blend <= 0.0:
		return
	var max_error := deg_to_rad(max_motor_error_deg)
	for motor in _motors:
		var parent_body := motor["parent"] as RigidBody3D
		var child_body := motor["child"] as RigidBody3D
		if parent_body == null or child_body == null:
			continue

		var parent_basis := parent_body.global_transform.basis.orthonormalized()
		var child_basis := child_body.global_transform.basis.orthonormalized()
		var current := (parent_basis.inverse() * child_basis).get_rotation_quaternion().normalized()
		var target := motor["target"] as Quaternion
		var error := (target * current.inverse()).normalized()
		if error.w < 0.0:
			error = Quaternion(-error.x, -error.y, -error.z, -error.w)
		var angle := 2.0 * acos(clampf(error.w, -1.0, 1.0))
		angle = minf(angle, max_error)
		var sin_half := sqrt(maxf(1.0 - error.w * error.w, 0.0))
		var local_axis := Vector3.ZERO
		if sin_half > 1e-5:
			local_axis = Vector3(error.x, error.y, error.z) / sin_half
		var world_axis := parent_basis * local_axis
		var relative_omega := child_body.angular_velocity - parent_body.angular_velocity
		var torque := world_axis * angle * float(motor["kp"]) - relative_omega * float(motor["kd"])
		torque = _clamp_length(torque, float(motor["max_torque"]) * _motor_blend)
		child_body.apply_torque(torque)
		parent_body.apply_torque(-torque)


func _limit_segment_angular_velocity() -> void:
	var limit := maxf(max_segment_angular_speed, 0.1)
	for value in _segments.values():
		var body := value as RigidBody3D
		if body.angular_velocity.length_squared() > limit * limit:
			body.angular_velocity = body.angular_velocity.normalized() * limit
	# The pelvis receives reaction torque from every limb. Keep catastrophic startup
	# errors from turning into an unrecoverable spinning whole-body state.
	var pelvis_limit := limit * 0.8
	if pelvis.angular_velocity.length_squared() > pelvis_limit * pelvis_limit:
		pelvis.angular_velocity = pelvis.angular_velocity.normalized() * pelvis_limit


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
