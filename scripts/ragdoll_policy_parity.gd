extends "res://scripts/ragdoll_policy_test.gd"
## Cross-engine parity layer for trained-policy playback.
##
## Stage-1 standing is learned in PhysX at 240 Hz. The policy observation also
## uses Isaac articulation *link-frame* positions for head/hands/feet, not rigid
## body centers of mass. This layer makes the viewport test obey those same two
## contracts while leaving the underlying Jolt ragdoll physical.

const TRAINING_PHYSICS_HZ: int = 240

var _saved_physics_ticks: int = 120
var _policy_changed_physics_rate: bool = false


func _ready() -> void:
	_saved_physics_ticks = Engine.physics_ticks_per_second
	super._ready()
	_sync_stage0_contract_drift()


func _exit_tree() -> void:
	_restore_viewport_physics_rate()
	super._exit_tree()


func activate_loaded_policy() -> void:
	if _policy_loaded and not _policy_active:
		_enable_training_physics_rate()
	super.activate_loaded_policy()
	if not _policy_active:
		_restore_viewport_physics_rate()


func deactivate_policy() -> void:
	super.deactivate_policy()
	_restore_viewport_physics_rate()


func unload_policy() -> void:
	super.unload_policy()
	_restore_viewport_physics_rate()


func _enable_training_physics_rate() -> void:
	if _policy_changed_physics_rate:
		return
	_saved_physics_ticks = Engine.physics_ticks_per_second
	Engine.physics_ticks_per_second = TRAINING_PHYSICS_HZ
	_policy_changed_physics_rate = true
	print(
		"Asterra viewport policy parity: physics %d -> %d Hz"
		% [_saved_physics_ticks, TRAINING_PHYSICS_HZ]
	)


func _restore_viewport_physics_rate() -> void:
	if not _policy_changed_physics_rate:
		return
	Engine.physics_ticks_per_second = _saved_physics_ticks
	_policy_changed_physics_rate = false
	print("Asterra viewport policy parity: restored physics to %d Hz" % _saved_physics_ticks)


func _send_policy_step() -> void:
	if _policy_peer == null or _policy_joint_order.size() != 18:
		return
	var pelvis: RigidBody3D = _bodies.get("pelvis") as RigidBody3D
	if pelvis == null:
		return

	var root_q: Quaternion = pelvis.global_transform.basis.get_rotation_quaternion().normalized()
	var key_positions: Array = []
	for body_name: String in POLICY_KEY_BODIES:
		# Isaac's body_state_w position is the link actor-frame position. Our
		# Godot RigidBody3D origins are physical centers, so reconstruct the
		# child link frame at its anatomical parent-joint anchor.
		key_positions.append(_vector_array(_policy_link_frame_position(body_name)))

	var joint_quats: Array = []
	for joint_name: String in _policy_joint_order:
		joint_quats.append(_quaternion_array(_relative_joint_quaternion(joint_name)))

	var packet: Dictionary = {
		"type": "step",
		"seq": _policy_sequence,
		"dt": POLICY_DT,
		"root_pos": _vector_array(pelvis.global_position),
		"root_quat": _quaternion_array(root_q),
		"root_lin_vel": _vector_array(pelvis.linear_velocity),
		"root_ang_vel": _vector_array(pelvis.angular_velocity),
		"key_body_pos": key_positions,
		"joint_quat": joint_quats,
		"foot_force_n": [
			_policy_foot_force("left_foot"),
			_policy_foot_force("right_foot"),
		],
	}
	_policy_sequence += 1
	_send_policy_packet(packet)


func _policy_link_frame_position(body_name: String) -> Vector3:
	if not _bodies.has(body_name):
		return Vector3.ZERO
	var body: RigidBody3D = _bodies[body_name] as RigidBody3D
	if body == null:
		return Vector3.ZERO
	if body_name == "pelvis":
		return body.global_position

	# In the generated URDF every physical child link is rooted at the
	# anatomical joint that connects it to its parent. The Godot body origin is
	# instead at neutral_center_m. Reconstruct that link origin in the moving
	# body's local frame so it follows rotation exactly.
	for joint_name_variant: Variant in _joint_specs_by_name.keys():
		var joint_name: String = String(joint_name_variant)
		var spec: Dictionary = _joint_specs_by_name[joint_name]
		if String(spec.get("child", "")) != body_name:
			continue
		var neutral_center: Vector3 = Vector3(_initial_positions[body_name])
		var neutral_anchor: Vector3 = Vector3(spec["anchor"])
		var anchor_local: Vector3 = neutral_anchor - neutral_center
		return body.to_global(anchor_local)

	# This should not be used by the five policy key bodies, but retain a safe
	# center fallback rather than sending a malformed packet.
	return body.global_position


func _sync_stage0_contract_drift() -> void:
	# The training contract's knee hard envelope was widened after the original
	# hand-written Jolt test rig. Coupled ROM still tightens it near extension,
	# but the outer physical envelope must match the generated training asset.
	for knee_name: String in ["left_knee", "right_knee"]:
		if not _joint_specs_by_name.has(knee_name):
			continue
		var spec: Dictionary = _joint_specs_by_name[knee_name]
		spec["lower"] = Vector3(-145.0, -10.0, -4.0)
		spec["upper"] = Vector3(5.0, 10.0, 4.0)
		_joint_specs_by_name[knee_name] = spec

	if _anatomical_limits_enabled:
		_restore_static_anatomical_ranges()
		_apply_coupled_limits()
