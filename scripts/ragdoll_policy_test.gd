extends "res://scripts/ragdoll_test.gd"
## Policy-enabled version of the isolated 19-body Jolt ragdoll.
##
## A trained RSL-RL checkpoint runs in a lightweight local Python process. Godot
## sends physical state at 60 Hz and receives 54 canonical target angles. Those
## targets feed Jolt angular spring drives with the same stiffness, damping and
## torque caps as the training articulation. No body transform is written by the
## neural controller; Jolt remains authoritative.

signal policy_state_changed

const POLICY_HZ: float = 60.0
const POLICY_DT: float = 1.0 / POLICY_HZ
const POLICY_ROOT_HEIGHT_M: float = 0.945
const POLICY_READY_TIMEOUT_MS: int = 12000
const POLICY_RESPONSE_TIMEOUT_MS: int = 750
const POLICY_PING_INTERVAL_MS: int = 250
const POLICY_BODY_SCRIPT: Script = preload("res://scripts/ragdoll_policy_body.gd")
const POLICY_KEY_BODIES: Array[String] = [
	"head", "left_hand", "right_hand", "left_foot", "right_foot"
]
const POLICY_AXES: Array[String] = ["x", "y", "z"]

var _repo_root: String = ""
var _policy_server_path: String = ""
var _policy_status_file: String = ""
var _policy_python: String = ""

var _policy_peer: PacketPeerUDP
var _policy_server_pid: int = 0
var _policy_server_port: int = 0
var _policy_checkpoint_path: String = ""
var _policy_model_iteration: int = -1
var _policy_state: String = "passive"
var _policy_detail: String = "No model loaded"
var _policy_loaded: bool = false
var _policy_active: bool = false
var _policy_server_started_ms: int = 0
var _policy_last_ping_ms: int = 0
var _policy_last_response_ms: int = 0
var _policy_accumulator: float = 0.0
var _policy_sequence: int = 0
var _policy_last_applied_sequence: int = -1
var _policy_latency_ms: float = 0.0

var _policy_joint_order: Array[String] = []
var _policy_stiffness: PackedFloat32Array = PackedFloat32Array()
var _policy_damping: PackedFloat32Array = PackedFloat32Array()
var _policy_effort: PackedFloat32Array = PackedFloat32Array()


func _ready() -> void:
	super._ready()
	_repo_root = ProjectSettings.globalize_path("res://").trim_suffix("/").trim_suffix("\\")
	_policy_server_path = _repo_root.path_join("experiments").path_join("locomotion_19body").path_join("training").path_join("scripts").path_join("policy_inference_server.py")
	_policy_status_file = _repo_root.path_join("experiments").path_join("locomotion_19body").path_join("runs").path_join("control").path_join("viewport_policy_status.json")
	_policy_python = _default_policy_python()
	_enable_policy_contact_sampling()
	_update_overlay()


func _physics_process(delta: float) -> void:
	super._physics_process(delta)
	_poll_policy_packets()
	_update_policy_process_health()

	if not _policy_loaded:
		_ping_loading_server_if_needed()
		return
	if not _policy_active:
		return

	var now_ms: int = Time.get_ticks_msec()
	if _policy_last_response_ms > 0 and now_ms - _policy_last_response_ms > POLICY_RESPONSE_TIMEOUT_MS:
		_policy_fail_safe("Policy response timed out; neural drives were disabled")
		return

	_policy_accumulator += delta
	while _policy_accumulator >= POLICY_DT:
		_policy_accumulator -= POLICY_DT
		_send_policy_step()


func _exit_tree() -> void:
	_stop_policy_server()


func _reset_ragdoll() -> void:
	if _policy_active and not _bodies.is_empty():
		_reset_policy_pose()
		return
	super._reset_ragdoll()


func _update_overlay() -> void:
	super._update_overlay()
	if _overlay_label == null:
		return
	var model_name: String = _policy_checkpoint_path.get_file() if not _policy_checkpoint_path.is_empty() else "none"
	var active_text: String = "ACTIVE" if _policy_active else _policy_state.to_upper()
	_overlay_label.text += "\nPolicy: %s   model: %s   inference: %.2f ms" % [
		active_text,
		model_name,
		_policy_latency_ms,
	]


func load_policy_checkpoint(checkpoint_path: String) -> void:
	var checkpoint: String = checkpoint_path.strip_edges()
	if checkpoint.is_empty() or not FileAccess.file_exists(checkpoint):
		_set_policy_state("error", "Checkpoint file does not exist")
		return
	if not FileAccess.file_exists(_policy_server_path):
		_set_policy_state("error", "Missing policy_inference_server.py")
		return
	_policy_python = _default_policy_python()
	if not FileAccess.file_exists(_policy_python):
		_set_policy_state("error", "Training Python is missing. Use F3 → Install / repair training tools first.")
		return

	_stop_policy_server()
	_policy_checkpoint_path = checkpoint
	_policy_model_iteration = -1
	_policy_loaded = false
	_policy_active = false
	_policy_joint_order.clear()
	_policy_stiffness = PackedFloat32Array()
	_policy_damping = PackedFloat32Array()
	_policy_effort = PackedFloat32Array()
	_policy_sequence = 0
	_policy_last_applied_sequence = -1
	_policy_latency_ms = 0.0

	if FileAccess.file_exists(_policy_status_file):
		DirAccess.remove_absolute(_policy_status_file)

	_policy_peer = PacketPeerUDP.new()
	var bind_error: Error = _policy_peer.bind(0, "127.0.0.1", 65536)
	if bind_error != OK:
		_policy_peer = null
		_set_policy_state("error", "Could not open local policy IPC socket")
		return

	_policy_server_port = randi_range(24000, 52000)
	var connect_error: Error = _policy_peer.connect_to_host("127.0.0.1", _policy_server_port)
	if connect_error != OK:
		_policy_peer.close()
		_policy_peer = null
		_set_policy_state("error", "Could not connect local policy IPC socket")
		return

	var arguments: PackedStringArray = PackedStringArray([
		_policy_server_path,
		"--checkpoint", checkpoint,
		"--port", str(_policy_server_port),
		"--device", "cpu",
		"--status-file", _policy_status_file,
	])
	_policy_server_pid = OS.create_process(_policy_python, arguments, false)
	if _policy_server_pid <= 0:
		_policy_peer.close()
		_policy_peer = null
		_set_policy_state("error", "Could not start the local policy process")
		return

	_policy_server_started_ms = Time.get_ticks_msec()
	_policy_last_ping_ms = 0
	_policy_last_response_ms = 0
	_set_policy_state("loading", "Loading %s..." % checkpoint.get_file())
	_send_policy_packet({"type": "ping"})


func activate_loaded_policy() -> void:
	if not _policy_loaded:
		_set_policy_state("error", "Load a model and wait for READY before activating it")
		return
	if _policy_active:
		return
	_configure_policy_drives(true)
	_policy_active = true
	_policy_accumulator = 0.0
	_policy_last_response_ms = Time.get_ticks_msec()
	_send_policy_packet({"type": "reset"})
	_reset_policy_pose()
	_set_policy_state("active", "Neural controller is driving the physical Jolt ragdoll")


func deactivate_policy() -> void:
	if not _policy_active:
		return
	_policy_active = false
	_policy_accumulator = 0.0
	_configure_policy_drives(false)
	_set_policy_state("ready", "Model stays loaded; ragdoll is passive")


func unload_policy() -> void:
	_stop_policy_server()
	_policy_checkpoint_path = ""
	_policy_model_iteration = -1
	_policy_loaded = false
	_policy_active = false
	_set_policy_state("passive", "No model loaded")


func get_policy_ui_state() -> Dictionary:
	return {
		"state": _policy_state,
		"detail": _policy_detail,
		"loaded": _policy_loaded,
		"active": _policy_active,
		"checkpoint": _policy_checkpoint_path,
		"model_iteration": _policy_model_iteration,
		"latency_ms": _policy_latency_ms,
		"server_pid": _policy_server_pid,
	}


func _default_policy_python() -> String:
	var root: String = _repo_root
	if root.is_empty():
		root = ProjectSettings.globalize_path("res://").trim_suffix("/").trim_suffix("\\")
	var windows_python: String = root.path_join(".venv-isaac").path_join("Scripts").path_join("python.exe")
	var unix_python: String = root.path_join(".venv-isaac").path_join("bin").path_join("python")
	if FileAccess.file_exists(windows_python):
		return windows_python
	if FileAccess.file_exists(unix_python):
		return unix_python
	return windows_python if OS.get_name() == "Windows" else unix_python


func _enable_policy_contact_sampling() -> void:
	for foot_name: String in ["left_foot", "right_foot"]:
		if not _bodies.has(foot_name):
			continue
		var body: RigidBody3D = _bodies[foot_name] as RigidBody3D
		if body == null:
			continue
		body.set_script(POLICY_BODY_SCRIPT)
		body.contact_monitor = true
		body.max_contacts_reported = 8


func _ping_loading_server_if_needed() -> void:
	if _policy_server_pid <= 0 or _policy_peer == null or _policy_state != "loading":
		return
	var now_ms: int = Time.get_ticks_msec()
	if now_ms - _policy_server_started_ms > POLICY_READY_TIMEOUT_MS:
		var failure_detail: String = _read_policy_status_detail()
		if failure_detail.is_empty():
			failure_detail = "Timed out while loading the checkpoint"
		_stop_policy_server()
		_set_policy_state("error", failure_detail)
		return
	if now_ms - _policy_last_ping_ms >= POLICY_PING_INTERVAL_MS:
		_policy_last_ping_ms = now_ms
		_send_policy_packet({"type": "ping"})


func _poll_policy_packets() -> void:
	if _policy_peer == null:
		return
	while _policy_peer.get_available_packet_count() > 0:
		var bytes: PackedByteArray = _policy_peer.get_packet()
		var text: String = bytes.get_string_from_utf8()
		var parsed: Variant = JSON.parse_string(text)
		if not (parsed is Dictionary):
			continue
		var packet: Dictionary = parsed as Dictionary
		var packet_type: String = String(packet.get("type", ""))
		if packet_type == "ready":
			_accept_policy_ready(packet)
		elif packet_type == "action":
			_accept_policy_action(packet)
		elif packet_type == "error":
			_set_policy_state("error", String(packet.get("error", "Policy server error")))


func _accept_policy_ready(packet: Dictionary) -> void:
	if _policy_loaded:
		return
	var obs_size: int = int(packet.get("observation_size", -1))
	var action_size: int = int(packet.get("action_size", -1))
	if obs_size != 197 or action_size != 54:
		_stop_policy_server()
		_set_policy_state("error", "Model ABI mismatch: expected 197 observations / 54 actions")
		return

	var names_variant: Variant = packet.get("joint_names", [])
	var stiffness_variant: Variant = packet.get("stiffness", [])
	var damping_variant: Variant = packet.get("damping", [])
	var effort_variant: Variant = packet.get("effort_limit", [])
	if not (names_variant is Array) or not (stiffness_variant is Array) or not (damping_variant is Array) or not (effort_variant is Array):
		_stop_policy_server()
		_set_policy_state("error", "Policy server returned malformed drive metadata")
		return
	var names: Array = names_variant as Array
	var stiffness: Array = stiffness_variant as Array
	var damping: Array = damping_variant as Array
	var effort: Array = effort_variant as Array
	if names.size() != 18 or stiffness.size() != 54 or damping.size() != 54 or effort.size() != 54:
		_stop_policy_server()
		_set_policy_state("error", "Policy drive metadata has the wrong size")
		return

	_policy_joint_order.clear()
	for name_variant: Variant in names:
		var joint_name: String = String(name_variant)
		if not _joints.has(joint_name):
			_stop_policy_server()
			_set_policy_state("error", "Viewport ragdoll is missing policy joint %s" % joint_name)
			return
		_policy_joint_order.append(joint_name)

	_policy_stiffness = PackedFloat32Array()
	_policy_damping = PackedFloat32Array()
	_policy_effort = PackedFloat32Array()
	for index: int in range(54):
		_policy_stiffness.append(float(stiffness[index]))
		_policy_damping.append(float(damping[index]))
		_policy_effort.append(float(effort[index]))

	_policy_model_iteration = int(packet.get("model_iteration", -1))
	_policy_loaded = true
	_policy_last_response_ms = Time.get_ticks_msec()
	_configure_policy_drives(false)
	_set_policy_state("ready", "Model loaded. Press ACTIVATE to let it control the ragdoll.")


func _accept_policy_action(packet: Dictionary) -> void:
	if not _policy_active:
		return
	var sequence: int = int(packet.get("seq", -1))
	if sequence < _policy_last_applied_sequence:
		return
	var target_variant: Variant = packet.get("target", [])
	if not (target_variant is Array):
		return
	var targets: Array = target_variant as Array
	if targets.size() != 54:
		_policy_fail_safe("Policy returned an invalid 54-DOF target")
		return
	_policy_last_applied_sequence = sequence
	_policy_last_response_ms = Time.get_ticks_msec()
	_policy_latency_ms = float(packet.get("latency_ms", 0.0))
	_apply_policy_targets(targets)
	_update_overlay()


func _send_policy_step() -> void:
	if _policy_peer == null or _policy_joint_order.size() != 18:
		return
	var pelvis: RigidBody3D = _bodies.get("pelvis") as RigidBody3D
	if pelvis == null:
		return

	var root_q: Quaternion = pelvis.global_transform.basis.get_rotation_quaternion().normalized()
	var key_positions: Array = []
	for body_name: String in POLICY_KEY_BODIES:
		var body: RigidBody3D = _bodies[body_name] as RigidBody3D
		key_positions.append(_vector_array(body.global_position))

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


func _send_policy_packet(packet: Dictionary) -> void:
	if _policy_peer == null:
		return
	var payload: PackedByteArray = JSON.stringify(packet).to_utf8_buffer()
	_policy_peer.put_packet(payload)


func _vector_array(value: Vector3) -> Array:
	return [value.x, value.y, value.z]


func _quaternion_array(value: Quaternion) -> Array:
	return [value.x, value.y, value.z, value.w]


func _policy_foot_force(body_name: String) -> float:
	if not _bodies.has(body_name):
		return 0.0
	var body: RigidBody3D = _bodies[body_name] as RigidBody3D
	if body == null:
		return 0.0
	var value: Variant = body.get("contact_force_n")
	return float(value) if value != null else 0.0


func _configure_policy_drives(enabled: bool) -> void:
	if _policy_joint_order.size() != 18:
		return
	for joint_index: int in range(_policy_joint_order.size()):
		var joint_name: String = _policy_joint_order[joint_index]
		var joint: Generic6DOFJoint3D = _joints[joint_name] as Generic6DOFJoint3D
		if joint == null:
			continue
		for axis_index: int in range(3):
			var axis: String = POLICY_AXES[axis_index]
			var dof_index: int = joint_index * 3 + axis_index
			joint.set("angular_motor_%s/enabled" % axis, false)
			joint.set("angular_spring_%s/enabled" % axis, enabled)
			joint.set("angular_spring_%s/stiffness" % axis, _policy_stiffness[dof_index])
			joint.set("angular_spring_%s/damping" % axis, _policy_damping[dof_index])
			# Jolt applies this as a torque cap in both position-spring and motor mode.
			joint.set("angular_drive_%s/torque_limit" % axis, _policy_effort[dof_index])
		if not enabled and joint.has_method("clear_angular_target_rotation"):
			joint.call("clear_angular_target_rotation")


func _apply_policy_targets(targets: Array) -> void:
	for joint_index: int in range(_policy_joint_order.size()):
		var joint_name: String = _policy_joint_order[joint_index]
		var joint: Generic6DOFJoint3D = _joints[joint_name] as Generic6DOFJoint3D
		if joint == null:
			continue
		var base_index: int = joint_index * 3
		var target: Vector3 = Vector3(
			float(targets[base_index]),
			float(targets[base_index + 1]),
			float(targets[base_index + 2])
		)

		if joint.has_method("set_angular_target_rotation"):
			# Training representation is Rx(qx) * Ry(qy) * Rz(qz). Convert the
			# desired anatomical-frame rotation into body-B-relative-to-body-A
			# space because that is what Jolt's quaternion target consumes.
			var desired_joint: Basis = (
				Basis(Quaternion(Vector3.RIGHT, target.x))
				* Basis(Quaternion(Vector3.UP, target.y))
				* Basis(Quaternion(Vector3(0.0, 0.0, 1.0), target.z))
			).orthonormalized()
			var frame_a: Basis = _joint_frame_a_local[joint_name]
			var frame_b: Basis = _joint_frame_b_local[joint_name]
			var body_target: Basis = (frame_a * desired_joint * frame_b.inverse()).orthonormalized()
			joint.call("set_angular_target_rotation", body_target.get_rotation_quaternion().normalized())
		else:
			# Compatibility fallback for an older Jolt build. The equilibrium
			# properties are still physical position-spring targets.
			joint.set("angular_spring_x/equilibrium_point", target.x)
			joint.set("angular_spring_y/equilibrium_point", target.y)
			joint.set("angular_spring_z/equilibrium_point", target.z)


func _reset_policy_pose() -> void:
	_release_grab()
	var pelvis_initial: Vector3 = Vector3(_initial_positions["pelvis"])
	var policy_offset: Vector3 = Vector3(0.0, POLICY_ROOT_HEIGHT_M - pelvis_initial.y, 0.0)

	for body_name_variant: Variant in _bodies.keys():
		var body_name: String = String(body_name_variant)
		var body: RigidBody3D = _bodies[body_name] as RigidBody3D
		body.freeze = true
		body.global_position = Vector3(_initial_positions[body_name]) + policy_offset
		body.global_rotation = Vector3.ZERO
		body.linear_velocity = Vector3.ZERO
		body.angular_velocity = Vector3.ZERO
		if body_name == "left_foot" or body_name == "right_foot":
			body.set("contact_force_n", 0.0)

	if _anatomical_limits_enabled:
		_restore_static_anatomical_ranges()
		_apply_coupled_limits()

	for body_name_variant: Variant in _bodies.keys():
		var body_name: String = String(body_name_variant)
		var body: RigidBody3D = _bodies[body_name] as RigidBody3D
		body.freeze = false
		body.sleeping = false

	_policy_last_response_ms = Time.get_ticks_msec()
	_send_policy_packet({"type": "reset"})
	_update_overlay()


func _update_policy_process_health() -> void:
	if _policy_server_pid <= 0:
		return
	if OS.is_process_running(_policy_server_pid):
		return
	var detail: String = _read_policy_status_detail()
	if detail.is_empty():
		detail = "Policy process exited unexpectedly"
	_policy_server_pid = 0
	if _policy_active:
		_policy_fail_safe(detail)
	elif not _policy_loaded:
		_set_policy_state("error", detail)


func _read_policy_status_detail() -> String:
	if not FileAccess.file_exists(_policy_status_file):
		return ""
	var parsed: Variant = JSON.parse_string(FileAccess.get_file_as_string(_policy_status_file))
	if not (parsed is Dictionary):
		return ""
	var data: Dictionary = parsed as Dictionary
	return String(data.get("detail", ""))


func _policy_fail_safe(detail: String) -> void:
	_policy_active = false
	_configure_policy_drives(false)
	_set_policy_state("error", detail)


func _stop_policy_server() -> void:
	if _policy_active:
		_policy_active = false
		_configure_policy_drives(false)
	if _policy_peer != null:
		if _policy_server_pid > 0:
			_send_policy_packet({"type": "quit"})
		_policy_peer.close()
		_policy_peer = null
	if _policy_server_pid > 0 and OS.is_process_running(_policy_server_pid):
		OS.kill(_policy_server_pid)
	_policy_server_pid = 0
	_policy_loaded = false


func _set_policy_state(state: String, detail: String) -> void:
	_policy_state = state
	_policy_detail = detail
	_update_overlay()
	policy_state_changed.emit()
