extends Node
## Coarse CPU compatibility companion for the resident global-height architecture.
##
## Runtime sub-grid terrain synthesis is GPU-only. This singleton intentionally
## does NOT reproduce geomorph/detail noise on the CPU. Legacy callers may ask for
## a synchronous height, but they receive the resident macro/coast field plus
## sparse player deltas. Precise contact must use TerrainHeightQuery or another
## asynchronous GPU query.

signal tile_ready(level: int, face: int, tile_x: int, tile_y: int)

const TARGET_FINE_DEPTH: int = 16
const MAX_LEVEL: int = 14
const PRIORITY_COLLISION: float = -1000.0
const PRIORITY_VISIBLE: float = 0.0
const PRIORITY_PREFETCH: float = 50.0


func sample_height(d: Vector3, level: int, snap: Dictionary = {}) -> float:
	var h: float = sample_pristine(d, level)
	if snap.is_empty():
		h += Deltas.offset_at(d)
	else:
		h += Deltas.offset_at_snapshot(d, snap)
	return h


func sample_height_nonblocking(d: Vector3, level: int,
		snap: Dictionary = {}) -> float:
	return sample_height(d, level, snap)


func sample_pristine(d: Vector3, _level: int) -> float:
	if not Planet.ready_state or Planet.cfg == null:
		return 0.0
	var raw_macro_h: float = Planet.macro_height(d)
	return raw_macro_h + Planet.coast_profile_offset(d, raw_macro_h)


func sample_pristine_nonblocking(d: Vector3, level: int) -> float:
	return sample_pristine(d, level)


## The complete coarse terrain is resident from world adoption onward. Historical
## page-request APIs remain zero-cost compatibility shims only.
func prefetch_sample(_d: Vector3, _finest_level: int = 0,
		_priority: float = PRIORITY_PREFETCH) -> void:
	pass


func request_sample(_d: Vector3, _level: int,
		_priority: float = PRIORITY_VISIBLE) -> void:
	pass


func request_samples(_directions: Array[Vector3], _level: int,
		_priority: float = PRIORITY_VISIBLE) -> void:
	pass


func request_samples_prioritized(_directions: Array[Vector3], _level: int,
		_priorities: PackedFloat32Array) -> void:
	pass


func resident_tile(_level: int, _face: int, _tile_x: int,
		_tile_y: int) -> PackedFloat32Array:
	return PackedFloat32Array()


func is_sample_resident(_d: Vector3, _level: int) -> bool:
	return Planet.ready_state and Planet.cfg != null


func cells_per_face(level: int) -> int:
	if Planet.cfg == null:
		return 0
	var depth: int = TARGET_FINE_DEPTH - clampi(level, 0, MAX_LEVEL)
	return Planet.cfg.chunk_grid * (1 << depth)


func spacing_for_level(level: int) -> float:
	if Planet.cfg == null:
		return 1.0
	var base: float = PI * 0.5 * Planet.cfg.planet_radius \
		/ (float(Planet.cfg.chunk_grid) * pow(2.0, float(TARGET_FINE_DEPTH)))
	return base * pow(2.0, float(clampi(level, 0, MAX_LEVEL)))


func level_for_spacing(metres: float) -> int:
	if Planet.cfg == null:
		return MAX_LEVEL
	var base: float = spacing_for_level(0)
	if metres <= base:
		return 0
	var level: int = int(round(log(metres / base) / log(2.0)))
	return clampi(level, 0, MAX_LEVEL)


func stats() -> Dictionary:
	return {
		"mode": "global_resident_coarse_only",
		"memory_tiles": 0,
		"memory_hits": 0,
		"disk_hits": 0,
		"tiles_built": 0,
		"queued": 0,
		"in_flight": 0,
		"bake_in_flight": 0,
		"dropped": 0,
		"max_level": MAX_LEVEL,
		"samples": 0,
		"global_resident": Planet.ready_state,
		"terrain_io": false,
		"procedural_collision": false,
		"runtime_detail": "gpu_only",
	}
