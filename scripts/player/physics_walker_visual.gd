class_name PhysicsWalkerVisual
extends Node3D
## Loads the existing Asterra humanoid and attaches it to PhysicsWalkerBody.
## This first pass deliberately keeps the imported skeleton in its authored pose;
## locomotion is evaluated on the body dynamics first. Bone-level active-ragdoll
## motors are the next layer once the COM/contact controller is stable.

const CHARACTER_PATH := "res://assets/character/asterrahuman.glb"

@export var body: PhysicsWalkerBody
@export var visible_character := true

var character: Node3D
var model_forward_correction_deg := 0.0
var model_height := 1.75


func _ready() -> void:
	_load_character()


func _process(_dt: float) -> void:
	if body == null or character == null:
		return
	visible = visible_character and body.active
	if not body.active:
		return
	global_transform = body.global_transform


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
