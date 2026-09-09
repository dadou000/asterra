class_name WaterPointSourceDefinition
extends Resource
## Serialized Planet Studio source for the production sparse hydrology runtime.

@export var source_id: String = ""
@export var display_name: String = "Water Source"
@export var enabled: bool = false
@export var direction: Vector3 = Vector3.UP
@export var rate_m3_s: float = 10.0
@export var injection_velocity_world: Vector3 = Vector3.ZERO
@export var tile_level: int = -1

func ensure_valid() -> void:
	if source_id.is_empty():
		source_id = make_source_id(display_name)
	if not _finite_vec3(direction) or direction.length_squared() < 1.0e-10:
		direction = Vector3.UP
	else:
		direction = direction.normalized()
	if not is_finite(rate_m3_s):
		rate_m3_s = 0.0
	if not _finite_vec3(injection_velocity_world):
		injection_velocity_world = Vector3.ZERO
	tile_level = maxi(-1, tile_level)

static func make_source_id(label: String) -> String:
	var safe := label.strip_edges().to_lower().replace(" ", "-")
	if safe.is_empty():
		safe = "water-source"
	return "%s-%d-%d" % [safe, Time.get_ticks_usec(), randi() & 0x7fffffff]

static func _finite_vec3(value: Vector3) -> bool:
	return is_finite(value.x) and is_finite(value.y) and is_finite(value.z)
