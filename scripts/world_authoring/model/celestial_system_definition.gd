class_name CelestialSystemDefinition
extends Resource
## Versioned top-level object saved by Planet Studio.

const BODY_SCRIPT := preload("res://scripts/world_authoring/model/celestial_body_definition.gd")
const SCHEMA_VERSION: int = 2

@export var schema_version: int = SCHEMA_VERSION
@export var system_id: String = "asterra-system"
@export var display_name: String = "Asterra System"
@export var bodies: Array[Resource] = []
@export var active_body_id: String = ""

func ensure_valid() -> void:
	schema_version = maxi(schema_version, SCHEMA_VERSION)
	for body: Resource in bodies:
		if body != null and body.has_method("ensure_children"):
			body.call("ensure_children")
	if active_body_id.is_empty() and not bodies.is_empty() and bodies[0] != null:
		active_body_id = String(bodies[0].get(&"body_id"))
	if find_body(active_body_id) == null and not bodies.is_empty():
		active_body_id = String(bodies[0].get(&"body_id"))
	_repair_parent_hierarchy()

func find_body(search_body_id: String) -> Resource:
	for body: Resource in bodies:
		if body != null and String(body.get(&"body_id")) == search_body_id:
			return body
	return null

func active_body() -> Resource:
	return find_body(active_body_id)

func add_body(body: Resource) -> void:
	if body == null:
		return
	if body.has_method("ensure_children"):
		body.call("ensure_children")
	var candidate_id := String(body.get(&"body_id"))
	if find_body(candidate_id) != null:
		body.set(&"body_id", BODY_SCRIPT.make_body_id(String(body.get(&"display_name"))))
	bodies.append(body)
	if active_body_id.is_empty():
		active_body_id = String(body.get(&"body_id"))
	var parent_id := String(body.get(&"parent_body_id"))
	if not can_parent_body(String(body.get(&"body_id")), parent_id):
		body.set(&"parent_body_id", "")

func remove_body(remove_body_id: String) -> bool:
	for index: int in bodies.size():
		var body: Resource = bodies[index]
		if body != null and String(body.get(&"body_id")) == remove_body_id:
			bodies.remove_at(index)
			for child: Resource in bodies:
				if child != null and String(child.get(&"parent_body_id")) == remove_body_id:
					child.set(&"parent_body_id", "")
			if active_body_id == remove_body_id:
				active_body_id = String(bodies[0].get(&"body_id")) if not bodies.is_empty() else ""
			return true
	return false

func can_parent_body(body_id: String, candidate_parent_id: String) -> bool:
	if body_id.is_empty():
		return false
	if candidate_parent_id.is_empty():
		return true
	if body_id == candidate_parent_id:
		return false
	if find_body(body_id) == null or find_body(candidate_parent_id) == null:
		return false
	var cursor_id := candidate_parent_id
	var visited: Dictionary = {}
	while not cursor_id.is_empty():
		if cursor_id == body_id or visited.has(cursor_id):
			return false
		visited[cursor_id] = true
		var cursor: Resource = find_body(cursor_id)
		if cursor == null:
			break
		cursor_id = String(cursor.get(&"parent_body_id"))
	return true

func set_parent_body(body_id: String, candidate_parent_id: String) -> bool:
	if not can_parent_body(body_id, candidate_parent_id):
		return false
	var body := find_body(body_id)
	if body == null:
		return false
	body.set(&"parent_body_id", candidate_parent_id)
	return true

func children_of(parent_body_id: String) -> Array[Resource]:
	var result: Array[Resource] = []
	for body: Resource in bodies:
		if body != null and String(body.get(&"parent_body_id")) == parent_body_id:
			result.append(body)
	return result

func _repair_parent_hierarchy() -> void:
	for body: Resource in bodies:
		if body == null:
			continue
		var body_id := String(body.get(&"body_id"))
		var parent_id := String(body.get(&"parent_body_id"))
		if parent_id.is_empty():
			continue
		if not can_parent_body(body_id, parent_id):
			body.set(&"parent_body_id", "")
