class_name CelestialSystemDefinition
extends Resource
## Versioned top-level object saved by Planet Studio.

const SCHEMA_VERSION: int = 1

@export var schema_version: int = SCHEMA_VERSION
@export var system_id: String = "asterra-system"
@export var display_name: String = "Asterra System"
@export var bodies: Array[CelestialBodyDefinition] = []
@export var active_body_id: String = ""

func ensure_valid() -> void:
	schema_version = maxi(schema_version, 1)
	for body: CelestialBodyDefinition in bodies:
		if body != null:
			body.ensure_children()
	if active_body_id.is_empty() and not bodies.is_empty() and bodies[0] != null:
		active_body_id = bodies[0].body_id
	if find_body(active_body_id) == null and not bodies.is_empty():
		active_body_id = bodies[0].body_id

func find_body(body_id: String) -> CelestialBodyDefinition:
	for body: CelestialBodyDefinition in bodies:
		if body != null and body.body_id == body_id:
			return body
	return null

func active_body() -> CelestialBodyDefinition:
	return find_body(active_body_id)

func add_body(body: CelestialBodyDefinition) -> void:
	if body == null:
		return
	body.ensure_children()
	if find_body(body.body_id) != null:
		body.body_id = CelestialBodyDefinition.make_body_id(body.display_name)
	bodies.append(body)
	if active_body_id.is_empty():
		active_body_id = body.body_id

func remove_body(body_id: String) -> bool:
	for index: int in bodies.size():
		var body: CelestialBodyDefinition = bodies[index]
		if body != null and body.body_id == body_id:
			bodies.remove_at(index)
			for child: CelestialBodyDefinition in bodies:
				if child != null and child.parent_body_id == body_id:
					child.parent_body_id = ""
			if active_body_id == body_id:
				active_body_id = bodies[0].body_id if not bodies.is_empty() else ""
			return true
	return false
