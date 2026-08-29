extends Node
## Deprecated compatibility adapter for the former streamed CPU height-tile store.
##
## The authoritative terrain stack now uses GroundHeightStore for the resident
## coarse field and TerrainHeightQuery for precise GPU contact. Keeping the old
## implementation here created a static dependency back into the Planet autoload,
## which made Planet -> height-store -> Planet a GDScript parse cycle. Legacy
## callers are forwarded dynamically so this file no longer participates in the
## Planet script dependency graph.

signal tile_ready(level: int, face: int, tile_x: int, tile_y: int)

const TARGET_FINE_DEPTH: int = 16
const MAX_LEVEL: int = 14
const PRIORITY_COLLISION: float = -1000.0
const PRIORITY_VISIBLE: float = 0.0
const PRIORITY_PREFETCH: float = 50.0


func _ready() -> void:
	var store: Node = _store()
	if store != null and store.has_signal("tile_ready"):
		var relay := Callable(self, "_relay_tile_ready")
		if not store.is_connected("tile_ready", relay):
			store.connect("tile_ready", relay)


func _relay_tile_ready(level: int, face: int, tile_x: int, tile_y: int) -> void:
	tile_ready.emit(level, face, tile_x, tile_y)


func _store() -> Node:
	var tree: SceneTree = get_tree()
	if tree == null:
		return null
	return tree.root.get_node_or_null("GroundHeightStore")


func sample_height(d: Vector3, level: int, snap: Dictionary = {}) -> float:
	var store: Node = _store()
	if store == null or not store.has_method("sample_height"):
		return 0.0
	return float(store.call("sample_height", d, level, snap))


func sample_height_nonblocking(d: Vector3, level: int,
		snap: Dictionary = {}) -> float:
	var store: Node = _store()
	if store == null:
		return 0.0
	if store.has_method("sample_height_nonblocking"):
		return float(store.call("sample_height_nonblocking", d, level, snap))
	return sample_height(d, level, snap)


func sample_pristine(d: Vector3, level: int) -> float:
	var store: Node = _store()
	if store == null or not store.has_method("sample_pristine"):
		return 0.0
	return float(store.call("sample_pristine", d, level))


func sample_pristine_nonblocking(d: Vector3, level: int) -> float:
	var store: Node = _store()
	if store == null:
		return 0.0
	if store.has_method("sample_pristine_nonblocking"):
		return float(store.call("sample_pristine_nonblocking", d, level))
	return sample_pristine(d, level)


func prefetch_sample(d: Vector3, finest_level: int = 0,
		priority: float = PRIORITY_PREFETCH) -> void:
	var store: Node = _store()
	if store != null and store.has_method("prefetch_sample"):
		store.call("prefetch_sample", d, finest_level, priority)


func request_sample(d: Vector3, level: int,
		priority: float = PRIORITY_VISIBLE) -> void:
	var store: Node = _store()
	if store != null and store.has_method("request_sample"):
		store.call("request_sample", d, level, priority)


func request_samples(directions: Array[Vector3], level: int,
		priority: float = PRIORITY_VISIBLE) -> void:
	var store: Node = _store()
	if store != null and store.has_method("request_samples"):
		store.call("request_samples", directions, level, priority)


func request_samples_prioritized(directions: Array[Vector3], level: int,
		priorities: PackedFloat32Array) -> void:
	var store: Node = _store()
	if store != null and store.has_method("request_samples_prioritized"):
		store.call("request_samples_prioritized", directions, level, priorities)


func resident_tile(level: int, face: int, tile_x: int,
		tile_y: int) -> PackedFloat32Array:
	var store: Node = _store()
	if store == null or not store.has_method("resident_tile"):
		return PackedFloat32Array()
	var value: Variant = store.call("resident_tile", level, face, tile_x, tile_y)
	return value as PackedFloat32Array


func is_sample_resident(d: Vector3, level: int) -> bool:
	var store: Node = _store()
	if store == null or not store.has_method("is_sample_resident"):
		return false
	return bool(store.call("is_sample_resident", d, level))


func cells_per_face(level: int) -> int:
	var store: Node = _store()
	if store == null or not store.has_method("cells_per_face"):
		return 0
	return int(store.call("cells_per_face", level))


func spacing_for_level(level: int) -> float:
	var store: Node = _store()
	if store != null and store.has_method("spacing_for_level"):
		return float(store.call("spacing_for_level", level))
	return 1.0


func level_for_spacing(metres: float) -> int:
	var store: Node = _store()
	if store == null or not store.has_method("level_for_spacing"):
		return MAX_LEVEL
	return int(store.call("level_for_spacing", metres))


func stats() -> Dictionary:
	var store: Node = _store()
	if store != null and store.has_method("stats"):
		var value: Variant = store.call("stats")
		if value is Dictionary:
			var out: Dictionary = (value as Dictionary).duplicate(true)
			out["legacy_adapter"] = true
			return out
	return {
		"mode": "legacy_height_store_adapter",
		"legacy_adapter": true,
		"max_level": MAX_LEVEL,
	}
