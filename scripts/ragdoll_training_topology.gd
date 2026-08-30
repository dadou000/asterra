extends "res://scripts/ragdoll_policy_parity.gd"
## Viewport replica of the exact Stage-0A training articulation topology.
##
## This is still simulated by Godot/Jolt, not NVIDIA PhysX, but the graph matches
## the generated training asset: 19 physical links + 36 near-massless virtual
## coordinate-frame links + 54 serial X/Y/Z revolute coordinates.
##
## Unlike the compact 18x Generic6DOF ragdoll, each canonical policy DOF gets its
## own physical revolute constraint and spring drive, which makes this viewport a
## much closer plant match for the policy trained in Isaac/PhysX.

const CONTRACT_PATH: String = "res://experiments/locomotion_19body/config/physics_contract_19body.json"
const TRAINING_PHYSICAL_BODY_COUNT: int = 19
const TRAINING_VIRTUAL_BODY_COUNT: int = 36
const TRAINING_DOF_COUNT: int = 54
const TRAINING_VIRTUAL_MASS_KG: float = 0.0001
const TRAINING_VIRTUAL_INERTIA: Vector3 = Vector3(0.0000001, 0.0000001, 0.0000001)
const TRAINING_VIRTUAL_COLLIDER_RADIUS: float = 0.00001
const TRAINING_VIRTUAL_MARKER_RADIUS: float = 0.008
const TRAINING_AXES: Array[String] = ["x", "y", "z"]

var _training_contract: Dictionary = {}
var _training_virtual_bodies: Dictionary = {}
var _training_dof_joints: Dictionary = {}
var _training_dof_specs: Dictionary = {}
var _training_dof_order: Array[String] = []
var _training_dof_frame_a_local: Dictionary = {}
var _training_dof_frame_b_local: Dictionary = {}
var _training_initial_transforms: Dictionary = {}


func get_skeleton_mode() -> String:
	return "training"


func _build_ragdoll() -> void:
	_training_contract = _load_training_contract()
	var link_frames: Dictionary = _derive_source_link_frames(_training_contract)

	for body_variant: Variant in _training_contract.get("bodies", []):
		var body_spec: Dictionary = body_variant as Dictionary
		var body_name: String = String(body_spec.get("name", ""))
		if not link_frames.has(body_name):
			push_error("Training topology is missing link frame for %s" % body_name)
			continue
		_make_training_physical_body(body_spec, link_frames[body_name] as Transform3D)

	for joint_variant: Variant in _training_contract.get("joints", []):
		var joint_spec: Dictionary = joint_variant as Dictionary
		var joint_name: String = String(joint_spec.get("name", ""))
		var frame_transform: Transform3D = _source_joint_world_transform(joint_spec)
		_make_training_virtual_body("frame__%s_x" % joint_name, frame_transform, Color(0.95, 0.36, 0.65, 0.80))
		_make_training_virtual_body("frame__%s_y" % joint_name, frame_transform, Color(0.32, 0.86, 0.94, 0.80))

	# The Stage-0A contract has self collision disabled. Virtual frame links have
	# collision layer/mask zero; physical links explicitly ignore one another.
	var physical_names: Array = _bodies.keys()
	for first_index: int in range(physical_names.size()):
		for second_index: int in range(first_index + 1, physical_names.size()):
			var first: RigidBody3D = _bodies[physical_names[first_index]] as RigidBody3D
			var second: RigidBody3D = _bodies[physical_names[second_index]] as RigidBody3D
			if first != null and second != null:
				first.add_collision_exception_with(second)
				second.add_collision_exception_with(first)

	for joint_variant: Variant in _training_contract.get("joints", []):
		_make_training_joint_triplet(joint_variant as Dictionary)


func _load_training_contract() -> Dictionary:
	if not FileAccess.file_exists(CONTRACT_PATH):
		push_error("Missing training physics contract: %s" % CONTRACT_PATH)
		return {}
	var parsed: Variant = JSON.parse_string(FileAccess.get_file_as_string(CONTRACT_PATH))
	if not (parsed is Dictionary):
		push_error("Could not parse training physics contract: %s" % CONTRACT_PATH)
		return {}
	return parsed as Dictionary


func _derive_source_link_frames(contract: Dictionary) -> Dictionary:
	var result: Dictionary = {}
	var root_name: String = String(contract.get("root_body", "pelvis"))
	var root_center: Vector3 = Vector3.ZERO
	for body_variant: Variant in contract.get("bodies", []):
		var body_spec: Dictionary = body_variant as Dictionary
		if String(body_spec.get("name", "")) == root_name:
			root_center = _contract_vec3(body_spec.get("neutral_center_m", [0.0, 0.0, 0.0]))
			break
	result[root_name] = Transform3D(Basis.IDENTITY, root_center + SPAWN_OFFSET)

	# The contract is stored in topological order. In the generated URDF every
	# physical child link frame is located at its anatomical parent-joint anchor
	# and oriented to that joint's anatomical XYZ basis.
	for joint_variant: Variant in contract.get("joints", []):
		var joint_spec: Dictionary = joint_variant as Dictionary
		var child_name: String = String(joint_spec.get("child", ""))
		result[child_name] = _source_joint_world_transform(joint_spec)
	return result


func _source_joint_world_transform(joint_spec: Dictionary) -> Transform3D:
	var frame: Dictionary = joint_spec.get("frame", {}) as Dictionary
	var flex: Vector3 = _contract_vec3(frame.get("flex_axis_world", [1.0, 0.0, 0.0]))
	var twist: Vector3 = _contract_vec3(frame.get("twist_axis_world", [0.0, 1.0, 0.0]))
	var basis: Basis = _make_anatomical_basis(flex, twist)
	var anchor: Vector3 = _contract_vec3(joint_spec.get("anchor_m", [0.0, 0.0, 0.0])) + SPAWN_OFFSET
	return Transform3D(basis, anchor)


func _contract_vec3(value: Variant) -> Vector3:
	if not (value is Array):
		return Vector3.ZERO
	var items: Array = value as Array
	if items.size() != 3:
		return Vector3.ZERO
	return Vector3(float(items[0]), float(items[1]), float(items[2]))


func _make_training_physical_body(body_spec: Dictionary, link_transform: Transform3D) -> void:
	var body_name: String = String(body_spec.get("name", "unnamed"))
	var body: RigidBody3D = RigidBody3D.new()
	body.name = body_name
	body.mass = float(body_spec.get("mass_kg", 1.0))
	body.can_sleep = false
	body.continuous_cd = true
	body.linear_damp = float(_training_contract.get("damping", {}).get("linear", 0.04))
	body.angular_damp = float(_training_contract.get("damping", {}).get("angular", 0.08))
	body.freeze = true
	body.transform = link_transform
	add_child(body)

	var material_cfg: Dictionary = _training_contract.get("material", {}) as Dictionary
	var physics_material: PhysicsMaterial = PhysicsMaterial.new()
	physics_material.friction = float(material_cfg.get("body_friction", 0.75))
	physics_material.bounce = float(material_cfg.get("body_bounce", 0.01))
	body.physics_material_override = physics_material

	var body_center_world: Vector3 = _contract_vec3(body_spec.get("neutral_center_m", [0.0, 0.0, 0.0])) + SPAWN_OFFSET
	var local_center: Vector3 = link_transform.affine_inverse() * body_center_world
	# Physical bodies are neutral/world-axis aligned in the source contract, while
	# their URDF link frames are anatomical. Rotate the collision/visual back into
	# the link so the world-space geometry is exactly the same as the training body.
	var geometry_local: Transform3D = Transform3D(link_transform.basis.inverse(), local_center)
	var geometry: Dictionary = body_spec.get("geometry", {}) as Dictionary
	var kind: String = String(geometry.get("type", ""))

	var collision: CollisionShape3D = CollisionShape3D.new()
	var visual: MeshInstance3D = MeshInstance3D.new()
	collision.transform = geometry_local
	visual.transform = geometry_local

	if kind == "box":
		var size: Vector3 = _contract_vec3(geometry.get("size_m", [0.1, 0.1, 0.1]))
		var box_shape: BoxShape3D = BoxShape3D.new()
		box_shape.size = size
		collision.shape = box_shape
		var box_mesh: BoxMesh = BoxMesh.new()
		box_mesh.size = size
		visual.mesh = box_mesh
	elif kind == "capsule":
		var radius: float = float(geometry.get("radius_m", 0.05))
		var height: float = float(geometry.get("height_m", 0.2))
		var capsule_shape: CapsuleShape3D = CapsuleShape3D.new()
		capsule_shape.radius = radius
		capsule_shape.height = height
		collision.shape = capsule_shape
		var capsule_mesh: CapsuleMesh = CapsuleMesh.new()
		capsule_mesh.radius = radius
		capsule_mesh.height = height
		visual.mesh = capsule_mesh
	elif kind == "sphere":
		var radius: float = float(geometry.get("radius_m", 0.1))
		var sphere_shape: SphereShape3D = SphereShape3D.new()
		sphere_shape.radius = radius
		collision.shape = sphere_shape
		var sphere_mesh: SphereMesh = SphereMesh.new()
		sphere_mesh.radius = radius
		sphere_mesh.height = radius * 2.0
		visual.mesh = sphere_mesh
	else:
		push_error("Unsupported training body geometry %s for %s" % [kind, body_name])
		body.queue_free()
		return

	body.add_child(collision)
	body.add_child(visual)
	var visual_material: StandardMaterial3D = StandardMaterial3D.new()
	visual_material.albedo_color = _body_color(body_name)
	visual_material.metallic = 0.08
	visual_material.roughness = 0.48
	visual.material_override = visual_material

	_bodies[body_name] = body
	_initial_positions[body_name] = link_transform.origin
	_training_initial_transforms[body_name] = link_transform


func _make_training_virtual_body(name: String, body_transform: Transform3D, marker_color: Color) -> void:
	var body: RigidBody3D = RigidBody3D.new()
	body.name = name
	body.mass = TRAINING_VIRTUAL_MASS_KG
	body.inertia = TRAINING_VIRTUAL_INERTIA
	body.can_sleep = false
	body.freeze = true
	body.collision_layer = 0
	body.collision_mask = 0
	body.transform = body_transform
	add_child(body)

	# Give Jolt a real inertia-bearing shape while keeping it completely outside
	# collision filtering. This mirrors the microscopic import-anchor geometry in
	# the generated training asset closely enough to keep the virtual body valid.
	var collision: CollisionShape3D = CollisionShape3D.new()
	var shape: SphereShape3D = SphereShape3D.new()
	shape.radius = TRAINING_VIRTUAL_COLLIDER_RADIUS
	collision.shape = shape
	body.add_child(collision)

	# Visible marker is deliberately larger than the real 10 µm training anchor so
	# the user can see where the two serial coordinate-frame links live.
	var marker: MeshInstance3D = MeshInstance3D.new()
	var marker_mesh: SphereMesh = SphereMesh.new()
	marker_mesh.radius = TRAINING_VIRTUAL_MARKER_RADIUS
	marker_mesh.height = TRAINING_VIRTUAL_MARKER_RADIUS * 2.0
	marker.mesh = marker_mesh
	var marker_material: StandardMaterial3D = StandardMaterial3D.new()
	marker_material.albedo_color = marker_color
	marker_material.emission_enabled = true
	marker_material.emission = Color(marker_color.r, marker_color.g, marker_color.b)
	marker_material.emission_energy_multiplier = 0.7
	marker_material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	marker.material_override = marker_material
	body.add_child(marker)

	_training_virtual_bodies[name] = body
	_training_initial_transforms[name] = body_transform


func _make_training_joint_triplet(joint_spec: Dictionary) -> void:
	var joint_name: String = String(joint_spec.get("name", ""))
	var parent_name: String = String(joint_spec.get("parent", ""))
	var child_name: String = String(joint_spec.get("child", ""))
	var parent_body: RigidBody3D = _bodies.get(parent_name) as RigidBody3D
	var child_body: RigidBody3D = _bodies.get(child_name) as RigidBody3D
	var frame_x: RigidBody3D = _training_virtual_bodies.get("frame__%s_x" % joint_name) as RigidBody3D
	var frame_y: RigidBody3D = _training_virtual_bodies.get("frame__%s_y" % joint_name) as RigidBody3D
	if parent_body == null or child_body == null or frame_x == null or frame_y == null:
		push_error("Could not build training joint chain for %s" % joint_name)
		return

	var frame_transform: Transform3D = _source_joint_world_transform(joint_spec)
	var limits: Dictionary = joint_spec.get("limits_deg", {}) as Dictionary
	var actuator: Dictionary = joint_spec.get("actuator", {}) as Dictionary
	var effort: Array = actuator.get("effort_limit_nm", []) as Array
	var stiffness: Array = actuator.get("stiffness_nm_per_rad", []) as Array
	var damping: Array = actuator.get("damping_nms_per_rad", []) as Array

	var chain_parents: Array[RigidBody3D] = [parent_body, frame_x, frame_y]
	var chain_children: Array[RigidBody3D] = [frame_x, frame_y, child_body]
	for axis_index: int in range(3):
		var axis_name: String = TRAINING_AXES[axis_index]
		var range_variant: Variant = limits.get(axis_name, [0.0, 0.0])
		var range_values: Array = range_variant as Array if range_variant is Array else [0.0, 0.0]
		var dof_name: String = "%s_%s" % [joint_name, axis_name]
		_make_training_revolute(
			dof_name,
			joint_name,
			chain_parents[axis_index],
			chain_children[axis_index],
			frame_transform,
			axis_name,
			float(range_values[0]),
			float(range_values[1]),
			float(effort[axis_index]) if effort.size() > axis_index else 0.0,
			float(stiffness[axis_index]) if stiffness.size() > axis_index else 0.0,
			float(damping[axis_index]) if damping.size() > axis_index else 0.0,
		)

	# Compatibility map used by the policy loader: every anatomical name must
	# exist, but actual driving/state lives in _training_dof_joints.
	_joints[joint_name] = _training_dof_joints["%s_x" % joint_name]
	_joint_specs_by_name[joint_name] = {
		"name": joint_name,
		"parent": parent_name,
		"child": child_name,
		"anchor": _contract_vec3(joint_spec.get("anchor_m", [0.0, 0.0, 0.0])),
	}


func _make_training_revolute(
	dof_name: String,
	anatomical_joint: String,
	parent_body: RigidBody3D,
	child_body: RigidBody3D,
	frame_transform: Transform3D,
	active_axis: String,
	lower_deg: float,
	upper_deg: float,
	effort_limit: float,
	stiffness: float,
	damping: float,
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
		"effort_limit": effort_limit,
		"stiffness": stiffness,
		"damping": damping,
		"parent_body": parent_body,
		"child_body": child_body,
	}
	_joint_count += 1


func _assert_articulation_contract() -> void:
	assert(_bodies.size() == TRAINING_PHYSICAL_BODY_COUNT, "Training topology must have 19 physical bodies")
	assert(_training_virtual_bodies.size() == TRAINING_VIRTUAL_BODY_COUNT, "Training topology must have 36 virtual links")
	assert(_training_dof_joints.size() == TRAINING_DOF_COUNT, "Training topology must have 54 revolute coordinates")
	assert(_training_dof_order.size() == TRAINING_DOF_COUNT, "Training topology canonical DOF order is incomplete")
	assert(_joints.size() == 18, "Training topology must expose 18 anatomical joint groups")
	print(
		"Asterra training-topology viewport ready: 19 physical + 36 virtual links, 54 serial revolute DOFs"
	)
	print("Topology: parent -> X -> frame_x -> Y -> frame_y -> Z -> child")


func _apply_coupled_limits() -> void:
	# Deliberately no dynamic hard-limit tightening here. Isaac/PhysX keeps the
	# generated outer hard ROM static; CanonicalActionProcessor applies the
	# posture-dependent coupling to commanded targets. Match that behavior.
	pass


func _restore_static_anatomical_ranges() -> void:
	for dof_name_variant: Variant in _training_dof_order:
		var dof_name: String = String(dof_name_variant)
		var joint: Generic6DOFJoint3D = _training_dof_joints[dof_name] as Generic6DOFJoint3D
		var spec: Dictionary = _training_dof_specs[dof_name] as Dictionary
		var active_axis: String = String(spec["axis"])
		for axis_name: String in TRAINING_AXES:
			if axis_name != active_axis:
				joint.set("angular_limit_%s/enabled" % axis_name, true)
				_set_angular_range(joint, axis_name, 0.0, 0.0)
				continue
			var active_enabled: bool = _anatomical_limits_enabled
			joint.set("angular_limit_%s/enabled" % axis_name, active_enabled)
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
	# This rig loads physics_contract_19body.json directly, so there is no
	# hand-written ROM copy to synchronize.
	pass


func _training_joint_position(dof_name: String) -> float:
	if not _training_dof_specs.has(dof_name):
		return 0.0
	var spec: Dictionary = _training_dof_specs[dof_name] as Dictionary
	var parent_body: RigidBody3D = spec["parent_body"] as RigidBody3D
	var child_body: RigidBody3D = spec["child_body"] as RigidBody3D
	if parent_body == null or child_body == null:
		return 0.0
	var frame_a_local: Basis = _training_dof_frame_a_local[dof_name] as Basis
	var frame_b_local: Basis = _training_dof_frame_b_local[dof_name] as Basis
	var frame_a_world: Basis = (parent_body.global_transform.basis * frame_a_local).orthonormalized()
	var frame_b_world: Basis = (child_body.global_transform.basis * frame_b_local).orthonormalized()
	var relative: Basis = (frame_a_world.inverse() * frame_b_world).orthonormalized()
	var q: Quaternion = relative.get_rotation_quaternion().normalized()
	var axis_name: String = String(spec["axis"])
	var axis_vector: Vector3 = Vector3.RIGHT
	if axis_name == "y":
		axis_vector = Vector3.UP
	elif axis_name == "z":
		axis_vector = Vector3(0.0, 0.0, 1.0)
	var vector_part: Vector3 = Vector3(q.x, q.y, q.z)
	var angle: float = 2.0 * atan2(vector_part.dot(axis_vector), q.w)
	return wrapf(angle, -PI, PI)


func _send_policy_step() -> void:
	if _policy_peer == null or _policy_joint_order.size() != 18 or _training_dof_order.size() != 54:
		return
	var pelvis: RigidBody3D = _bodies.get("pelvis") as RigidBody3D
	if pelvis == null:
		return

	var root_q: Quaternion = pelvis.global_transform.basis.get_rotation_quaternion().normalized()
	var key_positions: Array = []
	for body_name: String in POLICY_KEY_BODIES:
		var body: RigidBody3D = _bodies.get(body_name) as RigidBody3D
		key_positions.append(_vector_array(body.global_position if body != null else Vector3.ZERO))

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
	var dof_names_variant: Variant = packet.get("dof_names", [])
	if not (dof_names_variant is Array):
		_stop_policy_server()
		_set_policy_state("error", "Policy server did not provide canonical DOF names")
		return
	var dof_names: Array = dof_names_variant as Array
	if dof_names.size() != _training_dof_order.size():
		_stop_policy_server()
		_set_policy_state("error", "Training-topology DOF count does not match the checkpoint ABI")
		return
	for index: int in range(_training_dof_order.size()):
		if String(dof_names[index]) != _training_dof_order[index]:
			_stop_policy_server()
			_set_policy_state(
				"error",
				"Canonical DOF order mismatch at %d: viewport=%s model=%s"
				% [index, _training_dof_order[index], String(dof_names[index])],
			)
			return
	super._accept_policy_ready(packet)


func _configure_policy_drives(enabled: bool) -> void:
	if _training_dof_order.size() != 54:
		return
	for dof_index: int in range(_training_dof_order.size()):
		var dof_name: String = _training_dof_order[dof_index]
		var joint: Generic6DOFJoint3D = _training_dof_joints[dof_name] as Generic6DOFJoint3D
		var spec: Dictionary = _training_dof_specs[dof_name] as Dictionary
		if joint == null:
			continue
		var axis_name: String = String(spec["axis"])
		joint.set("angular_motor_%s/enabled" % axis_name, false)
		joint.set("angular_spring_%s/enabled" % axis_name, enabled)
		var stiffness: float = _policy_stiffness[dof_index] if _policy_stiffness.size() > dof_index else float(spec["stiffness"])
		var damping: float = _policy_damping[dof_index] if _policy_damping.size() > dof_index else float(spec["damping"])
		var effort: float = _policy_effort[dof_index] if _policy_effort.size() > dof_index else float(spec["effort_limit"])
		joint.set("angular_spring_%s/stiffness" % axis_name, stiffness)
		joint.set("angular_spring_%s/damping" % axis_name, damping)
		joint.set("angular_drive_%s/torque_limit" % axis_name, effort)


func _apply_policy_targets(targets: Array) -> void:
	if targets.size() != 54:
		return
	for dof_index: int in range(_training_dof_order.size()):
		var dof_name: String = _training_dof_order[dof_index]
		var joint: Generic6DOFJoint3D = _training_dof_joints[dof_name] as Generic6DOFJoint3D
		var spec: Dictionary = _training_dof_specs[dof_name] as Dictionary
		if joint == null:
			continue
		var axis_name: String = String(spec["axis"])
		# Every constraint has exactly one free rotational coordinate, so the
		# scalar equilibrium point is the direct equivalent of the PhysX revolute
		# position target. No 3-axis quaternion reconstruction is involved.
		joint.set("angular_spring_%s/equilibrium_point" % axis_name, float(targets[dof_index]))


func _reset_ragdoll() -> void:
	if _policy_active:
		_reset_training_topology(true)
	else:
		_reset_training_topology(false)


func _reset_policy_pose() -> void:
	_reset_training_topology(true)


func _reset_training_topology(policy_pose: bool) -> void:
	_release_grab()
	var pelvis_initial: Transform3D = _training_initial_transforms.get("pelvis", Transform3D.IDENTITY) as Transform3D
	var vertical_shift: float = 0.0
	if policy_pose:
		vertical_shift = POLICY_ROOT_HEIGHT_M - pelvis_initial.origin.y
	var shift: Vector3 = Vector3(0.0, vertical_shift, 0.0)

	var all_bodies: Array[RigidBody3D] = []
	for body_variant: Variant in _bodies.values():
		var physical: RigidBody3D = body_variant as RigidBody3D
		if physical != null:
			all_bodies.append(physical)
	for body_variant: Variant in _training_virtual_bodies.values():
		var virtual_body: RigidBody3D = body_variant as RigidBody3D
		if virtual_body != null:
			all_bodies.append(virtual_body)

	for body: RigidBody3D in all_bodies:
		body.freeze = true
		var initial: Transform3D = _training_initial_transforms.get(String(body.name), body.global_transform) as Transform3D
		initial.origin += shift
		body.global_transform = initial
		body.linear_velocity = Vector3.ZERO
		body.angular_velocity = Vector3.ZERO

	_restore_static_anatomical_ranges()

	for body: RigidBody3D in all_bodies:
		body.freeze = false
		body.sleeping = false

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
	var rate: int = Engine.physics_ticks_per_second
	_overlay_label.text = _overlay_label.text.replace(
		"19-BODY PASSIVE ANATOMICAL RAGDOLL / JOLT / 120 Hz",
		"TRAINING TOPOLOGY REPLICA / JOLT / %d Hz" % rate,
	)
	_overlay_label.text += "\nTraining graph: 19 physical + 36 virtual links / 54 serial revolute DOFs"
