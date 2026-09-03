class_name PhysicsWalkerVisual
extends Node3D
## Skinned-mesh bridge for the articulated active ragdoll.
##
## The imported Asterra human is authored facing the opposite direction from the
## physics walker convention (-Z forward). The character root is therefore yawed
## 180 degrees, and every physical joint rotation is conjugated through that same
## correction before it is applied to Skeleton3D. This is important: correcting only
## the visible root makes pitch/roll joint motion appear mirrored or reversed.

const CHARACTER_PATH := "res://assets/character/asterrahuman.glb"
const ARM_LOWER_DEG := 76.0

@export var body: PhysicsWalkerBody
@export var ragdoll: ActiveRagdollRig
@export var visible_character := true
@export_range(0.0, 1.0, 0.01) var physics_pose_follow := 0.72
@export var model_forward_correction_deg := 180.0

var character: Node3D
var skeleton: Skeleton3D
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
	if int(_bones.get("chest", -1)) != int(_bones.get("spine", -1)):
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

	# The skin rest pose is a T pose; the physical arm bodies are authored hanging.
	# This is a fixed rest offset, then the measured shoulder rotation is added.
	var left_arm_down := _q_axis(Vector3.BACK, deg_to_rad(ARM_LOWER_DEG))
	var right_arm_down := _q_axis(Vector3.BACK, deg_to_rad(-ARM_LOWER_DEG))
	_set_pose_delta("left_upper_arm", left_arm_down * ragdoll.relative_rotation("left_upper_arm"))
	_set_pose_delta("right_upper_arm", right_arm_down * ragdoll.relative_rotation("right_upper_arm"))
	_set_pose_delta("left_lower_arm", ragdoll.relative_rotation("left_lower_arm"))
	_set_pose_delta("right_lower_arm", ragdoll.relative_rotation("right_lower_arm"))


func _apply_neutral_pose() -> void:
	if skeleton == null:
		return
	for role in [
		"spine", "chest", "neck", "head",
		"left_upper_leg", "right_upper_leg",
		"left_lower_leg", "right_lower_leg",
		"left_foot", "right_foot"
	]:
		_set_pose_delta(role, Quaternion.IDENTITY, 1.0)
	_set_pose_delta("left_upper_arm", _q_axis(Vector3.BACK, deg_to_rad(ARM_LOWER_DEG)), 1.0)
	_set_pose_delta("right_upper_arm", _q_axis(Vector3.BACK, deg_to_rad(-ARM_LOWER_DEG)), 1.0)
	_set_pose_delta("left_lower_arm", _q_axis(Vector3.RIGHT, deg_to_rad(-6.0)), 1.0)
	_set_pose_delta("right_lower_arm", _q_axis(Vector3.RIGHT, deg_to_rad(-6.0)), 1.0)


func _set_pose_delta(role: String, physics_delta: Quaternion, responsiveness := -1.0) -> void:
	if skeleton == null or not _bones.has(role):
		return
	var bone := int(_bones[role])
	if bone < 0:
		return

	# Convert from the physics walker's coordinate system into the imported model's
	# coordinate system. A visible-only 180 degree root yaw is not enough: rotations
	# themselves must be transformed by C^-1 * R * C.
	var skeleton_delta := _physics_to_skeleton_delta(physics_delta)
	var rest_global := skeleton.get_bone_global_rest(bone).basis.get_rotation_quaternion()
	var target := (rest_global.inverse() * skeleton_delta * rest_global).normalized()
	var amount := physics_pose_follow if responsiveness < 0.0 else responsiveness
	var current := skeleton.get_bone_pose_rotation(bone)
	skeleton.set_bone_pose_rotation(
		bone,
		current.slerp(target, clampf(amount, 0.0, 1.0)).normalized()
	)


func _physics_to_skeleton_delta(delta: Quaternion) -> Quaternion:
	var correction := Quaternion(Vector3.UP, deg_to_rad(model_forward_correction_deg))
	return (correction.inverse() * delta * correction).normalized()


func _rest_position_physics(bone: int) -> Vector3:
	var p := skeleton.get_bone_global_rest(bone).origin
	var correction := Basis(Vector3.UP, deg_to_rad(model_forward_correction_deg))
	return correction * p


func _q_axis(axis: Vector3, angle: float) -> Quaternion:
	if absf(angle) <= 1e-7:
		return Quaternion.IDENTITY
	return Quaternion(axis.normalized(), angle)


func _resolve_bone_map() -> void:
	_bones.clear()
	if skeleton == null:
		return

	_bones["pelvis"] = _find_named_role(["hips", "pelvis", "hip"], 0, [])
	_bones["spine"] = _find_named_role(["spine", "abdomen", "torso"], 0, ["twist"])
	_bones["chest"] = _find_named_role(["chest", "spine2", "spine_2", "spine3", "spine_3"], 0, ["twist"])
	_bones["neck"] = _find_named_role(["neck"], 0, ["twist"])
	_bones["head"] = _find_named_role(["head"], 0, ["end"])

	_bones["left_upper_leg"] = _find_named_role(
		["thigh", "upleg", "up_leg", "upperleg", "upper_leg"], -1,
		["twist", "helper", "mch", "lower", "calf", "shin"]
	)
	_bones["right_upper_leg"] = _find_named_role(
		["thigh", "upleg", "up_leg", "upperleg", "upper_leg"], 1,
		["twist", "helper", "mch", "lower", "calf", "shin"]
	)
	_bones["left_lower_leg"] = _find_named_role(
		["shin", "calf", "lowerleg", "lower_leg", "leg"], -1,
		["twist", "helper", "mch", "upper", "thigh", "foot", "toe"]
	)
	_bones["right_lower_leg"] = _find_named_role(
		["shin", "calf", "lowerleg", "lower_leg", "leg"], 1,
		["twist", "helper", "mch", "upper", "thigh", "foot", "toe"]
	)
	_bones["left_foot"] = _find_named_role(["foot", "ankle"], -1, ["toe", "end", "helper"])
	_bones["right_foot"] = _find_named_role(["foot", "ankle"], 1, ["toe", "end", "helper"])

	_bones["left_upper_arm"] = _find_named_role(
		["upperarm", "upper_arm", "uparm", "up_arm", "arm"], -1,
		["twist", "helper", "mch", "fore", "lower", "hand", "clavicle", "shoulder"]
	)
	_bones["right_upper_arm"] = _find_named_role(
		["upperarm", "upper_arm", "uparm", "up_arm", "arm"], 1,
		["twist", "helper", "mch", "fore", "lower", "hand", "clavicle", "shoulder"]
	)
	_bones["left_lower_arm"] = _find_named_role(
		["forearm", "fore_arm", "lowerarm", "lower_arm"], -1,
		["twist", "helper", "mch", "hand", "upper"]
	)
	_bones["right_lower_arm"] = _find_named_role(
		["forearm", "fore_arm", "lowerarm", "lower_arm"], 1,
		["twist", "helper", "mch", "hand", "upper"]
	)

	_resolve_geometry_fallbacks()
	_remove_duplicate_limb_assignments()
	_resolve_geometry_fallbacks()
	_report_bone_mapping()


func _find_named_role(tokens: Array, side: int, excludes: Array) -> int:
	var best := -1
	var best_score := -100000
	for bone in skeleton.get_bone_count():
		if side != 0 and _bone_side(bone) != side:
			continue
		var name := _normalized_name(String(skeleton.get_bone_name(bone)))
		var compact := name.replace("_", "")
		var score := 0
		var matched := false
		for token_value in tokens:
			var token := String(token_value).to_lower()
			if name.contains(token) or compact.contains(token.replace("_", "")):
				matched = true
				score += 35 + token.length()
		for token_value in excludes:
			var token := String(token_value).to_lower()
			if name.contains(token) or compact.contains(token.replace("_", "")):
				score -= 70
		if not matched:
			continue
		if name.contains("def"):
			score += 5
		if score > best_score:
			best_score = score
			best = bone
	return best


func _resolve_geometry_fallbacks() -> void:
	var pelvis := int(_bones.get("pelvis", -1))
	if pelvis < 0:
		pelvis = _guess_pelvis()
		_bones["pelvis"] = pelvis

	if int(_bones.get("spine", -1)) < 0 and pelvis >= 0:
		_bones["spine"] = _guess_central_above(pelvis, false)
	if int(_bones.get("chest", -1)) < 0 and pelvis >= 0:
		_bones["chest"] = _guess_central_above(pelvis, true)
	# Some valid humanoid rigs expose only one torso bone. Reuse it for the chest
	# role instead of reporting a broken ragdoll map for an otherwise usable rig.
	if int(_bones.get("chest", -1)) < 0:
		_bones["chest"] = int(_bones.get("spine", -1))

	var spine := int(_bones.get("spine", -1))
	var chest := int(_bones.get("chest", -1))
	var torso_root := chest if chest >= 0 else spine

	if int(_bones.get("neck", -1)) < 0 and torso_root >= 0:
		_bones["neck"] = _guess_central_child_above(torso_root)
	if int(_bones.get("head", -1)) < 0:
		var neck := int(_bones.get("neck", -1))
		if neck >= 0:
			_bones["head"] = _guess_central_child_above(neck)

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
			lower_leg = _guess_descendant_in_direction(upper_leg, side, Vector3.DOWN, 3)
			_bones[lower_leg_key] = lower_leg
		if int(_bones.get(foot_key, -1)) < 0 and lower_leg >= 0:
			_bones[foot_key] = _guess_lowest_descendant(lower_leg, side, 3)

		var upper_arm := int(_bones.get(upper_arm_key, -1))
		if upper_arm < 0 and torso_root >= 0:
			upper_arm = _guess_upper_arm(torso_root, side)
			_bones[upper_arm_key] = upper_arm
		if int(_bones.get(lower_arm_key, -1)) < 0 and upper_arm >= 0:
			var outward := Vector3.LEFT if side < 0 else Vector3.RIGHT
			_bones[lower_arm_key] = _guess_descendant_in_direction(upper_arm, side, outward, 4)


func _guess_pelvis() -> int:
	var bounds := _rest_bounds()
	var min_y := (bounds[0] as Vector3).y
	var max_y := (bounds[1] as Vector3).y
	var target_y := lerpf(min_y, max_y, 0.48)
	var best := -1
	var best_score := -INF
	for bone in skeleton.get_bone_count():
		var p := _rest_position_physics(bone)
		var children := _direct_children(bone).size()
		if children < 2:
			continue
		var score := float(children) * 3.0 - absf(p.x) * 10.0 - absf(p.y - target_y) * 4.0
		if score > best_score:
			best_score = score
			best = bone
	return best


func _guess_central_above(root: int, highest: bool) -> int:
	var root_p := _rest_position_physics(root)
	var best := -1
	var best_value := -INF if highest else INF
	for bone in _descendants(root, 6):
		var p := _rest_position_physics(bone)
		if p.y <= root_p.y + model_height * 0.025:
			continue
		if absf(p.x) > model_height * 0.12:
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


func _guess_central_child_above(root: int) -> int:
	var root_p := _rest_position_physics(root)
	var best := -1
	var best_dist := INF
	for bone in _descendants(root, 3):
		var p := _rest_position_physics(bone)
		if p.y <= root_p.y + 0.01 or absf(p.x) > model_height * 0.10:
			continue
		var d := p.distance_to(root_p)
		if d < best_dist:
			best_dist = d
			best = bone
	return best


func _guess_upper_leg(pelvis_bone: int, side: int) -> int:
	var root_p := _rest_position_physics(pelvis_bone)
	var best := -1
	var best_score := -INF
	for bone in _descendants(pelvis_bone, 3):
		if _bone_side(bone) != side:
			continue
		var p := _rest_position_physics(bone)
		if p.y >= root_p.y - 0.01:
			continue
		var vertical := root_p.y - p.y
		var score := -vertical * 4.0 + absf(p.x - root_p.x) * 0.6 - float(_bone_depth_from(bone, pelvis_bone)) * 0.2
		if score > best_score:
			best_score = score
			best = bone
	return best


func _guess_upper_arm(torso_root: int, side: int) -> int:
	var root_p := _rest_position_physics(torso_root)
	var best := -1
	var best_score := -INF
	for bone in _descendants(torso_root, 5):
		if _bone_side(bone) != side:
			continue
		var p := _rest_position_physics(bone)
		var lateral := absf(p.x - root_p.x)
		if lateral < model_height * 0.08:
			continue
		if absf(p.y - root_p.y) > model_height * 0.28:
			continue
		var child_bonus := 1.0 if not _direct_children(bone).is_empty() else 0.0
		var score := lateral * 3.0 + child_bonus - float(_bone_depth_from(bone, torso_root)) * 0.32
		if score > best_score:
			best_score = score
			best = bone
	return best


func _guess_descendant_in_direction(root: int, side: int, direction: Vector3, max_depth: int) -> int:
	var root_p := _rest_position_physics(root)
	var dir := direction.normalized()
	var best := -1
	var best_score := -INF
	for bone in _descendants(root, max_depth):
		var bone_side := _bone_side(bone)
		if bone_side != side and bone_side != 0:
			continue
		var delta := _rest_position_physics(bone) - root_p
		var distance := delta.length()
		if distance < 0.01:
			continue
		var alignment := delta.normalized().dot(dir)
		if alignment < 0.35:
			continue
		var score := alignment * 4.0 - distance * 0.35 - float(_bone_depth_from(bone, root)) * 0.1
		if score > best_score:
			best_score = score
			best = bone
	return best


func _guess_lowest_descendant(root: int, side: int, max_depth: int) -> int:
	var best := -1
	var best_y := INF
	for bone in _descendants(root, max_depth):
		var bone_side := _bone_side(bone)
		if bone_side != side and bone_side != 0:
			continue
		var p := _rest_position_physics(bone)
		if p.y < best_y:
			best_y = p.y
			best = bone
	return best


func _remove_duplicate_limb_assignments() -> void:
	var roles := [
		"left_upper_leg", "left_lower_leg", "left_foot",
		"right_upper_leg", "right_lower_leg", "right_foot",
		"left_upper_arm", "left_lower_arm",
		"right_upper_arm", "right_lower_arm"
	]
	var used: Dictionary = {}
	for role_value in roles:
		var role := String(role_value)
		var bone := int(_bones.get(role, -1))
		if bone < 0:
			continue
		if used.has(bone):
			push_warning(
				"Active ragdoll duplicate bone %s for %s and %s; re-resolving %s" % [
					skeleton.get_bone_name(bone), String(used[bone]), role, role
				]
			)
			_bones[role] = -1
		else:
			used[bone] = role


func _bone_side(bone: int) -> int:
	var name := _normalized_name(String(skeleton.get_bone_name(bone)))
	var padded := "_%s_" % name
	if name.contains("left") or padded.contains("_l_") or name.begins_with("l_") or name.ends_with("_l"):
		return -1
	if name.contains("right") or padded.contains("_r_") or name.begins_with("r_") or name.ends_with("_r"):
		return 1

	# Name did not tell us. Use the corrected rest pose, not raw GLB X: the imported
	# model is yawed 180 degrees relative to the physical character frame.
	var p := _rest_position_physics(bone)
	var epsilon := maxf(model_height * 0.018, 0.015)
	if p.x < -epsilon:
		return -1
	if p.x > epsilon:
		return 1
	return 0


func _normalized_name(name: String) -> String:
	var result := name.to_lower()
	for separator in [".", "-", ":", " ", "/", "\\"]:
		result = result.replace(separator, "_")
	while result.contains("__"):
		result = result.replace("__", "_")
	return result


func _direct_children(parent_bone: int) -> Array[int]:
	var result: Array[int] = []
	for bone in skeleton.get_bone_count():
		if skeleton.get_bone_parent(bone) == parent_bone:
			result.append(bone)
	return result


func _descendants(root: int, max_depth: int) -> Array[int]:
	var result: Array[int] = []
	for bone in skeleton.get_bone_count():
		var depth := _bone_depth_from(bone, root)
		if depth > 0 and depth <= max_depth:
			result.append(bone)
	return result


func _bone_depth_from(bone: int, ancestor: int) -> int:
	var depth := 0
	var current := bone
	while current >= 0 and depth <= 32:
		if current == ancestor:
			return depth
		current = skeleton.get_bone_parent(current)
		depth += 1
	return -1


func _rest_bounds() -> Array[Vector3]:
	var minimum := Vector3(INF, INF, INF)
	var maximum := Vector3(-INF, -INF, -INF)
	for bone in skeleton.get_bone_count():
		var p := _rest_position_physics(bone)
		minimum.x = minf(minimum.x, p.x)
		minimum.y = minf(minimum.y, p.y)
		minimum.z = minf(minimum.z, p.z)
		maximum.x = maxf(maximum.x, p.x)
		maximum.y = maxf(maximum.y, p.y)
		maximum.z = maxf(maximum.z, p.z)
	return [minimum, maximum]


func _report_bone_mapping() -> void:
	if _mapping_reported:
		return
	_mapping_reported = true
	var parts: Array[String] = []
	var missing: Array[String] = []
	for role in _bones.keys():
		var index := int(_bones[role])
		if index >= 0:
			parts.append(
				"%s=%s rest=%s" % [
					String(role),
					String(skeleton.get_bone_name(index)),
					str(_rest_position_physics(index))
				]
			)
		else:
			missing.append(String(role))
	print(
		"Active ragdoll bone map (model yaw %.1f deg): %s" % [
			model_forward_correction_deg,
			", ".join(parts)
		]
	)
	if not missing.is_empty():
		push_warning("Active ragdoll could not resolve bones: %s" % ", ".join(missing))
		_print_bone_inventory()


func _print_bone_inventory() -> void:
	print("Asterra humanoid Skeleton3D inventory:")
	for bone in skeleton.get_bone_count():
		print(
			"  #%d %s parent=%d rest_physics=%s" % [
				bone,
				String(skeleton.get_bone_name(bone)),
				skeleton.get_bone_parent(bone),
				str(_rest_position_physics(bone))
			]
		)


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
	var center_xz := Vector3(
		(min_point.x + max_point.x) * 0.5,
		0.0,
		(min_point.z + max_point.z) * 0.5
	)
	var desired_bottom := -body.com_height if body != null else -0.90
	character.position = Vector3(
		-center_xz.x,
		desired_bottom - min_point.y,
		-center_xz.z
	)


func _collect_meshes(node: Node, out: Array[MeshInstance3D]) -> void:
	if node is MeshInstance3D:
		out.append(node as MeshInstance3D)
	for child in node.get_children():
		_collect_meshes(child, out)
