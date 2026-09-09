extends SceneTree
## Authoring-only headless extractor for the canonical Asterra humanoid.
##
## Usage:
## godot --headless --path . --script res://experiments/locomotion_19body/scripts/export_character_manifest.gd -- \
##   --output=res://experiments/locomotion_19body/generated/asterrahuman_skeleton_manifest.json

const CHARACTER_PATH := "res://assets/character/asterrahuman.glb"
const DEFAULT_OUTPUT := "res://experiments/locomotion_19body/generated/asterrahuman_skeleton_manifest.json"


func _initialize() -> void:
	call_deferred("_run")


func _run() -> void:
	var output_path := _output_path_from_args()
	var packed := load(CHARACTER_PATH) as PackedScene
	if packed == null:
		_fail("Could not load canonical character: %s" % CHARACTER_PATH)
		return

	var instance := packed.instantiate()
	if not (instance is Node3D):
		instance.queue_free()
		_fail("Canonical character root is not Node3D")
		return

	root.add_child(instance)
	var character := instance as Node3D
	var skeleton := _find_skeleton(character)
	if skeleton == null:
		character.queue_free()
		_fail("No Skeleton3D found in %s" % CHARACTER_PATH)
		return

	var bounds := _character_bounds(character)
	var bones: Array = []
	for bone in skeleton.get_bone_count():
		var parent := skeleton.get_bone_parent(bone)
		var local_rest := skeleton.get_bone_rest(bone)
		var global_rest := skeleton.get_bone_global_rest(bone)
		bones.append({
			"index": bone,
			"name": String(skeleton.get_bone_name(bone)),
			"parent_index": parent,
			"parent_name": String(skeleton.get_bone_name(parent)) if parent >= 0 else "",
			"local_rest": _transform_dict(local_rest),
			"global_rest": _transform_dict(global_rest),
			"global_rest_origin": _v3(global_rest.origin),
			"length_to_parent": global_rest.origin.distance_to(skeleton.get_bone_global_rest(parent).origin) if parent >= 0 else 0.0,
		})

	var manifest := {
		"schema_version": 1,
		"source": {
			"godot_path": CHARACTER_PATH,
			"role": "canonical_character_editor_humanoid",
		},
		"coordinate_system": {
			"godot_up": "+Y",
			"godot_forward": "-Z",
			"character_forward": "derive_from_skeleton_and_mesh",
		},
		"skeleton": {
			"node_path": String(character.get_path_to(skeleton)),
			"bone_count": skeleton.get_bone_count(),
			"bones": bones,
		},
		"character_bounds": bounds,
	}

	var absolute_output := ProjectSettings.globalize_path(output_path)
	var output_dir := absolute_output.get_base_dir()
	var dir_error := DirAccess.make_dir_recursive_absolute(output_dir)
	if dir_error != OK and dir_error != ERR_ALREADY_EXISTS:
		character.queue_free()
		_fail("Could not create output directory %s (error %d)" % [output_dir, dir_error])
		return

	var file := FileAccess.open(output_path, FileAccess.WRITE)
	if file == null:
		character.queue_free()
		_fail("Could not open manifest output: %s" % output_path)
		return
	file.store_string(JSON.stringify(manifest, "  "))
	file.store_string("\n")
	file.close()

	print("Asterra character manifest written: %s" % output_path)
	print("  skeleton: %s" % String(character.get_path_to(skeleton)))
	print("  bones: %d" % skeleton.get_bone_count())
	print("  bounds size: %s" % str(bounds["size"]))
	character.queue_free()
	quit(0)


func _output_path_from_args() -> String:
	for arg in OS.get_cmdline_user_args():
		var value := String(arg)
		if value.begins_with("--output="):
			return value.trim_prefix("--output=")
	return DEFAULT_OUTPUT


func _find_skeleton(node: Node) -> Skeleton3D:
	if node is Skeleton3D:
		return node as Skeleton3D
	for child in node.get_children():
		var found := _find_skeleton(child)
		if found != null:
			return found
	return null


func _character_bounds(character: Node3D) -> Dictionary:
	var meshes: Array[MeshInstance3D] = []
	_collect_meshes(character, meshes)
	var minimum := Vector3(INF, INF, INF)
	var maximum := Vector3(-INF, -INF, -INF)
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
					var world_point := mesh.to_global(local_point)
					var point := character.to_local(world_point)
					minimum.x = minf(minimum.x, point.x)
					minimum.y = minf(minimum.y, point.y)
					minimum.z = minf(minimum.z, point.z)
					maximum.x = maxf(maximum.x, point.x)
					maximum.y = maxf(maximum.y, point.y)
					maximum.z = maxf(maximum.z, point.z)
					found = true

	if not found:
		return {
			"valid": false,
			"mesh_count": meshes.size(),
			"min": _v3(Vector3.ZERO),
			"max": _v3(Vector3.ZERO),
			"size": _v3(Vector3.ZERO),
		}

	return {
		"valid": true,
		"mesh_count": meshes.size(),
		"min": _v3(minimum),
		"max": _v3(maximum),
		"size": _v3(maximum - minimum),
	}


func _collect_meshes(node: Node, out: Array[MeshInstance3D]) -> void:
	if node is MeshInstance3D:
		out.append(node as MeshInstance3D)
	for child in node.get_children():
		_collect_meshes(child, out)


func _transform_dict(transform: Transform3D) -> Dictionary:
	var q := transform.basis.orthonormalized().get_rotation_quaternion()
	return {
		"origin": _v3(transform.origin),
		"basis_x": _v3(transform.basis.x),
		"basis_y": _v3(transform.basis.y),
		"basis_z": _v3(transform.basis.z),
		"rotation_xyzw": [q.x, q.y, q.z, q.w],
	}


func _v3(value: Vector3) -> Array:
	return [value.x, value.y, value.z]


func _fail(message: String) -> void:
	push_error(message)
	printerr(message)
	quit(1)
