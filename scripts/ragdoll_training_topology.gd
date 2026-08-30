extends "res://scripts/ragdoll_policy_parity.gd"
## Jolt replica of the articulation topology used by the Isaac/PhysX trainer.
##
## The physical 19 bodies are the same validated viewport bodies, but every
## anatomical 3-DOF joint is expanded exactly like the generated training asset:
##
## parent -> X revolute -> virtual_x -> Y revolute -> virtual_y -> Z revolute -> child
##
## This produces 19 physical bodies + 36 near-massless virtual links + 54 serial
## revolute coordinates. It is still Jolt, not NVIDIA PhysX, but it removes the
## largest topology difference of the compact 18x Generic6DOF test rig.

const TRAINING_PHYSICAL_BODY_COUNT: int = 19
const TRAINING_VIRTUAL_BODY_COUNT: int = 36
const TRAINING_DOF_COUNT: int = 54
const TRAINING_VIRTUAL_MASS_KG: float = 0.0001
const TRAINING_VIRTUAL_COLLIDER_RADIUS: float = 0.00001
const TRAINING_VIRTUAL_MARKER_RADIUS: float = 0.008
const TRAINING_AXES: Array[String] = ["x", "y", "z"]

var _training_virtual_bodies: Dictionary = {}
var _training_virtual_initial: Dictionary = {}
var _training_dof_joints: Dictionary = {}
var _training_dof_specs: Dictionary = {}
var _training_dof_order: Array[String] = []
var _training_dof_frame_a_local: Dictionary = {}
var _training_dof_frame_b_local: Dictionary = {}


func get_skeleton_mode() -> String:
	return "training"


func _build_ragdoll() -> void:
	# Reuse the same 19 physical bodies/geometry already validated by the passive
	# Jolt smoke rig. Only the articulation graph is replaced here.
	for body_variant: Variant in _body_specs():
		_make_body(body_variant as Dictionary)

	var physical_names: Array = _bodies.keys()
	for first_index: int in range(physical_names.size()):
		for second_index: int in range(first_index + 1, physical_names.size()):
			var first: RigidBody3D = _bodies[physical_names[first_index]] as RigidBody3D
			var second: RigidBody3D = _bodies[physical_names[second_index]] as RigidBody3D
			if first != null and second != null:
				first.add_collision_exception_with(second)
				second.add_collision_exception_with(first)

	for joint_variant: Variant in _training_joint_specs():
		var joint_spec: Dictionary = joint_variant as Dictionary
		_make_training_joint_chain(joint_spec)


func _training_joint_specs() -> Array:
	var result: Array = []
	for joint_variant: Variant in _joint_specs():
		var spec: Dictionary = (joint_variant as Dictionary).duplicate(true)
		var name: String = String(spec["name"])
		# Match the current Stage-0A training contract outer knee envelope. The
		# learned target coupling still tightens Y/Z near extension in Python.
		if name == "left_knee" or name == "right_knee":
			spec["lower"] = Vector3(-145.0, -10.0, -4.0)
			spec["upper"] = Vector3(5.0, 10.0, 4.0)
		result.append(spec)
	return result


func _make_training_joint_chain(spec: Dictionary) -> void:
	_validate_joint_spec(spec)
	var joint_name: String = String(spec["name"])
	var parent_body: RigidBody3D = _bodies[String(spec["parent"])] as RigidBody3D
	var child_body: RigidBody3D = _bodies[String(spec["child"])] as RigidBody3D
	var frame_basis: Basis = _make_anatomical_basis(
		Vector3(spec["flex_axis_world"]),
		Vector3(spec["twist_axis_world"])
	)
	var frame_transform: Transform3D = Transform3D(
		frame_basis,
		Vector3(spec["anchor"]) + SPAWN_OFFSET
	)

	var virtual_x: RigidBody3D = _make_training_virtual_body(
		"frame__%s_x" % joint_name,
		frame_transform,
		Color(0.95, 0.36, 0.65, 0.82),
	)
	var virtual_y: RigidBody3D = _make_training_virtual_body(
		"frame__%s_y" % joint_name,
		frame_transform,
		Color(0.32, 0.86, 0.94, 0.82),
	)

	var lower: Vector3 = Vector3(spec["lower"])
	var upper: Vector3 = Vector3(spec["upper"])
	_make_training_revolute(
		"%s_x" % joint_name,
		joint_name,
		parent_body,
		virtual_x,
		frame_transform,
		"x",
		lower.x,
		upper.x,
	)
	_make_training_revolute(
		"%s_y" % joint_name,
		joint_name,
		virtual_x,
		virtual_y,
		frame_transform,
		"y",
		lower.y,
		upper.y,
	)
	_make_training_revolute(
		"%s_z" % joint_name,
		joint_name,
		virtual_y,
		child_body,
		frame_transform,
		"z",
		lower.z,
		upper.z,
	)

	# The existing policy loader validates the 18 semantic joint names. Point
	# each semantic entry at its X coordinate; actual state/drives use the 54-DOF
	# dictionaries below.
	_joints[joint_name] = _training_dof_joints["%s_x" % joint_name]
	_joint_specs_by_name[joint_name] = spec


func _make_training_virtual_body(
	body_name: String,
	body_transform: Transform3D,
	marker_color: Color,
) -> RigidBody3D:
	var body: RigidBody3D = RigidBody3D.new()
	body.name = body_name
	body.mass = TRAINING_VIRTUAL_MASS_KG
	body.can_sleep = false
	body.continuous_cd = true
	body.collision_layer = 0
	body.collision_mask = 0
	body.freeze = true
	body.transform = body_transform
	add_child(body)

	# A microscopic non-colliding sphere gives Jolt a valid mass/inertia-bearing
	# rigid body, equivalent in purpose to the transparent import-anchor visual in
	# the generated URDF/USD chain.
	var collision: CollisionShape3D = CollisionShape3D.new()
	var shape: SphereShape3D = SphereShape3D.new()
	shape.radius = TRAINING_VIRTUAL_COLLIDER_RADIUS
	collision.shape = shape
	body.add_child(collision)

	# Debug marker only. The real training anchor is 10 µm and invisible; these
	# colored points make the serial X/Y frame links visible in the viewport.
	var marker: MeshInstance3D = MeshInstance3D.new()
	var marker_mesh: SphereMesh = SphereMesh.new()
	marker_mesh.radius = TRAINING_VIRTUAL_MARKER_RADIUS
	marker_mesh.height = TRAINING_VIRTUAL_MARKER_RADIUS * 2.0
	marker.mesh = marker_mesh
	var marker_material: StandardMaterial3D = StandardMaterial3D.new()
	marker_material.albedo_color = marker_color
	marker_material.emission_enabled = true
	marker_material.emission = Color(marker_color.r, marker_color.g, marker_color.b)
	marker_material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	marker.material_override = marker_material
	body.add_child(marker)

	_training_virtual_bodies[body_name] = body
	_training_virtual_initial[body_name] = body_transform
	return body


func _make_training_revolute(
	dof_name: String,
	anatomical_joint: String,
	parent_body: RigidBody3D,
	child_body: RigidBody3D,
	frame_transform: Transform3D,
	active_axis: String,
	lower_deg: float,
	upper_deg: float,
) -> void:
	var joint: Generic6DOFJoint3D = Generic6DOFJoint3D.new()
	joint.name = dof_name
	joint.transform = frame_transform
	add_child(joint)
	joint.node_a = joint.get_path_to(parent_body)
	joint.node_b = joint.get_path_to(child_body)
	joint.exclude_nodes_from_collision = true
	_lock_linear_axes(joint)

	for axis_name: String in TRAINING_AXES:
		joint.set("angular_limit_%s/enabled" % axis_name, true)
		if axis_name == active_axis:
			_set_angular_range(joint, axis_name, lower_deg, upper_deg)
		else:
			_set_angular_range(joint, axis_name, 0.0, 0.0)
		joint.set("angular_motor_%s/enabled" % axis_name, false)
		joint.set("angular_spring_%s/enabled" % axis_name, false)

	var joint_world_basis: Basis = frame_transform.basis.orthonormalized()
	_training_dof_frame_a_local[dof_name] = (
		parent_body.global_transform.basis.inverse() * joint_world_basis
	).orthonormalized()
	_training_dof_frame_b_local[dof_name] = (
		child_body.global_transform.basis.inverse() * joint_world_basis
	).orthonormalized()
	_training_dof_joints[dof_name] = joint
	_training_dof_order.append(dof_name)
	_training_dof_specs[dof_name] = {
		"anatomical_joint": anatomical_joint,
		"axis": active_axis,
		"lower_deg": lower_deg,
		"upper_deg": upper_deg,
		"parent_body": parent_body,
		"child_body": child_body,
	}
	_joint_count += 1


func _assert_articulation_contract() -> void:
	assert(_bodies.size() == TRAINING_PHYSICAL_BODY_COUNT, "Training topology needs 19 physical bodies")
	assert(_training_virtual_bodies.size() == TRAINING_VIRTUAL_BODY_COUNT, "Training topology needs 36 virtual links")
	assert(_training_dof_joints.size() == TRAINING_DOF_COUNT, "Training topology needs 54 serial revolute coordinates")
	assert(_training_dof_order.size() == TRAINING_DOF_COUNT, "Canonical 54-DOF order is incomplete")
	assert(_joints.size() == 18, "Training topology needs 18 semantic joint groups")
	print("Asterra training-topology viewport ready: 19 physical + 36 virtual links / 54 serial DOFs")
	print("Training graph: parent -> X -> virtual_x -> Y -> virtual_y -> Z -> child")


func _apply_coupled_limits() -> void:
	# PhysX keeps the generated outer hard limits static. Pose-dependent coupling
	# is applied to neural targets by CanonicalActionProcessor, not by mutating the
	# hard solver envelope each physics step.
	pass


func _restore_static_anatomical_ranges() -> void:
	for dof_name: String in _training_dof_order:
		var joint: Generic6DOFJoint3D = _training_dof_joints[dof_name] as Generic6DOFJoint3D
		var spec: Dictionary = _training_dof_specs[dof_name] as Dictionary
		var active_axis: String = String(spec["axis"])
		for axis_name: String in TRAINING_AXES:
			if axis_name != active_axis:
				joint.set("angular_limit_%s/enabled" % axis_name, true)
				_set_angular_range(joint, axis_name, 0.0, 0.0)
				continue
			joint.set("angular_limit_%s/enabled" % axis_name, _anatomical_limits_enabled)
			if axis_name == "y" and not _axial_twist_enabled:
				joint.set("angular_limit_%s/enabled" % axis_name, true)
				_set_angular_range(joint, axis_name, 0.0, 0.0)
			else:
				_set_angular_range(
					joint,
					axis_name,
					float(spec["lower_deg"]),
					float(spec["upper_deg"]),
				)


func _set_anatomical_limits_enabled(enabled: bool) -> void:
	_anatomical_limits_enabled = enabled
	_restore_static_anatomical_ranges()
	_update_overlay()


func _set_axial_twist_enabled(enabled: bool) -> void:
	_axial_twist_enabled = enabled
	_restore_static_anatomical_ranges()
	_update_overlay()


func _sync_stage0_contract_drift() -> void:
	# Knee drift is already corrected in _training_joint_specs().
	pass


func _training_joint_position(dof_name: String) -> float:
	var spec: Dictionary = _training_dof_specs[dof_name] as Dictionary
	var parent_body: RigidBody3D = spec["parent_body"] as RigidBody3D
	var child_body: RigidBody3D = spec["child_body"] as RigidBody3D
	if parent_body == null or child_body == null:
		return 0.0
	var frame_a_local: Basis = _training_dof_frame_a_local[dof_name]
	var frame_b_local: Basis = _training_dof_frame_b_local[dof_name]
	var frame_a_world: Basis = (
		parent_body.global_transform.basis * frame_a_local
	).orthonormalized()
	var frame_b_world: Basis = (
		child_body.global_transform.basis * frame_b_local
	).orthonormalized()
	var relative: Basis = (
		frame_a_world.inverse() * frame_b_world
	).orthonormalized()
	var q: Quaternion = relative.get_rotation_quaternion().normalized()
	var axis_name: String = String(spec["axis"])
	var axis_vector: Vector3 = Vector3.RIGHT
	if axis_name == "y":
		axis_vector = Vector3.UP
	elif axis_name == "z":
		axis_vector = Vector3(0.0, 0.0, 1.0)
	var q_vector: Vector3 = Vector3(q.x, q.y, q.z)
	var angle: float = 2.0 * atan2(q_vector.dot(axis_vector), q.w)
	return wrapf(angle, -PI, PI)


func _send_policy_step() -> void:
	if _policy_peer == null or _policy_joint_order.size() != 18:
		return
	if _training_dof_order.size() != TRAINING_DOF_COUNT:
		return
	var pelvis: RigidBody3D = _bodies.get("pelvis") as RigidBody3D
	if pelvis == null:
		return

	var root_q: Quaternion = pelvis.global_transform.basis.get_rotation_quaternion().normalized()
	var key_positions: Array = []
	for body_name: String in POLICY_KEY_BODIES:
		# Physical body origins are COM-centered in this viewport, so keep using
		# the parity helper that reconstructs Isaac's moving link-frame origin.
		key_positions.append(_vector_array(_policy_link_frame_position(body_name)))

	var joint_positions: Array = []
	for dof_name: String in _training_dof_order:
		joint_positions.append(_training_joint_position(dof_name))

	var packet: Dictionary = {
		"type": "step",
		"seq": _policy_sequence,
		"dt": POLICY_DT,
		"root_pos": _vector_array(pelvis.global_position),
		"root_quat": _quaternion_array(root_q),
		"root_lin_vel": _vector_array(pelvis.linear_velocity),
		"root_ang_vel": _vector_array(pelvis.angular_velocity),
		"key_body_pos": key_positions,
		"joint_pos": joint_positions,
		"foot_force_n": [
			_policy_foot_force("left_foot"),
			_policy_foot_force("right_foot"),
		],
	}
	_policy_sequence += 1
	_send_policy_packet(packet)


func _accept_policy_ready(packet: Dictionary) -> void:
	var names_variant: Variant = packet.get("dof_names", [])
	if not (names_variant is Array):
		_stop_policy_server()
		_set_policy_state("error", "Policy server did not provide canonical DOF names")
		return
	var names: Array = names_variant as Array
	if names.size() != _training_dof_order.size():
		_stop_policy_server()
		_set_policy_state("error", "Viewport/model 54-DOF count mismatch")
		return
	for index: int in range(_training_dof_order.size()):
		if String(names[index]) != _training_dof_order[index]:
			_stop_policy_server()
			_set_policy_state(
				"error",
				"DOF order mismatch %d: viewport=%s model=%s"
				% [index, _training_dof_order[index], String(names[index])],
			)
			return
	super._accept_policy_ready(packet)


func _configure_policy_drives(enabled: bool) -> void:
	if _training_dof_order.size() != TRAINING_DOF_COUNT:
		return
	for dof_index: int in range(_training_dof_order.size()):
		var dof_name: String = _training_dof_order[dof_index]
		var joint: Generic6DOFJoint3D = _training_dof_joints[dof_name] as Generic6DOFJoint3D
		var spec: Dictionary = _training_dof_specs[dof_name] as Dictionary
		var axis_name: String = String(spec["axis"])
		joint.set("angular_motor_%s/enabled" % axis_name, false)
		joint.set("angular_spring_%s/enabled" % axis_name, enabled)
		if _policy_stiffness.size() > dof_index:
			joint.set("angular_spring_%s/stiffness" % axis_name, _policy_stiffness[dof_index])
		if _policy_damping.size() > dof_index:
			joint.set("angular_spring_%s/damping" % axis_name, _policy_damping[dof_index])
		if _policy_effort.size() > dof_index:
			joint.set("angular_drive_%s/torque_limit" % axis_name, _policy_effort[dof_index])


func _apply_policy_targets(targets: Array) -> void:
	if targets.size() != TRAINING_DOF_COUNT:
		return
	for dof_index: int in range(_training_dof_order.size()):
		var dof_name: String = _training_dof_order[dof_index]
		var joint: Generic6DOFJoint3D = _training_dof_joints[dof_name] as Generic6DOFJoint3D
		var spec: Dictionary = _training_dof_specs[dof_name] as Dictionary
		var axis_name: String = String(spec["axis"])
		# Each constraint has only one free rotational coordinate, so the scalar
		# spring equilibrium is the direct Jolt analogue of the PhysX revolute
		# position target.
		joint.set(
			"angular_spring_%s/equilibrium_point" % axis_name,
			float(targets[dof_index]),
		)


func _reset_ragdoll() -> void:
	_reset_training_topology(_policy_active)


func _reset_policy_pose() -> void:
	_reset_training_topology(true)


func _reset_training_topology(policy_pose: bool) -> void:
	_release_grab()
	var vertical_shift: float = 0.0
	if policy_pose:
		vertical_shift = POLICY_ROOT_HEIGHT_M - float(Vector3(_initial_positions["pelvis"]).y)

	for body_name_variant: Variant in _bodies.keys():
		var body_name: String = String(body_name_variant)
		var body: RigidBody3D = _bodies[body_name] as RigidBody3D
		body.freeze = true
		body.global_position = Vector3(_initial_positions[body_name]) + Vector3(0.0, vertical_shift, 0.0)
		body.global_rotation = Vector3.ZERO
		body.linear_velocity = Vector3.ZERO
		body.angular_velocity = Vector3.ZERO

	for virtual_name_variant: Variant in _training_virtual_bodies.keys():
		var virtual_name: String = String(virtual_name_variant)
		var virtual_body: RigidBody3D = _training_virtual_bodies[virtual_name] as RigidBody3D
		var initial: Transform3D = _training_virtual_initial[virtual_name]
		initial.origin += Vector3(0.0, vertical_shift, 0.0)
		virtual_body.freeze = true
		virtual_body.global_transform = initial
		virtual_body.linear_velocity = Vector3.ZERO
		virtual_body.angular_velocity = Vector3.ZERO

	_restore_static_anatomical_ranges()

	for body_variant: Variant in _bodies.values():
		var body: RigidBody3D = body_variant as RigidBody3D
		if body != null:
			body.freeze = false
			body.sleeping = false
	for body_variant: Variant in _training_virtual_bodies.values():
		var virtual_body: RigidBody3D = body_variant as RigidBody3D
		if virtual_body != null:
			virtual_body.freeze = false
			virtual_body.sleeping = false

	for foot_name: String in ["left_foot", "right_foot"]:
		var foot: RigidBody3D = _bodies.get(foot_name) as RigidBody3D
		if foot != null and foot.get("contact_force_n") != null:
			foot.set("contact_force_n", 0.0)

	if policy_pose:
		_policy_last_response_ms = Time.get_ticks_msec()
		_send_policy_packet({"type": "reset"})
	else:
		var pelvis: RigidBody3D = _bodies.get("pelvis") as RigidBody3D
		if pelvis != null:
			pelvis.angular_velocity = Vector3(0.15, 0.05, 0.28)
	_update_overlay()


func _update_overlay() -> void:
	super._update_overlay()
	if _overlay_label == null:
		return
	_overlay_label.text = _overlay_label.text.replace(
		"19-BODY PASSIVE ANATOMICAL RAGDOLL / JOLT / 120 Hz",
		"TRAINING TOPOLOGY REPLICA / JOLT / %d Hz" % Engine.physics_ticks_per_second,
	)
	_overlay_label.text += "\n19 physical + 36 virtual links / 54 serial X-Y-Z revolute DOFs"
