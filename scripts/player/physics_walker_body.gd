class_name PhysicsWalkerBody
extends RigidBody3D
## Pelvis / high-level COM controller for the authoritative-terrain active ragdoll.
##
## body_mass is the mass of the complete humanoid. This RigidBody3D is only the
## pelvis portion; ActiveRagdollRig owns the remaining distributed masses. When the
## articulated rig is active, authoritative terrain support comes through its two
## physical feet. The older virtual-foot path remains as a fallback.

const GRAVITY_MPS2 := 9.62
const MIN_NORMAL_DOT := 0.05

@export_group("Body")
@export var body_mass := 72.0
@export var pelvis_mass := 18.5
@export var body_radius := 0.16
@export var body_height := 0.32
@export var com_height := 0.90

@export_group("Locomotion")
@export var velocity_gain := 6.5
@export var max_ground_accel := 12.0
@export var max_air_accel := 1.8
@export var jump_speed := 5.2
@export var friction_coefficient := 1.05
@export var max_walkable_slope_deg := 52.0

@export_group("Fallback virtual legs")
@export var foot_separation := 0.26
@export var foot_fore_aft := 0.035
@export var support_probe := 0.24
@export var support_stiffness := 10500.0
@export var support_damping := 1250.0
@export var max_support_force_g := 3.4

@export_group("Balance")
@export var upright_kp := 900.0
@export var upright_kd := 125.0
@export var heading_kp := 280.0
@export var heading_kd := 48.0
@export var max_balance_torque := 1250.0
@export var terrain_normal_follow := 0.12
@export var acceleration_lean := 0.16
@export var max_lean_deg := 13.0

var active := false
var grounded := false
var desired_velocity := Vector3.ZERO
var desired_forward := Vector3.ZERO
var ragdoll: ActiveRagdollRig

var last_agl := INF
var last_surface_normal := Vector3.UP
var last_support_force := 0.0
var last_drive_force := 0.0
var last_slope_deg := 0.0
var left_foot_contact := false
var right_foot_contact := false

var _jump_requested := false
var _jump_cooldown := 0.0


func _ready() -> void:
	mass = pelvis_mass
	gravity_scale = 0.0
	linear_damp = 0.04
	angular_damp = 0.12
	can_sleep = false
	freeze = true

	var collision := CollisionShape3D.new()
	collision.name = "PelvisCollision"
	var capsule := CapsuleShape3D.new()
	capsule.radius = body_radius
	capsule.height = body_height
	collision.shape = capsule
	add_child(collision)

	Frames.origin_shifted.connect(_on_origin_shifted)


func activate_at(direction: Vector3, forward_hint: Vector3) -> void:
	if Planet.cfg == null:
		return
	var up := direction.normalized()
	TerrainContactSampler.request_surface(up)
	TerrainContactSampler.request_contact_height(up)
	var broad_height := TerrainContactSampler.height(up)
	var ground_height := TerrainContactSampler.contact_height(up, broad_height)
	var spawn_world := Vec3D.from_v3(up).mul(
		Planet.cfg.planet_radius + ground_height + com_height + 0.025
	)

	var forward := forward_hint - up * forward_hint.dot(up)
	if forward.length_squared() < 1e-8:
		var tangent := CubeSphere.tangent_basis(up)
		forward = (tangent[1] as Vector3).normalized()
	else:
		forward = forward.normalized()
	var right := forward.cross(up).normalized()
	var basis := Basis(right, up, -forward).orthonormalized()

	freeze = false
	active = true
	grounded = false
	_jump_requested = false
	_jump_cooldown = 0.0
	linear_velocity = Vector3.ZERO
	angular_velocity = Vector3.ZERO
	global_transform = Transform3D(basis, Frames.to_render(spawn_world))
	desired_velocity = Vector3.ZERO
	desired_forward = forward
	sleeping = false


func deactivate() -> void:
	active = false
	grounded = false
	desired_velocity = Vector3.ZERO
	_jump_requested = false
	linear_velocity = Vector3.ZERO
	angular_velocity = Vector3.ZERO
	freeze = true


func set_control(target_velocity: Vector3, forward: Vector3) -> void:
	desired_velocity = target_velocity
	if forward.length_squared() > 1e-8:
		desired_forward = forward.normalized()
	if active:
		sleeping = false


func request_jump() -> void:
	if active:
		_jump_requested = true
	sleeping = false


func world_position() -> Vec3D:
	return Frames.to_world(global_position)


func debug_state() -> Dictionary:
	var result := {
		"grounded": grounded,
		"agl": last_agl,
		"support_n": last_support_force,
		"drive_n": last_drive_force,
		"slope_deg": last_slope_deg,
		"left_contact": left_foot_contact,
		"right_contact": right_foot_contact,
		"speed": linear_velocity.length(),
		"pelvis_mass": mass,
		"total_mass": body_mass,
	}
	if ragdoll != null:
		result["ragdoll"] = ragdoll.debug_state()
	return result


func _integrate_forces(state: PhysicsDirectBodyState3D) -> void:
	if not active or Planet.cfg == null or not Planet.ready_state:
		return

	_jump_cooldown = maxf(0.0, _jump_cooldown - state.step)
	var render_pos := state.transform.origin
	var world := Frames.to_world(render_pos)
	if world.length_sq() <= 1.0:
		return

	var radial_up := world.normalized().to_v3()
	var body_forward := -state.transform.basis.z
	body_forward -= radial_up * body_forward.dot(radial_up)
	if body_forward.length_squared() < 1e-8:
		var tangent := CubeSphere.tangent_basis(radial_up)
		body_forward = (tangent[1] as Vector3).normalized()
	else:
		body_forward = body_forward.normalized()
	var body_right := body_forward.cross(radial_up).normalized()

	# Only the pelvis mass belongs to this body. ActiveRagdollRig applies radial
	# gravity independently to every other physical segment.
	state.apply_central_force(-radial_up * mass * GRAVITY_MPS2)

	var use_ragdoll_feet := ragdoll != null and ragdoll.active
	var left: Dictionary = {}
	var right: Dictionary = {}
	var total_support := 0.0
	var support_surface_normal := radial_up

	if use_ragdoll_feet:
		total_support = ragdoll.total_support_force
		support_surface_normal = ragdoll.support_normal
		left_foot_contact = ragdoll.left_support_force > 0.0
		right_foot_contact = ragdoll.right_support_force > 0.0
		last_agl = ragdoll.min_foot_agl
	else:
		left = _sample_foot(state, world, radial_up, body_right, body_forward, -1.0)
		right = _sample_foot(state, world, radial_up, body_right, body_forward, 1.0)
		left_foot_contact = bool(left["contact"])
		right_foot_contact = bool(right["contact"])
		var contact_count := int(left_foot_contact) + int(right_foot_contact)
		var weighted_normal := Vector3.ZERO
		if contact_count > 0 and _jump_cooldown <= 0.0:
			var share := 1.0 / float(contact_count)
			total_support += _apply_foot_support(state, left, radial_up, share)
			total_support += _apply_foot_support(state, right, radial_up, share)
			if float(left["support"]) > 0.0:
				weighted_normal += (left["normal"] as Vector3) * float(left["support"])
			if float(right["support"]) > 0.0:
				weighted_normal += (right["normal"] as Vector3) * float(right["support"])
		if weighted_normal.length_squared() > 1e-8:
			support_surface_normal = weighted_normal.normalized()
		last_agl = minf(float(left["agl"]), float(right["agl"]))

	if support_surface_normal.dot(radial_up) < 0.0:
		support_surface_normal = -support_surface_normal
	last_surface_normal = support_surface_normal
	last_support_force = total_support
	last_slope_deg = rad_to_deg(acos(clampf(support_surface_normal.dot(radial_up), -1.0, 1.0)))
	grounded = total_support > body_mass * GRAVITY_MPS2 * 0.16 and _jump_cooldown <= 0.0

	# Jump is a whole-body impulse delivered through the pelvis. The articulated
	# joints distribute it to the other masses. Release physical feet briefly so the
	# terrain springs cannot immediately oppose the impulse.
	if _jump_requested and grounded:
		state.apply_central_impulse(radial_up * body_mass * jump_speed)
		_jump_cooldown = 0.16
		if use_ragdoll_feet:
			ragdoll.begin_jump_release(_jump_cooldown)
	_jump_requested = false

	var target_velocity := desired_velocity - support_surface_normal * desired_velocity.dot(support_surface_normal)
	var current_tangent := state.linear_velocity - support_surface_normal * state.linear_velocity.dot(support_surface_normal)
	var velocity_error := target_velocity - current_tangent
	var accel_limit := max_ground_accel if grounded else max_air_accel
	var desired_accel := _clamp_length(velocity_error * velocity_gain, accel_limit)
	var drive_force := desired_accel * body_mass
	var walkable := last_slope_deg <= max_walkable_slope_deg
	if grounded:
		var traction_limit := total_support * friction_coefficient
		if not walkable:
			traction_limit *= 0.12
		drive_force = _clamp_length(drive_force, traction_limit)
		if use_ragdoll_feet:
			ragdoll.apply_drive_force(drive_force)
		else:
			_apply_drive_through_feet(state, drive_force, left, right, total_support)
	else:
		# Mild in-air authority acts at the pelvis only.
		state.apply_central_force(drive_force)
	last_drive_force = drive_force.length()

	_apply_balance_torque(state, radial_up, support_surface_normal, desired_accel)


func _sample_foot(
		state: PhysicsDirectBodyState3D,
		world: Vec3D,
		radial_up: Vector3,
		body_right: Vector3,
		body_forward: Vector3,
		side: float
	) -> Dictionary:
	var planar_offset := body_right * (side * foot_separation * 0.5) \
		+ body_forward * foot_fore_aft
	var probe_world := Frames.to_world(state.transform.origin + planar_offset)
	var direction := probe_world.normalized().to_v3()
	TerrainContactSampler.request_surface(direction)
	TerrainContactSampler.request_contact_height(direction)
	var broad_height := TerrainContactSampler.height(direction)
	var surface := TerrainContactSampler.surface(direction)
	var height := float(surface.get("height", broad_height))
	var precise_height := TerrainContactSampler.contact_height(direction, height)
	if is_finite(precise_height):
		height = precise_height
	var normal := direction
	var normal_value: Variant = surface.get("normal", direction)
	if normal_value is Vector3:
		normal = (normal_value as Vector3).normalized()
	if normal.dot(direction) < 0.0:
		normal = -normal

	var center_altitude := world.length() - Planet.cfg.planet_radius
	var agl := center_altitude - height
	var force_offset := planar_offset - radial_up * com_height
	var point_velocity := state.linear_velocity + state.angular_velocity.cross(force_offset)
	var normal_velocity := point_velocity.dot(normal)
	var contact := agl <= com_height + support_probe and normal.dot(radial_up) > MIN_NORMAL_DOT
	return {
		"contact": contact,
		"agl": agl,
		"normal": normal,
		"normal_velocity": normal_velocity,
		"offset": force_offset,
		"support": 0.0,
	}


func _apply_foot_support(
		state: PhysicsDirectBodyState3D,
		foot: Dictionary,
		radial_up: Vector3,
		share: float
	) -> float:
	if not bool(foot["contact"]):
		foot["support"] = 0.0
		return 0.0
	var normal := foot["normal"] as Vector3
	var gravity_normal := body_mass * GRAVITY_MPS2 * maxf(radial_up.dot(normal), 0.0) * share
	var compression := com_height - float(foot["agl"])
	var spring := support_stiffness * compression * share
	var damping := -support_damping * float(foot["normal_velocity"]) * share
	var maximum := body_mass * GRAVITY_MPS2 * max_support_force_g * share
	var magnitude := clampf(gravity_normal + spring + damping, 0.0, maximum)
	foot["support"] = magnitude
	if magnitude > 0.0:
		state.apply_force(normal * magnitude, foot["offset"] as Vector3)
	return magnitude


func _apply_drive_through_feet(
		state: PhysicsDirectBodyState3D,
		drive_force: Vector3,
		left: Dictionary,
		right: Dictionary,
		total_support: float
	) -> void:
	if total_support <= 1e-5:
		state.apply_central_force(drive_force)
		return
	if float(left["support"]) > 0.0:
		var weight := float(left["support"]) / total_support
		state.apply_force(drive_force * weight, left["offset"] as Vector3)
	if float(right["support"]) > 0.0:
		var weight := float(right["support"]) / total_support
		state.apply_force(drive_force * weight, right["offset"] as Vector3)


func _apply_balance_torque(
		state: PhysicsDirectBodyState3D,
		radial_up: Vector3,
		surface_normal_value: Vector3,
		desired_accel: Vector3
	) -> void:
	var base_up := radial_up
	if grounded:
		base_up = radial_up.lerp(surface_normal_value, clampf(terrain_normal_follow, 0.0, 1.0)).normalized()

	var lean := desired_accel * (acceleration_lean / GRAVITY_MPS2)
	var max_lean := tan(deg_to_rad(max_lean_deg))
	lean = _clamp_length(lean, max_lean)
	var target_up := (base_up + lean).normalized()
	var current_up := state.transform.basis.y.normalized()
	var cross_up := current_up.cross(target_up)
	var tilt_angle := acos(clampf(current_up.dot(target_up), -1.0, 1.0))
	var tilt_axis := Vector3.ZERO
	if cross_up.length_squared() > 1e-10:
		tilt_axis = cross_up.normalized()
	var tilt_omega := state.angular_velocity - target_up * state.angular_velocity.dot(target_up)
	var torque := tilt_axis * (tilt_angle * upright_kp) - tilt_omega * upright_kd

	var target_forward := desired_forward - target_up * desired_forward.dot(target_up)
	var current_forward := -state.transform.basis.z
	current_forward -= target_up * current_forward.dot(target_up)
	if target_forward.length_squared() > 1e-8 and current_forward.length_squared() > 1e-8:
		target_forward = target_forward.normalized()
		current_forward = current_forward.normalized()
		var yaw_error := atan2(
			target_up.dot(current_forward.cross(target_forward)),
			clampf(current_forward.dot(target_forward), -1.0, 1.0)
		)
		var yaw_rate := state.angular_velocity.dot(target_up)
		torque += target_up * (yaw_error * heading_kp - yaw_rate * heading_kd)

	state.apply_torque(_clamp_length(torque, max_balance_torque))


func _clamp_length(value: Vector3, maximum: float) -> Vector3:
	var length_sq := value.length_squared()
	if maximum <= 0.0 or length_sq <= maximum * maximum:
		return value
	return value * (maximum / sqrt(length_sq))


func _on_origin_shifted(delta_render: Vector3) -> void:
	if not active:
		return
	global_position += delta_render
	sleeping = false
