class_name PhysicsWalkerVisual
extends Node3D
## Skinned-mesh bridge for the articulated active ragdoll.
##
## No locomotion is authored here anymore. ActiveRagdollRig drives real constrained
## rigid bodies with finite torques; this node reads their measured relative joint
## rotations and applies those rotations to the imported Skeleton3D.

const CHARACTER_PATH := "res://assets/character/asterrahuman.glb"
const ARM_LOWER_DEG := 76.0

@export var body: PhysicsWalkerBody
@export var ragdoll: ActiveRagdollRig
@export var visible_character := true
@export_range(0.0, 1.0, 0.01) var physics_pose_follow := 0.82

var character: Node3D
var skeleton: Skeleton3D
var model_forward_correction_deg := 0.0
var model_height := 1.75

var _bones: Dictionary = {}
var _mapping_reported := false


func _ready() -> void:
	_ensure_ragdoll()
	_load_character()


func _process(_dt: float) -> void:
	if body == null or character == null:
		return
	if ragdoll == null:
		_ensure_ragdoll()
	if ragdoll != null:
		if body.active and not ragdoll.active:
			ragdoll.activate()
		elif not body.active and ragdoll.active:
			ragdoll.deactivate()
	visible = visible_character and body.active
	if not body.active:
		return
	global_transform = body.global_transform
	if skeleton == null:
		return
	if ragdoll != null and ragdoll.active:
		_apply_ragdoll_pose()
	else:
		_apply_neutral_pose()


func _ensure_ragdoll() -> void:
	if body == null or get_parent() == null:
		return
	if body.ragdoll != null:
		ragdoll = body.ragdoll
		return
	ragdoll = ActiveRagdollRig.new()
	ragdoll.name = "ActiveRagdollRig"
	ragdoll.pelvis = body
	body.ragdoll = ragdoll
	get_parent().add_child(ragdoll)


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
	_apply_neutral_pose()


func _apply_ragdoll_pose() -> void:
	var torso_rel := ragdoll.relative_rotation("torso")
	_set_pose_delta("spine", Quaternion.IDENTITY.slerp(torso_rel, 0.44))
	_set_pose_delta("chest", Quaternion.IDENTITY.slerp(torso_rel, 0.56))

	var head_rel := ragdoll.relative_rotation("head")
	_set_pose_delta("neck", Quaternion.IDENTITY.slerp(head_rel, 0.42))
	_set_pose_delta("head", Quaternion.IDENTITY.slerp(head_rel, 0.58))

	_set_pose_delta("left_upper_leg", ragdoll.relative_rotation("left_upper_leg"))
	_set_pose_delta("right_upper_leg", ragdoll.relative_rotation("right_upper_leg"))
	_set_pose_delta("left_lower_leg", ragdoll.relative_rotation("left_lower_leg"))
	_set_pose_delta("right_lower_leg", ragdoll.relative_rotation("right_lower_leg"))
	_set_pose_delta("left_foot", ragdoll.relative_rotation("left_foot"))
	_set_pose_delta("right_foot", ragdoll.relative_rotation("right_foot"))

	# The mesh rest pose is a T pose while the physical arm capsules are authored in
	# the natural hanging orientation. Apply that fixed rest-to-physics offset once;
	# every additional shoulder/elbow rotation is measured from the real bodies.
	var left_arm_down := _q_axis(Vector3.BACK, deg_to_rad(ARM_LOWER_DEG))
	var right_arm_down := _q_axis(Vector3.BACK, deg_to_rad(-ARM_LOWER_DEG))
	_set_pose_delta("left_upper_arm", left_arm_down * ragdoll.relative_rotation("left_upper_arm"))
	_set_pose_delta("right_upper_arm", right_arm_down * ragdoll.relative_rotation("right_upper_arm"))
	_set_pose_delta("left_lower_arm", ragdoll.relative_rotation("left_lower_arm"))
	_set_pose_delta("right_lower_arm", ragdoll.relative_rotation("right_lower_arm"))


func _apply_neutral_pose() -> void:
	if skeleton == null:
		return
	_set_pose_delta("spine", Quaternion.IDENTITY, 1.0)
	_set_pose_delta("chest", Quaternion.IDENTITY, 1.0)
	_set_pose_delta("neck", Quaternion.IDENTITY, 1.0)
	_set_pose_delta("head", Quaternion.IDENTITY, 1.0)
	_set_pose_delta("left_upper_leg", Quaternion.IDENTITY, 1.0)
	_set_pose_delta("right_upper_leg", Quaternion.IDENTITY, 1.0)
	_set_pose_delta("left_lower_leg", Quaternion.IDENTITY, 1.0)
	_set_pose_delta("right_lower_leg", Quaternion.IDENTITY, 1.0)
	_set_pose_delta("left_foot", Quaternion.IDENTITY, 1.0)
	_set_pose_delta("right_foot", Quaternion.IDENTITY, 1.0)
	_set_pose_delta("left_upper_arm", _q_axis(Vector3.BACK, deg_to_rad(ARM_LOWER_DEG)), 1.0)
	_set_pose_delta("right_upper_arm", _q_axis(Vector3.BACK, deg_to_rad(-ARM_LOWER_DEG)), 1.0)
	_set_pose_delta("left_lower_arm", _q_axis(Vector3.RIGHT, deg_to_rad(-7.0)), 1.0)
	_set_pose_delta("right_lower_arm", _q_axis(Vector3.RIGHT, deg_to_rad(-7.0)), 1.0)


func _set_pose_delta(role: String, skeleton_delta: Quaternion, responsiveness := -1.0) -> void:
	if skeleton == null or not _bones.has(role):
		return
	var bone := int(_bones[role])
	if bone < 0:
		return
	var rest_global := skeleton.get_bone_global_rest(bone).basis.get_rotation_quaternion()
	var target := (rest_global.inverse() * skeleton_delta * rest_global).normalized()
	var amount := physics_pose_follow if responsiveness < 0.0 else responsiveness
	var current := skeleton.get_bone_pose_rotation(bone)
	skeleton.set_bone_pose_rotation(bone, current.slerp(target, clampf(amount, 0.0, 1.0)).normalized())


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
	_bones["left_upper_leg"] = _find_limb_role(-1, ["upleg", "up_leg", "upperleg", "upper_leg", "thigh"], ["lower", "calf", "shin"])
	_bones["right_upper_leg"] = _find_limb_role(1, ["upleg", "up_leg", "upperleg", "upper_leg", "thigh"], ["lower", "calf", "shin"])
	_bones["left_lower_leg"] = _find_limb_role(-1, ["lowerleg", "lower_leg", "calf", "shin", "leg"], ["upper", "up_leg", "upleg", "thigh", "foot", "toe"])
	_bones["right_lower_leg"] = _find_limb_role(1, ["lowerleg", "lower_leg", "calf", "shin", "leg"], ["upper", "up_leg", "upleg", "thigh", "foot", "toe"])
	_bones["left_foot"] = _find_limb_role(-1, ["foot", "ankle"], ["toe"])
	_bones["right_foot"] = _find_limb_role(1, ["foot", "ankle"], ["toe"])
	_bones["left_upper_arm"] = _find_limb_role(-1, ["upperarm", "upper_arm", "uparm", "up_arm", "arm"], ["fore", "lower", "hand", "shoulder"])
	_bones["right_upper_arm"] = _find_limb_role(1, ["upperarm", "upper_arm", "uparm", "up_arm", "arm"], ["fore", "lower", "hand", "shoulder"])
	_bones["left_lower_arm"] = _find_limb_role(-1, ["forearm", "fore_arm", "lowerarm", "lower_arm"], ["hand", "upper"])
	_bones["right_lower_arm"] = _find_limb_role(1, ["forearm", "fore_arm", "lowerarm", "lower_arm"], ["hand", "upper"])
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
	var parts: Array[String] = []
	var missing: Array[String] = []
	for role in _bones.keys():
		var index := int(_bones[role])
		if index >= 0:
			parts.append("%s=%s" % [role, skeleton.get_bone_name(index)])
		else:
			missing.append(String(role))
	print("Active ragdoll bone map: ", ", ".join(parts))
	if not missing.is_empty():
		push_warning("Active ragdoll could not resolve bones: %s" % ", ".join(missing))


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
		var player := node as AnimationPlayer
		player.stop()
		player.active = false
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
