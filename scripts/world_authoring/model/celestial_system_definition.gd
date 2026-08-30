class_name CelestialSystemDefinition
extends Resource
## Versioned top-level object saved by Planet Studio.

const BODY_SCRIPT := preload("res://scripts/world_authoring/model/celestial_body_definition.gd")
const SCHEMA_VERSION: int = 2
const DEFAULT_CHILD_ORBIT_PARENT_RADII: float = 4.0
const DEFAULT_CHILD_ORBIT_CHILD_RADII: float = 3.0
const DEFAULT_CHILD_ORBIT_ANOMALY_DEG: float = 35.0

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
	_repair_zero_child_orbits()

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
	else:
		_seed_default_child_orbit(body)

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
	_seed_default_child_orbit(body)
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

func _repair_zero_child_orbits() -> void:
	for body: Resource in bodies:
		_seed_default_child_orbit(body)

func _seed_default_child_orbit(body: Resource) -> void:
	if body == null:
		return
	var parent_id: String = String(body.get(&"parent_body_id"))
	if parent_id.is_empty():
		return
	var parent: Resource = find_body(parent_id)
	var orbit: Resource = body.get(&"orbit") as Resource
	if parent == null or orbit == null:
		return
	if float(orbit.get(&"semi_major_axis_m")) > 0.0:
		return
	var parent_radius: float = maxf(float(parent.get(&"radius_m")), 1.0)
	var child_radius: float = maxf(float(body.get(&"radius_m")), 1.0)
	orbit.set(&"semi_major_axis_m", maxf(
		parent_radius * DEFAULT_CHILD_ORBIT_PARENT_RADII,
		parent_radius + child_radius * DEFAULT_CHILD_ORBIT_CHILD_RADII))
	if absf(float(orbit.get(&"mean_anomaly_at_epoch_deg"))) <= 1e-6:
		orbit.set(&"mean_anomaly_at_epoch_deg", DEFAULT_CHILD_ORBIT_ANOMALY_DEG)
	if int(body.get(&"body_type")) == BODY_SCRIPT.BodyType.MOON \
			and absf(float(orbit.get(&"inclination_deg"))) <= 1e-6:
		orbit.set(&"inclination_deg", 5.0)
