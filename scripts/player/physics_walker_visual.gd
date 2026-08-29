class_name PhysicsWalkerVisual
extends Node3D
## Skinned-mesh bridge for the articulated active ragdoll.
##
## ActiveRagdollRig drives the physical bodies. This node resolves the imported
## humanoid bones, then copies measured physical joint rotations onto the skin.
## Bone names are treated only as hints: side and chain fallbacks use the rest-pose
## geometry/hierarchy so Blender/Rigify/Mixamo/custom naming can all be handled.

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

	# The skin is authored in a T pose while the physical arm bodies start hanging.
	var left_arm_down := _q_axis(Vector3.BACK, deg_to_rad(ARM_LOWER_DEG))
	var right_arm_down := _q_axis(Vector3.BACK, deg_to_rad(-ARM_LOWER_DEG))
	_set_pose_delta("left_upper_arm", left_arm_down * ragdoll.relative_rotation("left_upper_arm"))
	_set_pose_delta("right_upper_arm", right_arm_down * ragdoll.relative_rotation("right_upper_arm"))
	_set_pose_delta("left_lower_arm", ragdoll.relative_rotation("left_lower_arm"))
	_set_pose_delta("right_lower_arm", ragdoll.relative_rotation("right_lower_arm"))


func _apply_neutral_pose() -> void:
	if skeleton == null:
		return
	for role in ["spine", "chest", "neck", "head", "left_upper_leg", "right_upper_leg",
			"left_lower_leg", "right_lower_leg", "left_foot", "right_foot"]:
		_set_pose_delta(role, Quaternion.IDENTITY, 1.0)
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
	_bones["left_upper_arm"] = _find_limb_role(-1, ["upperarm", "upper_arm", "uparm", "up_arm", "arm"], ["fore", "lower", "hand", "shoulder", "clavicle"])
	_bones["right_upper_arm"] = _find_limb_role(1, ["upperarm", "upper_arm", "uparm", "up_arm", "arm"], ["fore", "lower", "hand", "shoulder", "clavicle"])
	_bones["left_lower_arm"] = _find_limb_role(-1, ["forearm", "fore_arm", "lowerarm", "lower_arm"], ["hand", "upper"])
	_bones["right_lower_arm"] = _find_limb_role(1, ["forearm", "fore_arm", "lowerarm", "lower_arm"], ["hand", "upper"])

	_resolve_hierarchy_fallbacks()
	_remove_duplicate_limb_assignments()
	_resolve_hierarchy_fallbacks()
	_report_bone_mapping()


func _find_limb_role(side: int, include_tokens: Array[String], exclude_tokens: Array[String]) -> int:
	var best := -1
	var best_score := -100000
	for bone in skeleton.get_bone_count():
		if _bone_side(bone) != side:
			continue
		var normalized := _normalized_name(String(skeleton.get_bone_name(bone)))
		var compact := normalized.replace("_", "")
		var score := 0
		var matched := false
		for token in include_tokens:
			var token_compact := token.replace("_", "")
			if normalized.contains(token) or compact.contains(token_compact):
				matched = true
				score += 30 + token.length()
		for token in exclude_tokens:
			var token_compact := token.replace("_", "")
			if normalized.contains(token) or compact.contains(token_compact):
				score -= 55
		if not matched:
			continue
		if _is_helper_bone(normalized):
			score -= 50
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
		if side != 0 and _bone_side(bone) != side:
			continue
		var normalized := _normalized_name(String(skeleton.get_bone_name(bone)))
		var compact := normalized.replace("_", "")
		var score := 0
		var matched := false
		for token in tokens:
			if normalized.contains(token) or compact.contains(token.replace("_", "")):
				matched = true
				score += 30 + token.length()
		if not matched:
			continue
		if _is_helper_bone(normalized):
			score -= 50
		if score > best_score:
			best_score = score
			best = bone
	return best


func _find_spine(highest: bool) -> int:
	var candidates: Array[int] = []
	for bone in skeleton.get_bone_count():
		var normalized := _normalized_name(String(skeleton.get_bone_name(bone)))
		if (normalized.contains("spine") or normalized.contains("chest")) and not _is_helper_bone(normalized):
			candidates.append(bone)
	if candidates.is_empty():
		return -1
	candidates.sort_custom(func(a: int, b: int) -> bool:
		return _bone_depth(a) < _bone_depth(b)
	)
	return candidates[candidates.size() - 1] if highest else candidates[0]


func _resolve_hierarchy_fallbacks() -> void:
	var pelvis := int(_bones.get("pelvis", -1))
	if pelvis < 0:
		pelvis = _guess_pelvis()
		_bones["pelvis"] = pelvis

	var spine := int(_bones.get("spine", -1))
	var chest := int(_bones.get("chest", -1))
	if spine < 0 and pelvis >= 0:
		spine = _guess_central_up_chain(pelvis, false)
		_bones["spine"] = spine
	if chest < 0 and pelvis >= 0:
		chest = _guess_central_up_chain(pelvis, true)
		_bones["chest"] = chest

	for side in [-1, 1]:
		var prefix := "left" if side < 0 else "right"
		var upper_leg_key := "%s_upper_leg" % prefix
		var lower_leg_key := "%s_lower_leg" % prefix
		var foot_key := "%s_foot" % prefix
		var upper_arm_key := "%s_upper_arm" % prefix
		var lower_arm_key := "%s_lower_arm" % prefix

		var upper_leg := int(_bones.get(upper_leg_key, -1))
		if upper_leg < 0 and pelvis >= 0:
			upper_leg = _guess_upper_leg(pelvis, side)
			_bones[upper_leg_key] = upper_leg
		var lower_leg := int(_bones.get(lower_leg_key, -1))
		if lower_leg < 0 and upper_leg >= 0:
			lower_leg = _guess_chain_child(upper_leg, side, false)
			_bones[lower_leg_key] = lower_leg
		if int(_bones.get(foot_key, -1)) < 0 and lower_leg >= 0:
			_bones[foot_key] = _guess_foot(lower_leg, side)

		var arm_root := chest if chest >= 0 else spine
		var upper_arm := int(_bones.get(upper_arm_key, -1))
		if upper_arm < 0 and arm_root >= 0:
			upper_arm = _guess_upper_arm(arm_root, side)
			_bones[upper_arm_key] = upper_arm
		if int(_bones.get(lower_arm_key, -1)) < 0 and upper_arm >= 0:
			_bones[lower_arm_key] = _guess_chain_child(upper_arm, side, true)


func _guess_pelvis() -> int:
	var bounds := _rest_bounds()
	var min_y := (bounds[0] as Vector3).y
	var max_y := (bounds[1] as Vector3).y
	var target_y := lerpf(min_y, max_y, 0.47)
	var best := -1
	var best_score := -INF
	for bone in skeleton.get_bone_count():
		var p := skeleton.get_bone_global_rest(bone).origin
		var children := _direct_children(bone).size()
		if children < 2:
			continue
		var score := float(children) * 4.0 - absf(p.x) * 8.0 - absf(p.y - target_y) * 3.0
		if score > best_score:
			best_score = score
			best = bone
	return best


func _guess_central_up_chain(root: int, highest: bool) -> int:
	var root_p := skeleton.get_bone_global_rest(root).origin
	var best := -1
	var best_value := -INF if highest else INF
	for bone in _descendants(root, 5):
		var p := skeleton.get_bone_global_rest(bone).origin
		if p.y <= root_p.y + 0.02:
			continue
		if absf(p.x - root_p.x) > maxf(model_height * 0.12, 0.08):
			continue
		if highest:
			if p.y > best_value:
				best_value = p.y
				best = bone
		else:
			if p.y < best_value:
				best_value = p.y
				best = bone
	return best


func _guess_upper_leg(pelvis_bone: int, side: int) -> int:
	var root_p := skeleton.get_bone_global_rest(pelvis_bone).origin
	var best := -1
	var best_score := -INF
	for bone in _descendants(pelvis_bone, 3):
		if _bone_side(bone) != side:
			continue
		var p := skeleton.get_bone_global_rest(bone).origin
		if p.y > root_p.y + model_height * 0.08:
			continue
		var drop := _max_downstream_drop(bone)
		if drop < model_height * 0.10:
			continue
		var score := drop * 8.0 - float(_distance_from_ancestor(pelvis_bone, bone)) * 0.9
		score -= absf(p.y - root_p.y) * 0.8
		if score > best_score:
			best_score = score
			best = bone
	return best


func _guess_upper_arm(root: int, side: int) -> int:
	var root_p := skeleton.get_bone_global_rest(root).origin
	var best := -1
	var best_score := -INF
	for bone in _descendants(root, 4):
		if _bone_side(bone) != side:
			continue
		var p := skeleton.get_bone_global_rest(bone).origin
		var lateral := absf(p.x - root_p.x)
		var reach := _max_downstream_lateral_reach(bone, root_p.x)
		if reach < model_height * 0.12:
			continue
		var score := reach * 7.0 + lateral * 2.0 - float(_distance_from_ancestor(root, bone)) * 0.8
		if score > best_score:
			best_score = score
			best = bone
	return best


func _guess_chain_child(parent_bone: int, side: int, lateral_chain: bool) -> int:
	var parent_p := skeleton.get_bone_global_rest(parent_bone).origin
	var best := -1
	var best_score := -INF
	for bone in _descendants(parent_bone, 2):
		if _bone_side(bone) != side:
			continue
		var p := skeleton.get_bone_global_rest(bone).origin
		var progress := absf(p.x - parent_p.x) if lateral_chain else (parent_p.y - p.y)
		if progress <= 0.015:
			continue
		var score := progress * 8.0 - float(_distance_from_ancestor(parent_bone, bone)) * 0.9
		if score > best_score:
			best_score = score
			best = bone
	return best


func _guess_foot(lower_leg: int, side: int) -> int:
	var parent_p := skeleton.get_bone_global_rest(lower_leg).origin
	var best := -1
	var best_score := -INF
	for bone in _descendants(lower_leg, 3):
		if _bone_side(bone) != side:
			continue
		var p := skeleton.get_bone_global_rest(bone).origin
		var depth := _distance_from_ancestor(lower_leg, bone)
		var score := (parent_p.y - p.y) * 5.0 + absf(p.z - parent_p.z) * 2.0 - float(depth) * 0.45
		if score > best_score:
			best_score = score
			best = bone
	return best


func _remove_duplicate_limb_assignments() -> void:
	var roles := ["left_upper_leg", "right_upper_leg", "left_lower_leg", "right_lower_leg",
		"left_foot", "right_foot", "left_upper_arm", "right_upper_arm",
		"left_lower_arm", "right_lower_arm"]
	var used: Dictionary = {}
	for role in roles:
		var bone := int(_bones.get(role, -1))
		if bone < 0:
			continue
		if used.has(bone):
			push_warning("Bone %s was assigned to both %s and %s; re-resolving %s" % [
				skeleton.get_bone_name(bone), used[bone], role, role])
			_bones[role] = -1
		else:
			used[bone] = role


func _bone_side(bone: int) -> int:
	var named_side := _name_side(String(skeleton.get_bone_name(bone)))
	if named_side != 0:
		return named_side
	var center_x := 0.0
	var pelvis := int(_bones.get("pelvis", -1))
	if pelvis >= 0 and pelvis < skeleton.get_bone_count():
		center_x = skeleton.get_bone_global_rest(pelvis).origin.x
	var x := skeleton.get_bone_global_rest(bone).origin.x - center_x
	var threshold := maxf(model_height * 0.015, 0.012)
	if x < -threshold:
		return -1
	if x > threshold:
		return 1
	return 0


func _name_side(name: String) -> int:
	var lower := name.to_lower()
	var normalized := _normalized_name(name)
	var parts := normalized.split("_", false)
	if lower.contains("left") or lower.ends_with(".l") or lower.ends_with("-l") \
			or normalized.begins_with("l_") or normalized.ends_with("_l") \
			or normalized.contains("_l_") or parts.has("l"):
		return -1
	if lower.contains("right") or lower.ends_with(".r") or lower.ends_with("-r") \
			or normalized.begins_with("r_") or normalized.ends_with("_r") \
			or normalized.contains("_r_") or parts.has("r"):
		return 1
	return 0


func _is_helper_bone(normalized: String) -> bool:
	return normalized.contains("twist") or normalized.contains("helper") \
		or normalized.contains("mch") or normalized.contains("mechanism")


func _normalized_name(name: String) -> String:
	var result := name.to_lower()
	for separator in [".", "-", ":", " ", "/", "\\"]:
		result = result.replace(separator, "_")
	return result


func _bone_depth(bone: int) -> int:
	var depth := 0
	var current := bone
	while current >= 0:
		current = skeleton.get_bone_parent(current)
		depth += 1
	return depth


func _direct_children(parent_bone: int) -> Array[int]:
	var result: Array[int] = []
	for bone in skeleton.get_bone_count():
		if skeleton.get_bone_parent(bone) == parent_bone:
			result.append(bone)
	return result


func _descendants(root: int, max_depth: int) -> Array[int]:
	var result: Array[int] = []
	var frontier: Array[Dictionary] = [{"bone": root, "depth": 0}]
	var cursor := 0
	while cursor < frontier.size():
		var item := frontier[cursor]
		cursor += 1
		var depth := int(item["depth"])
		if depth >= max_depth:
			continue
		for child in _direct_children(int(item["bone"])):
			result.append(child)
			frontier.append({"bone": child, "depth": depth + 1})
	return result


func _distance_from_ancestor(ancestor: int, bone: int) -> int:
	var distance := 0
	var current := bone
	while current >= 0 and current != ancestor:
		current = skeleton.get_bone_parent(current)
		distance += 1
		if distance > skeleton.get_bone_count():
			break
	return distance if current == ancestor else 999


func _max_downstream_drop(bone: int) -> float:
	var origin_y := skeleton.get_bone_global_rest(bone).origin.y
	var drop := 0.0
	for child in _descendants(bone, 5):
		drop = maxf(drop, origin_y - skeleton.get_bone_global_rest(child).origin.y)
	return drop


func _max_downstream_lateral_reach(bone: int, center_x: float) -> float:
	var reach := absf(skeleton.get_bone_global_rest(bone).origin.x - center_x)
	for child in _descendants(bone, 5):
		reach = maxf(reach, absf(skeleton.get_bone_global_rest(child).origin.x - center_x))
	return reach


func _rest_bounds() -> Array[Vector3]:
	var min_p := Vector3(INF, INF, INF)
	var max_p := Vector3(-INF, -INF, -INF)
	for bone in skeleton.get_bone_count():
		var p := skeleton.get_bone_global_rest(bone).origin
		min_p.x = minf(min_p.x, p.x)
		min_p.y = minf(min_p.y, p.y)
		min_p.z = minf(min_p.z, p.z)
		max_p.x = maxf(max_p.x, p.x)
		max_p.y = maxf(max_p.y, p.y)
		max_p.z = maxf(max_p.z, p.z)
	return [min_p, max_p]


func _report_bone_mapping() -> void:
	if _mapping_reported:
		return
	_mapping_reported = true
	var parts: Array[String] = []
	var missing: Array[String] = []
	for role in _bones.keys():
		var index := int(_bones[role])
		if index >= 0:
			var p := skeleton.get_bone_global_rest(index).origin
			parts.append("%s=#%d %s rest=(%.3f,%.3f,%.3f)" % [
				role, index, skeleton.get_bone_name(index), p.x, p.y, p.z])
		else:
			missing.append(String(role))
	print("Active ragdoll bone map: ", ", ".join(parts))
	if not missing.is_empty():
		push_warning("Active ragdoll could not resolve bones: %s" % ", ".join(missing))
		_print_skeleton_inventory()


func _print_skeleton_inventory() -> void:
	var rows: Array[String] = []
	for bone in skeleton.get_bone_count():
		var p := skeleton.get_bone_global_rest(bone).origin
		rows.append("#%d %s parent=%d rest=(%.3f,%.3f,%.3f)" % [
			bone, skeleton.get_bone_name(bone), skeleton.get_bone_parent(bone), p.x, p.y, p.z])
	print("Asterra humanoid skeleton inventory:\n", "\n".join(rows))


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
