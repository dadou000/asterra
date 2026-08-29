class_name PhysicsWalkerVisual
extends Node3D
## Procedural visual locomotion for the experimental physics walker.
##
## The authoritative motion still comes entirely from PhysicsWalkerBody. This node
## only poses the imported skinned skeleton so the character no longer remains in
## its bind/T pose. No canned walk animation is required: cadence and pose amplitude
## are derived from rigid-body velocity/contact state.

const CHARACTER_PATH := "res://assets/character/asterrahuman.glb"
const WALK_REFERENCE_SPEED := 4.6
const RUN_REFERENCE_SPEED := 11.0
const ARM_LOWER_DEG := 76.0

@export var body: PhysicsWalkerBody
@export var visible_character := true
@export var procedural_gait_enabled := true
@export_range(0.0, 1.0, 0.01) var gait_strength := 1.0

var character: Node3D
var skeleton: Skeleton3D
var model_forward_correction_deg := 0.0
var model_height := 1.75

var _bones: Dictionary = {}
var _gait_phase := 0.0
var _gait_blend := 0.0
var _mapping_reported := false


func _ready() -> void:
	_load_character()


func _process(dt: float) -> void:
	if body == null or character == null:
		return
	visible = visible_character and body.active
	if not body.active:
		return
	global_transform = body.global_transform
	if procedural_gait_enabled and skeleton != null:
		_update_procedural_gait(dt)


func _load_character() -> void:
	var packed := load(CHARACTER_PATH) as PackedScene
	if packed == null:
		push_error("Physics walker could not load %s" % CHARACTER_PATH)
		return
	var instance := packed.instantiate()
	if not (instance is Node3D):
		push_error("Asterra human GLB root is not Node3D")
		instance.queue_free()
		return
	character = instance as Node3D
	character.name = "AsterraHumanPhysicsVisual"
	add_child(character)
	_center_mesh_on_com()
	character.rotation_degrees.y = model_forward_correction_deg

	skeleton = _find_skeleton(character)
	if skeleton == null:
		push_warning("Physics walker visual found no Skeleton3D in Asterra human")
		return
	_stop_imported_animation_players(character)
	_resolve_bone_map()
	_apply_neutral_pose(1.0)


func _update_procedural_gait(dt: float) -> void:
	var up := body.global_transform.basis.y.normalized()
	var planar_velocity := body.linear_velocity - up * body.linear_velocity.dot(up)
	var speed := planar_velocity.length()
	var movement_blend := clampf(speed / 0.65, 0.0, 1.0)
	if not body.grounded:
		movement_blend *= 0.55
	_gait_blend = move_toward(_gait_blend, movement_blend, dt * 7.0)

	var speed_norm := clampf(speed / WALK_REFERENCE_SPEED, 0.0, 1.0)
	var run_norm := clampf(
		(speed - WALK_REFERENCE_SPEED) / maxf(RUN_REFERENCE_SPEED - WALK_REFERENCE_SPEED, 0.1),
		0.0,
		1.0
	)
	if speed > 0.08:
		# Roughly 1.0 cycles/s at a slow walk and ~2.2 cycles/s near run speed.
		var cadence_hz := lerpf(1.0, 1.72, speed_norm) + run_norm * 0.48
		_gait_phase = fmod(_gait_phase + dt * TAU * cadence_hz, TAU)

	var phase_sin := sin(_gait_phase)
	var phase_cos := cos(_gait_phase)
	var left_swing := maxf(phase_sin, 0.0)
	var right_swing := maxf(-phase_sin, 0.0)

	var hip_swing_deg := lerpf(18.0, 34.0, speed_norm) + run_norm * 14.0
	var hip_swing := deg_to_rad(hip_swing_deg) * phase_sin * _gait_blend * gait_strength
	var knee_peak := deg_to_rad(lerpf(30.0, 52.0, speed_norm) + run_norm * 16.0) \
		* _gait_blend * gait_strength
	var ankle_peak := deg_to_rad(lerpf(9.0, 16.0, speed_norm)) * _gait_blend * gait_strength
	var arm_swing := deg_to_rad(lerpf(13.0, 31.0, speed_norm) + run_norm * 12.0) \
		* phase_sin * _gait_blend * gait_strength
	var elbow_flex := deg_to_rad(9.0 + 18.0 * speed_norm + 17.0 * run_norm) \
		* _gait_blend * gait_strength
	var torso_twist := deg_to_rad(5.5 + 3.5 * run_norm) * phase_sin * _gait_blend * gait_strength
	var torso_side := deg_to_rad(1.8) * phase_cos * _gait_blend * gait_strength

	# Legs: negative rotation about model X sends the thigh toward model forward
	# (-Z). Knees flex only during each leg's swing phase, avoiding the old T-pose
	# while still keeping stance legs comparatively straight and load-bearing.
	_set_pose_delta("left_upper_leg", _q_axis(Vector3.RIGHT, -hip_swing))
	_set_pose_delta("right_upper_leg", _q_axis(Vector3.RIGHT, hip_swing))
	_set_pose_delta("left_lower_leg", _q_axis(Vector3.RIGHT, -knee_peak * left_swing))
	_set_pose_delta("right_lower_leg", _q_axis(Vector3.RIGHT, -knee_peak * right_swing))
	_set_pose_delta(
		"left_foot",
		_q_axis(Vector3.RIGHT, ankle_peak * (phase_sin * 0.55 - left_swing * 0.65))
	)
	_set_pose_delta(
		"right_foot",
		_q_axis(Vector3.RIGHT, ankle_peak * (-phase_sin * 0.55 - right_swing * 0.65))
	)

	# The imported rig is authored in a T pose. First adduct the arms into a relaxed
	# hanging pose around model Z, then add contralateral sagittal arm swing.
	var arm_down := deg_to_rad(ARM_LOWER_DEG) * gait_strength
	_set_pose_delta(
		"left_upper_arm",
		_q_axis(Vector3.BACK, arm_down) * _q_axis(Vector3.RIGHT, arm_swing)
	)
	_set_pose_delta(
		"right_upper_arm",
		_q_axis(Vector3.BACK, -arm_down) * _q_axis(Vector3.RIGHT, -arm_swing)
	)
	_set_pose_delta(
		"left_lower_arm",
		_q_axis(Vector3.RIGHT, -elbow_flex * (0.72 + 0.28 * right_swing))
	)
	_set_pose_delta(
		"right_lower_arm",
		_q_axis(Vector3.RIGHT, -elbow_flex * (0.72 + 0.28 * left_swing))
	)

	# Small counter-rotation stops the whole mesh from reading as a rigid mannequin.
	_set_pose_delta(
		"spine",
		_q_axis(Vector3.UP, -torso_twist * 0.45) * _q_axis(Vector3.FORWARD, torso_side)
	)
	_set_pose_delta("chest", _q_axis(Vector3.UP, -torso_twist * 0.55))
	_set_pose_delta("neck", _q_axis(Vector3.UP, torso_twist * 0.28))


func _apply_neutral_pose(weight: float) -> void:
	if skeleton == null:
		return
	var arm_down := deg_to_rad(ARM_LOWER_DEG) * clampf(weight, 0.0, 1.0)
	_set_pose_delta("left_upper_arm", _q_axis(Vector3.BACK, arm_down), 1.0)
	_set_pose_delta("right_upper_arm", _q_axis(Vector3.BACK, -arm_down), 1.0)
	_set_pose_delta("left_lower_arm", _q_axis(Vector3.RIGHT, deg_to_rad(-7.0)), 1.0)
	_set_pose_delta("right_lower_arm", _q_axis(Vector3.RIGHT, deg_to_rad(-7.0)), 1.0)


func _set_pose_delta(role: String, skeleton_delta: Quaternion, responsiveness := 0.42) -> void:
	if not _bones.has(role):
		return
	var bone := int(_bones[role])
	if bone < 0:
		return
	var rest_global := skeleton.get_bone_global_rest(bone).basis.get_rotation_quaternion()
	# Godot bone pose rotation is expressed after the bone's rest rotation. Conjugate
	# the model/skeleton-space delta into the bone's authored rest-local coordinates
	# so this works with arbitrary Blender bone roll rather than assuming local X/Y/Z.
	var target := (rest_global.inverse() * skeleton_delta * rest_global).normalized()
	var current := skeleton.get_bone_pose_rotation(bone)
	var amount := clampf(responsiveness, 0.0, 1.0)
	skeleton.set_bone_pose_rotation(bone, current.slerp(target, amount).normalized())


func _q_axis(axis: Vector3, angle: float) -> Quaternion:
	if absf(angle) <= 1e-7:
		return Quaternion.IDENTITY
	return Quaternion(axis.normalized(), angle)


func _resolve_bone_map() -> void:
	_bones.clear()
	if skeleton == null:
		return

	_bones["pelvis"] = _find_role(["hips", "pelvis", "hip"], 0)
	_bones["spine"] = _find_spine(false)
	_bones["chest"] = _find_spine(true)
	_bones["neck"] = _find_role(["neck"], 0)
	_bones["head"] = _find_role(["head"], 0)

	_bones["left_upper_leg"] = _find_limb_role(
		-1, ["upleg", "up_leg", "upperleg", "upper_leg", "thigh"], ["lower", "calf", "shin"]
	)
	_bones["right_upper_leg"] = _find_limb_role(
		1, ["upleg", "up_leg", "upperleg", "upper_leg", "thigh"], ["lower", "calf", "shin"]
	)
	_bones["left_lower_leg"] = _find_limb_role(
		-1, ["lowerleg", "lower_leg", "calf", "shin", "leg"], ["upper", "up_leg", "upleg", "thigh", "foot", "toe"]
	)
	_bones["right_lower_leg"] = _find_limb_role(
		1, ["lowerleg", "lower_leg", "calf", "shin", "leg"], ["upper", "up_leg", "upleg", "thigh", "foot", "toe"]
	)
	_bones["left_foot"] = _find_limb_role(-1, ["foot", "ankle"], ["toe"])
	_bones["right_foot"] = _find_limb_role(1, ["foot", "ankle"], ["toe"])

	_bones["left_upper_arm"] = _find_limb_role(
		-1, ["upperarm", "upper_arm", "uparm", "up_arm", "arm"], ["fore", "lower", "hand", "shoulder"]
	)
	_bones["right_upper_arm"] = _find_limb_role(
		1, ["upperarm", "upper_arm", "uparm", "up_arm", "arm"], ["fore", "lower", "hand", "shoulder"]
	)
	_bones["left_lower_arm"] = _find_limb_role(
		-1, ["forearm", "fore_arm", "lowerarm", "lower_arm"], ["hand", "upper"]
	)
	_bones["right_lower_arm"] = _find_limb_role(
		1, ["forearm", "fore_arm", "lowerarm", "lower_arm"], ["hand", "upper"]
	)

	_report_bone_mapping()


func _find_limb_role(side: int, include_tokens: Array[String], exclude_tokens: Array[String]) -> int:
	var best := -1
	var best_score := -100000
	for bone in skeleton.get_bone_count():
		var original := String(skeleton.get_bone_name(bone))
		if _name_side(original) != side:
			continue
		var normalized := _normalized_name(original)
		var score := 0
		var matched := false
		for token in include_tokens:
			if normalized.contains(token):
				matched = true
				score += 30 + token.length()
		for token in exclude_tokens:
			if normalized.contains(token):
				score -= 55
		if not matched:
			continue
		# Prefer deform bones over helper/twist/mechanism bones when both exist.
		if normalized.contains("twist") or normalized.contains("helper") or normalized.contains("mch"):
			score -= 35
		if normalized.contains("def"):
			score += 5
		if score > best_score:
			best_score = score
			best = bone
	return best


func _find_role(tokens: Array[String], side: int) -> int:
	var best := -1
	var best_score := -100000
	for bone in skeleton.get_bone_count():
		var original := String(skeleton.get_bone_name(bone))
		if side != 0 and _name_side(original) != side:
			continue
		var normalized := _normalized_name(original)
		var score := 0
		var matched := false
		for token in tokens:
			if normalized.contains(token):
				matched = true
				score += 30 + token.length()
		if not matched:
			continue
		if normalized.contains("twist") or normalized.contains("helper") or normalized.contains("mch"):
			score -= 35
		if score > best_score:
			best_score = score
			best = bone
	return best


func _find_spine(highest: bool) -> int:
	var candidates: Array[int] = []
	for bone in skeleton.get_bone_count():
		var normalized := _normalized_name(String(skeleton.get_bone_name(bone)))
		if normalized.contains("spine") or normalized.contains("chest"):
			if not normalized.contains("twist") and not normalized.contains("helper"):
				candidates.append(bone)
	if candidates.is_empty():
		return -1
	candidates.sort_custom(func(a: int, b: int) -> bool:
		return _bone_depth(a) < _bone_depth(b)
	)
	return candidates[candidates.size() - 1] if highest else candidates[0]


func _bone_depth(bone: int) -> int:
	var depth := 0
	var current := bone
	while current >= 0:
		current = skeleton.get_bone_parent(current)
		depth += 1
	return depth


func _name_side(name: String) -> int:
	var lower := name.to_lower()
	var normalized := _normalized_name(name)
	if lower.contains("left") or lower.ends_with(".l") or lower.ends_with("-l") \
			or normalized.begins_with("l_") or normalized.ends_with("_l"):
		return -1
	if lower.contains("right") or lower.ends_with(".r") or lower.ends_with("-r") \
			or normalized.begins_with("r_") or normalized.ends_with("_r"):
		return 1
	return 0


func _normalized_name(name: String) -> String:
	var result := name.to_lower()
	for separator in [".", "-", ":", " ", "/", "\\"]:
		result = result.replace(separator, "_")
	return result


func _report_bone_mapping() -> void:
	if _mapping_reported:
		return
	_mapping_reported = true
	var missing: Array[String] = []
	var resolved: Array[String] = []
	for role in _bones.keys():
		var bone := int(_bones[role])
		if bone >= 0:
			resolved.append("%s=%s" % [role, skeleton.get_bone_name(bone)])
		else:
			missing.append(String(role))
	print("Physics walker bone map: ", ", ".join(resolved))
	if not missing.is_empty():
		push_warning("Physics walker could not resolve bones: %s" % ", ".join(missing))


func _find_skeleton(node: Node) -> Skeleton3D:
	if node is Skeleton3D:
		return node as Skeleton3D
	for child in node.get_children():
		var found := _find_skeleton(child)
		if found != null:
			return found
	return null


func _stop_imported_animation_players(node: Node) -> void:
	if node is AnimationPlayer:
		(node as AnimationPlayer).stop()
	for child in node.get_children():
		_stop_imported_animation_players(child)


func _center_mesh_on_com() -> void:
	var meshes: Array[MeshInstance3D] = []
	_collect_meshes(character, meshes)
	if meshes.is_empty():
		return
	var min_point := Vector3(INF, INF, INF)
	var max_point := Vector3(-INF, -INF, -INF)
	var found := false
	for mesh in meshes:
		if mesh.mesh == null:
			continue
		var aabb := mesh.get_aabb()
		for x in 2:
			for y in 2:
				for z in 2:
					var local_point := aabb.position + Vector3(
						aabb.size.x * float(x),
						aabb.size.y * float(y),
						aabb.size.z * float(z)
					)
					var p := character.to_local(mesh.to_global(local_point))
					min_point.x = minf(min_point.x, p.x)
					min_point.y = minf(min_point.y, p.y)
					min_point.z = minf(min_point.z, p.z)
					max_point.x = maxf(max_point.x, p.x)
					max_point.y = maxf(max_point.y, p.y)
					max_point.z = maxf(max_point.z, p.z)
					found = true
	if not found:
		return
	var size := max_point - min_point
	model_height = maxf(size.y, 0.1)
	var center_xz := Vector3((min_point.x + max_point.x) * 0.5, 0.0, (min_point.z + max_point.z) * 0.5)
	var desired_bottom := -body.com_height if body != null else -0.90
	character.position = Vector3(-center_xz.x, desired_bottom - min_point.y, -center_xz.z)


func _collect_meshes(node: Node, out: Array[MeshInstance3D]) -> void:
	if node is MeshInstance3D:
		out.append(node as MeshInstance3D)
	for child in node.get_children():
		_collect_meshes(child, out)
