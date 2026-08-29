class_name CelestialSystemDefinition
extends Resource
## Versioned top-level object saved by Planet Studio.

const BODY_SCRIPT := preload("res://scripts/world_authoring/model/celestial_body_definition.gd")
const SCHEMA_VERSION: int = 1

@export var schema_version: int = SCHEMA_VERSION
@export var system_id: String = "asterra-system"
@export var display_name: String = "Asterra System"
@export var bodies: Array[Resource] = []
@export var active_body_id: String = ""

func ensure_valid() -> void:
	schema_version = maxi(schema_version, 1)
	for body: Resource in bodies:
		if body != null and body.has_method("ensure_children"):
			body.call("ensure_children")
	if active_body_id.is_empty() and not bodies.is_empty() and bodies[0] != null:
		active_body_id = String(bodies[0].get(&"body_id"))
	if find_body(active_body_id) == null and not bodies.is_empty():
		active_body_id = String(bodies[0].get(&"body_id"))

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
