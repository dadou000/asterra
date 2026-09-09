class_name WaterPointSourceDefinition
extends Resource
## Serialized Planet Studio point source for the sparse hydrology runtime.
##
## The physical ingress API is direction-based: the terrain bed determines the
## actual source elevation/cell when the sparse tile is activated. Keeping the
## authoring definition in canonical body-space direction also makes it stable
## across floating-origin shifts and editor camera movement.

@export var source_id: String = ""
@export var display_name: String = "Water Source"
@export var enabled: bool = true
@export var direction: Vector3 = Vector3.UP
@export var rate_m3_s: float = 10.0
@export var injection_velocity_world: Vector3 = Vector3.ZERO
@export var tile_level: int = -1


func ensure_valid() -> void:
	if source_id.is_empty():
		source_id = make_source_id(display_name)
	if direction.length_squared() < 1.0e-10 or not _finite_vec3(direction):
		direction = Vector3.UP
	else:
		direction = direction.normalized()
	if not is_finite(rate_m3_s):
		rate_m3_s = 0.0
	if not _finite_vec3(injection_velocity_world):
		injection_velocity_world = Vector3.ZERO
	# -1 asks WaterSystem to use the runtime metric-compatible default level.
	tile_level = maxi(-1, tile_level)


static func make_source_id(label: String) -> String:
	var safe := label.strip_edges().to_lower().replace(" ", "-")
	if safe.is_empty():
		safe = "water-source"
	return "%s-%d-%d" % [safe, Time.get_ticks_usec(), randi() & 0x7fffffff]


static func _finite_vec3(value: Vector3) -> bool:
	return is_finite(value.x) and is_finite(value.y) and is_finite(value.z)
