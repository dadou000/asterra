extends Node3D
## Visual-only bridge from the 19 rigid-body ragdoll to the complete imported
## Asterra character skeleton. The GLB never becomes physics-authoritative.
##
## C toggles the skinned character. The 19 semantic rigid bodies directly drive
## matched landmark bones. Bones between two landmarks are interpolated in the
## original hierarchy (useful for twist/helper/spine bones); all remaining bones
## preserve their imported rest pose and inherit from their nearest driven parent.

const CHARACTER_SCENE: PackedScene = preload("res://assets/character/asterrahuman.glb")
const TOGGLE_KEY: Key = KEY_C
const EXPECTED_ANCHORS: int = 19

const SEMANTICS: Array[String] = [
	"pelvis", "spine", "chest", "neck", "head",
	"left_clavicle", "right_clavicle",
	"left_upper_arm", "right_upper_arm",
	"left_forearm", "right_forearm",
	"left_hand", "right_hand",
	"left_thigh", "right_thigh",
	"left_shin", "right_shin",
	"left_foot", "right_foot",
]

var _ragdoll: Node3D
var _bodies: Dictionary = {}
var _character_root: Node3D
var _skeleton: Skeleton3D
var _status_label: Label
var _initialized: bool = false
var _character_enabled: bool = false

# semantic String -> bone index int
var _anchor_bones: Dictionary = {}
# bone index int -> semantic String
var _semantic_by_bone: Dictionary = {}
# semantic String -> Transform3D from rigid-body frame to neutral bone frame
var _body_to_bone_bind: Dictionary = {}
# bone index int -> neutral fitted world rest Transform3D
var _bone_rest_world: Dictionary = {}
# bone index int -> wiring record Dictionary
var _bone_wires: Dictionary = {}


func _ready() -> void:
	_ragdoll = get_parent() as Node3D
	_make_status_overlay()
	set_physics_process(false)
	call_deferred("_late_initialize")


func _unhandled_input(event: InputEvent) -> void:
	if not (event is InputEventKey):
		return
	var key_event: InputEventKey = event as InputEventKey
	if key_event.pressed and not key_event.echo and key_event.keycode == TOGGLE_KEY:
		_set_character_enabled(not _character_enabled)


func _physics_process(_delta: float) -> void:
	if not _initialized or not _character_enabled:
		return
	_drive_full_skeleton()


func _late_initialize() -> void:
	if _ragdoll == null:
		_set_status("C: character unavailable — ragdoll root missing")
		return
	if not _collect_ragdoll_bodies():
		# Parent creates the bodies in _ready(); defer once more if this child was
		# initialized before the parent's procedural ragdoll construction finished.
		call_deferred("_late_initialize")
		return

	var instance: Node = CHARACTER_SCENE.instantiate()
	_character_root = instance as Node3D
	if _character_root == null:
		_set_status("C: character unavailable — GLB root is not Node3D")
		instance.queue_free()
		return

	_character_root.name = "AsterraCharacterOverlay"
	add_child(_character_root)
	_skeleton = _find_skeleton(_character_root)
	if _skeleton == null:
		_set_status("C: character unavailable — no Skeleton3D in GLB")
		_character_root.queue_free()
		_character_root = null
		return

	_skeleton.reset_bone_poses()
	_resolve_anchor_bones()
	if not _has_minimum_fit_anchors():
		_set_status(
			"C: mapping failed — need pelvis/head/feet + side landmarks (%d/%d anchors found)"
			% [_anchor_bones.size(), EXPECTED_ANCHORS]
		)
		_character_root.visible = false
		return

	_fit_character_to_ragdoll()
	_capture_neutral_rest_and_bind_offsets()
	_build_full_bone_wiring()

	_character_root.visible = false
	_initialized = true
	set_physics_process(true)
	_update_status()
	_print_mapping_report()


func _collect_ragdoll_bodies() -> bool:
	_bodies.clear()
	for semantic: String in SEMANTICS:
		var node: Node = _ragdoll.get_node_or_null(NodePath(semantic))
		var body: RigidBody3D = node as RigidBody3D
		if body == null:
			return false
		_bodies[semantic] = body
	return _bodies.size() == EXPECTED_ANCHORS


func _find_skeleton(node: Node) -> Skeleton3D:
	if node is Skeleton3D:
		return node as Skeleton3D
	for child: Node in node.get_children():
		var found: Skeleton3D = _find_skeleton(child)
		if found != null:
			return found
	return null


func _resolve_anchor_bones() -> void:
	_anchor_bones.clear()
	_semantic_by_bone.clear()
	var used: Dictionary = {}

	# Resolve the unambiguous named landmarks first.
	var priority: Array[String] = [
		"head", "neck", "pelvis",
		"left_hand", "right_hand", "left_foot", "right_foot",
		"left_forearm", "right_forearm", "left_upper_arm", "right_upper_arm",
		"left_shin", "right_shin", "left_thigh", "right_thigh",
		"left_clavicle", "right_clavicle",
	]
	for semantic: String in priority:
		var bone_idx: int = _find_best_named_bone(semantic, used)
		if bone_idx >= 0:
			_register_anchor(semantic, bone_idx, used)

	# Generic rigs often call the torso simply spine/spine.001/... . Fill missing
	# pelvis/spine/chest from the actual hierarchy instead of guessing a number.
	_resolve_torso_from_hierarchy(used)

	# Likewise, fill any missing arm/leg landmarks from the chain between an
	# already-resolved distal bone and the torso/pelvis.
	_resolve_limb_from_hierarchy("left", used)
	_resolve_limb_from_hierarchy("right", used)


func _register_anchor(semantic: String, bone_idx: int, used: Dictionary) -> void:
	if bone_idx < 0 or used.has(bone_idx):
		return
	_anchor_bones[semantic] = bone_idx
	_semantic_by_bone[bone_idx] = semantic
	used[bone_idx] = true


func _find_best_named_bone(semantic: String, used: Dictionary) -> int:
	var best_idx: int = -1
	var best_score: int = -100000
	var bone_count: int = _skeleton.get_bone_count()
	for bone_idx: int in range(bone_count):
		if used.has(bone_idx):
			continue
		var bone_name: String = _skeleton.get_bone_name(bone_idx)
		var score: int = _name_score(semantic, bone_name)
		if score > best_score:
			best_score = score
			best_idx = bone_idx
	# Require meaningful evidence. Hierarchy fallback handles generic names.
	return best_idx if best_score >= 55 else -1


func _name_score(semantic: String, raw_name: String) -> int:
	var name: String = _normalize_bone_name(raw_name)
	var side: int = _name_side(raw_name)
	var wanted_side: int = _semantic_side(semantic)
	if wanted_side != 0 and side != 0 and side != wanted_side:
		return -10000

	var score: int = 0
	if wanted_side != 0 and side == wanted_side:
		score += 35

	var tokens: Array[String] = _semantic_tokens(semantic)
	for token: String in tokens:
		if name == token:
			score = maxi(score, 150)
		elif name.contains(token):
			score = maxi(score, 90 + mini(token.length(), 20))

	# Reject common helper mechanisms as direct landmarks when a deform bone exists.
	if name.contains("ik") or name.contains("pole") or name.contains("target"):
		score -= 80
	if name.contains("twist") or name.contains("roll"):
		score -= 25
	if raw_name.to_lower().contains("mch") or raw_name.to_lower().contains("org"):
		score -= 20
	return score


func _normalize_bone_name(raw_name: String) -> String:
	var value: String = raw_name.to_lower()
	for prefix: String in ["mixamorig", "def", "org", "mch", "bone"]:
		value = value.replace(prefix, "")
	for separator: String in [".", "_", "-", " ", ":"]:
		value = value.replace(separator, "")
	value = value.replace("left", "l")
	value = value.replace("right", "r")
	return value


func _name_side(raw_name: String) -> int:
	var n: String = raw_name.to_lower()
	if n.contains("left") or n.ends_with(".l") or n.ends_with("_l") or n.ends_with("-l"):
		return -1
	if n.contains("right") or n.ends_with(".r") or n.ends_with("_r") or n.ends_with("-r"):
		return 1
	var compact: String = _normalize_bone_name(raw_name)
	if compact.begins_with("l"):
		return -1
	if compact.begins_with("r"):
		return 1
	return 0


func _semantic_side(semantic: String) -> int:
	if semantic.begins_with("left_"):
		return -1
	if semantic.begins_with("right_"):
		return 1
	return 0


func _semantic_tokens(semantic: String) -> Array[String]:
	match semantic:
		"pelvis":
			return ["pelvis", "hips", "hiproot", "rootpelvis"]
		"spine":
			return ["spine", "lowerback", "abdomen", "torso"]
		"chest":
			return ["chest", "upperchest", "upperback", "thorax", "spine2", "spine3", "spine03"]
		"neck":
			return ["neck", "cervical"]
		"head":
			return ["head", "skull"]
		"left_clavicle", "right_clavicle":
			return ["clavicle", "collar", "shoulder"]
		"left_upper_arm", "right_upper_arm":
			return ["upperarm", "uparm", "humerus", "arm"]
		"left_forearm", "right_forearm":
			return ["forearm", "lowerarm", "radius", "ulna"]
		"left_hand", "right_hand":
			return ["hand", "wrist"]
		"left_thigh", "right_thigh":
			return ["thigh", "upperleg", "upleg", "femur"]
		"left_shin", "right_shin":
			return ["shin", "lowerleg", "calf", "tibia", "leg"]
		"left_foot", "right_foot":
			return ["foot", "ankle"]
	return []


func _resolve_torso_from_hierarchy(used: Dictionary) -> void:
	var head_idx: int = _anchor_index("head")
	if head_idx < 0:
		return
	var chain: Array[int] = []
	var cursor: int = head_idx
	while cursor >= 0:
		chain.push_front(cursor)
		cursor = _skeleton.get_bone_parent(cursor)
	if chain.size() < 4:
		return

	var neck_pos: int = chain.size() - 2
	if _anchor_index("neck") >= 0:
		neck_pos = chain.find(_anchor_index("neck"))
		if neck_pos < 0:
			neck_pos = chain.size() - 2

	var torso_end: int = maxi(1, neck_pos - 1)
	var torso_candidates: Array[int] = []
	for i: int in range(torso_end + 1):
		var idx: int = chain[i]
		var n: String = _normalize_bone_name(_skeleton.get_bone_name(idx))
		if n.contains("spine") or n.contains("pelvis") or n.contains("hip") or n.contains("chest") or n.contains("back"):
			torso_candidates.append(idx)
	if torso_candidates.size() < 3:
		return

	if _anchor_index("pelvis") < 0:
		_register_anchor("pelvis", torso_candidates[0], used)
	if _anchor_index("spine") < 0:
		_register_anchor("spine", torso_candidates[int(torso_candidates.size() / 2)], used)
	if _anchor_index("chest") < 0:
		_register_anchor("chest", torso_candidates[torso_candidates.size() - 1], used)


func _resolve_limb_from_hierarchy(side_name: String, used: Dictionary) -> void:
	var hand_semantic: String = side_name + "_hand"
	var forearm_semantic: String = side_name + "_forearm"
	var upper_arm_semantic: String = side_name + "_upper_arm"
	var clavicle_semantic: String = side_name + "_clavicle"
	var foot_semantic: String = side_name + "_foot"
	var shin_semantic: String = side_name + "_shin"
	var thigh_semantic: String = side_name + "_thigh"

	var hand_idx: int = _anchor_index(hand_semantic)
	if hand_idx >= 0:
		var arm_chain: Array[int] = _ancestor_chain_until(hand_idx, _anchor_index("chest"))
		if _anchor_index(forearm_semantic) < 0 and arm_chain.size() >= 2:
			_register_anchor(forearm_semantic, arm_chain[1], used)
		if _anchor_index(upper_arm_semantic) < 0 and arm_chain.size() >= 3:
			_register_anchor(upper_arm_semantic, arm_chain[2], used)
		if _anchor_index(clavicle_semantic) < 0 and arm_chain.size() >= 4:
			_register_anchor(clavicle_semantic, arm_chain[3], used)

	var foot_idx: int = _anchor_index(foot_semantic)
	if foot_idx >= 0:
		var leg_chain: Array[int] = _ancestor_chain_until(foot_idx, _anchor_index("pelvis"))
		if _anchor_index(shin_semantic) < 0 and leg_chain.size() >= 2:
			_register_anchor(shin_semantic, leg_chain[1], used)
		if _anchor_index(thigh_semantic) < 0 and leg_chain.size() >= 3:
			_register_anchor(thigh_semantic, leg_chain[2], used)


func _ancestor_chain_until(start_idx: int, stop_idx: int) -> Array[int]:
	var chain: Array[int] = []
	var cursor: int = start_idx
	while cursor >= 0 and cursor != stop_idx:
		chain.append(cursor)
		cursor = _skeleton.get_bone_parent(cursor)
	return chain


func _anchor_index(semantic: String) -> int:
	if not _anchor_bones.has(semantic):
		return -1
	return int(_anchor_bones[semantic])


func _has_minimum_fit_anchors() -> bool:
	for semantic: String in ["pelvis", "head", "left_foot", "right_foot", "left_upper_arm", "right_upper_arm"]:
		if _anchor_index(semantic) < 0:
			return false
	return true


func _fit_character_to_ragdoll() -> void:
	var source_pelvis: Vector3 = _bone_rest_world_now(_anchor_index("pelvis")).origin
	var source_head: Vector3 = _bone_rest_world_now(_anchor_index("head")).origin
	var source_left_foot: Vector3 = _bone_rest_world_now(_anchor_index("left_foot")).origin
	var source_right_foot: Vector3 = _bone_rest_world_now(_anchor_index("right_foot")).origin
	var source_left_arm: Vector3 = _bone_rest_world_now(_anchor_index("left_upper_arm")).origin
	var source_right_arm: Vector3 = _bone_rest_world_now(_anchor_index("right_upper_arm")).origin
	var source_feet_mid: Vector3 = (source_left_foot + source_right_foot) * 0.5

	var target_pelvis: Vector3 = _body("pelvis").global_position
	var target_head: Vector3 = _body("head").global_position
	var target_left_foot: Vector3 = _body("left_foot").global_position
	var target_right_foot: Vector3 = _body("right_foot").global_position
	var target_left_arm: Vector3 = _body("left_upper_arm").global_position
	var target_right_arm: Vector3 = _body("right_upper_arm").global_position
	var target_feet_mid: Vector3 = (target_left_foot + target_right_foot) * 0.5

	var source_height: float = source_feet_mid.distance_to(source_head)
	var target_height: float = target_feet_mid.distance_to(target_head)
	var scale_factor: float = target_height / maxf(source_height, 0.001)

	var source_basis: Basis = _frame_from_up_and_right(
		source_head - source_pelvis,
		source_right_arm - source_left_arm
	)
	var target_basis: Basis = _frame_from_up_and_right(
		target_head - target_pelvis,
		target_right_arm - target_left_arm
	)
	var rotation_basis: Basis = (target_basis * source_basis.inverse()).orthonormalized()
	var fitted_basis: Basis = rotation_basis.scaled(Vector3.ONE * scale_factor)
	var delta: Transform3D = Transform3D(
		fitted_basis,
		target_pelvis - fitted_basis * source_pelvis
	)
	_character_root.global_transform = delta * _character_root.global_transform


func _frame_from_up_and_right(up_hint: Vector3, right_hint: Vector3) -> Basis:
	var y_axis: Vector3 = up_hint.normalized()
	var x_axis: Vector3 = right_hint - y_axis * right_hint.dot(y_axis)
	if x_axis.length_squared() < 0.000001:
		x_axis = Vector3.RIGHT
	x_axis = x_axis.normalized()
	var z_axis: Vector3 = x_axis.cross(y_axis).normalized()
	y_axis = z_axis.cross(x_axis).normalized()
	return Basis(x_axis, y_axis, z_axis).orthonormalized()


func _bone_rest_world_now(bone_idx: int) -> Transform3D:
	return _skeleton.global_transform * _skeleton.get_bone_global_rest(bone_idx)


func _capture_neutral_rest_and_bind_offsets() -> void:
	_bone_rest_world.clear()
	_body_to_bone_bind.clear()
	var bone_count: int = _skeleton.get_bone_count()
	for bone_idx: int in range(bone_count):
		_bone_rest_world[bone_idx] = _bone_rest_world_now(bone_idx)

	for semantic_variant: Variant in _anchor_bones.keys():
		var semantic: String = String(semantic_variant)
		var bone_idx: int = int(_anchor_bones[semantic])
		var rest_world: Transform3D = _bone_rest_world[bone_idx]
		var body: RigidBody3D = _body(semantic)
		_body_to_bone_bind[semantic] = body.global_transform.affine_inverse() * rest_world


func _build_full_bone_wiring() -> void:
	_bone_wires.clear()
	var bone_count: int = _skeleton.get_bone_count()
	for bone_idx: int in range(bone_count):
		if _semantic_by_bone.has(bone_idx):
			_bone_wires[bone_idx] = {
				"mode": "anchor",
				"semantic": String(_semantic_by_bone[bone_idx]),
			}
			continue

		var ancestor_idx: int = _nearest_anchor_ancestor(bone_idx)
		var descendant_idx: int = _nearest_anchor_descendant(bone_idx)
		if ancestor_idx >= 0 and descendant_idx >= 0 and ancestor_idx != descendant_idx:
			var alpha: float = _chain_fraction(ancestor_idx, bone_idx, descendant_idx)
			if alpha >= 0.0:
				var rest_a: Transform3D = _bone_rest_world[ancestor_idx]
				var rest_b: Transform3D = _bone_rest_world[descendant_idx]
				var neutral_blend: Transform3D = _blend_transform(rest_a, rest_b, alpha)
				var rest_bone: Transform3D = _bone_rest_world[bone_idx]
				_bone_wires[bone_idx] = {
					"mode": "between",
					"a": ancestor_idx,
					"b": descendant_idx,
					"alpha": alpha,
					"offset": neutral_blend.affine_inverse() * rest_bone,
				}
				continue

		# Explicitly record inherited bones too. Fingers, toes, facial helpers, etc.
		# retain their imported local rest transform and follow the closest driven
		# ancestor through normal Skeleton3D hierarchy propagation.
		_bone_wires[bone_idx] = {
			"mode": "inherit",
			"ancestor": ancestor_idx,
		}


func _nearest_anchor_ancestor(bone_idx: int) -> int:
	var cursor: int = _skeleton.get_bone_parent(bone_idx)
	while cursor >= 0:
		if _semantic_by_bone.has(cursor):
			return cursor
		cursor = _skeleton.get_bone_parent(cursor)
	return -1


func _nearest_anchor_descendant(bone_idx: int) -> int:
	var best_idx: int = -1
	var best_depth: int = 1000000
	for mapped_variant: Variant in _semantic_by_bone.keys():
		var mapped_idx: int = int(mapped_variant)
		var depth: int = _descendant_depth(bone_idx, mapped_idx)
		if depth >= 1 and depth < best_depth:
			best_depth = depth
			best_idx = mapped_idx
	return best_idx


func _descendant_depth(ancestor_idx: int, candidate_idx: int) -> int:
	var depth: int = 0
	var cursor: int = candidate_idx
	while cursor >= 0:
		if cursor == ancestor_idx:
			return depth
		cursor = _skeleton.get_bone_parent(cursor)
		depth += 1
	return -1


func _chain_fraction(ancestor_idx: int, middle_idx: int, descendant_idx: int) -> float:
	if _descendant_depth(ancestor_idx, middle_idx) < 0 or _descendant_depth(middle_idx, descendant_idx) < 0:
		return -1.0
	var total: float = _rest_chain_distance(ancestor_idx, descendant_idx)
	var partial: float = _rest_chain_distance(ancestor_idx, middle_idx)
	if total <= 0.000001:
		return 0.0
	return clampf(partial / total, 0.0, 1.0)


func _rest_chain_distance(ancestor_idx: int, descendant_idx: int) -> float:
	var distance: float = 0.0
	var cursor: int = descendant_idx
	while cursor >= 0 and cursor != ancestor_idx:
		var parent_idx: int = _skeleton.get_bone_parent(cursor)
		if parent_idx < 0:
			return 0.0
		var child_rest: Transform3D = _bone_rest_world[cursor]
		var parent_rest: Transform3D = _bone_rest_world[parent_idx]
		distance += child_rest.origin.distance_to(parent_rest.origin)
		cursor = parent_idx
	return distance if cursor == ancestor_idx else 0.0


func _drive_full_skeleton() -> void:
	_skeleton.reset_bone_poses()

	# Build current world targets for all directly physical anchor bones first.
	var anchor_targets: Dictionary = {}
	for semantic_variant: Variant in _anchor_bones.keys():
		var semantic: String = String(semantic_variant)
		if not _body_to_bone_bind.has(semantic):
			continue
		var body: RigidBody3D = _body(semantic)
		var bind: Transform3D = _body_to_bone_bind[semantic]
		anchor_targets[int(_anchor_bones[semantic])] = body.global_transform * bind

	# Skeleton bone indices are parent-before-child. Applying global poses in this
	# order keeps hierarchy recalculation deterministic.
	var bone_count: int = _skeleton.get_bone_count()
	for bone_idx: int in range(bone_count):
		var wire: Dictionary = _bone_wires[bone_idx]
		var mode: String = String(wire["mode"])
		var target_world: Transform3D
		var should_apply: bool = false

		if mode == "anchor":
			if anchor_targets.has(bone_idx):
				target_world = anchor_targets[bone_idx]
				should_apply = true
		elif mode == "between":
			var a: int = int(wire["a"])
			var b: int = int(wire["b"])
			if anchor_targets.has(a) and anchor_targets.has(b):
				var alpha: float = float(wire["alpha"])
				var offset: Transform3D = wire["offset"]
				var blend: Transform3D = _blend_transform(anchor_targets[a], anchor_targets[b], alpha)
				target_world = blend * offset
				should_apply = true

		if should_apply:
			var skeleton_pose: Transform3D = _skeleton.global_transform.affine_inverse() * target_world
			_skeleton.set_bone_global_pose(bone_idx, skeleton_pose)


func _blend_transform(a: Transform3D, b: Transform3D, alpha: float) -> Transform3D:
	var qa: Quaternion = a.basis.orthonormalized().get_rotation_quaternion()
	var qb: Quaternion = b.basis.orthonormalized().get_rotation_quaternion()
	var rotation: Quaternion = qa.slerp(qb, clampf(alpha, 0.0, 1.0)).normalized()
	var position: Vector3 = a.origin.lerp(b.origin, clampf(alpha, 0.0, 1.0))
	return Transform3D(Basis(rotation), position)


func _body(semantic: String) -> RigidBody3D:
	return _bodies[semantic] as RigidBody3D


func _set_character_enabled(enabled: bool) -> void:
	_character_enabled = enabled and _initialized
	if _character_root != null:
		_character_root.visible = _character_enabled
	if _character_enabled:
		_drive_full_skeleton()
	_update_status()


func _make_status_overlay() -> void:
	var canvas: CanvasLayer = CanvasLayer.new()
	canvas.name = "CharacterOverlayStatus"
	add_child(canvas)
	_status_label = Label.new()
	_status_label.position = Vector2(18.0, 92.0)
	_status_label.add_theme_font_size_override("font_size", 15)
	canvas.add_child(_status_label)
	_set_status("C: character loading…")


func _set_status(text: String) -> void:
	if _status_label != null:
		_status_label.text = text


func _update_status() -> void:
	if not _initialized:
		return
	var state: String = "ON" if _character_enabled else "OFF"
	_set_status(
		"C: Asterra character %s   anchors: %d/%d   full skeleton: %d bones wired"
		% [state, _anchor_bones.size(), EXPECTED_ANCHORS, _skeleton.get_bone_count()]
	)


func _print_mapping_report() -> void:
	print("Asterra character overlay: %d imported bones, %d/%d physical anchors" % [
		_skeleton.get_bone_count(), _anchor_bones.size(), EXPECTED_ANCHORS
	])
	for semantic: String in SEMANTICS:
		if _anchor_bones.has(semantic):
			var bone_idx: int = int(_anchor_bones[semantic])
			print("  %s -> %s [%d]" % [semantic, _skeleton.get_bone_name(bone_idx), bone_idx])
		else:
			print("  %s -> MISSING" % semantic)
