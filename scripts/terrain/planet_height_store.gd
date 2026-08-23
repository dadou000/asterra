extends Node
## Planet-wide persistent sparse height-tile store.
##
## This is the authoritative runtime height source for both the spherical GPU
## geometry clipmap and CPU collision. The hierarchy now runs from L0 (~0.75 m)
## through L14 (~12 km). Missing pages are queued asynchronously; visual terrain
## can fall back to coarser pages/orbit macro data while they arrive.

signal tile_ready(level: int, face: int, tile_x: int, tile_y: int)

const CACHE_VERSION := 1
const MAGIC := 0x41535448 # "ASTH"
const TARGET_FINE_DEPTH := 16
const MAX_LEVEL := 14
const TILE_CELLS := 32
const TILE_VERTS := TILE_CELLS + 1
const MEMORY_TILE_LIMIT := 8192
const CACHE_DIR := "terrain_height_cache"

# Warm/precompiled worlds are I/O-bound. Cold procedural synthesis remains
# deliberately single-lane so this cache can never become the old all-core visual
# terrain generator again.
const MAX_ASYNC_TOTAL_IN_FLIGHT := 8
const MAX_ASYNC_BAKE_IN_FLIGHT := 1
const MAX_REQUEST_START_SCAN := 128
const MAX_REQUEST_QUEUE := 4096
const REQUEST_QUEUE_TRIM_SLACK := 128

# Lower score means more urgent. Level bias is added separately so parent/coarse
# data at the same urgency arrives before its fine detail representation.
const PRIORITY_COLLISION := -1000.0
const PRIORITY_VISIBLE := 0.0
const PRIORITY_PREFETCH := 50.0

var _namespace := "unconfigured"
var _memory: Dictionary = {}
var _last_use: Dictionary = {}
var _clock := 0
var _mutex := Mutex.new()
var _disk_hits := 0
var _memory_hits := 0
var _tiles_built := 0
var _requests_dropped := 0

var _request_queue: Array[Dictionary] = []
var _queued: Dictionary = {}
var _async_in_flight := 0
var _async_bake_in_flight := 0
var _active_tasks: Dictionary = {}
var _async_results: Array[Dictionary] = []
var _result_mutex := Mutex.new()
var _shutting_down := false


func _ready() -> void:
	process_priority = 7
	Planet.world_ready.connect(_on_world_ready)
	Planet.coast_profile_changed.connect(_on_coast_profile_changed)
	if Planet.ready_state and Planet.cfg != null:
		_configure_namespace()


func _process(_dt: float) -> void:
	_drain_async_results()
	_pump_async_requests()


func _on_world_ready(_fields: PlanetFields) -> void:
	_configure_namespace()


func _on_coast_profile_changed() -> void:
	if Planet.ready_state and Planet.cfg != null:
		_configure_namespace()


func _configure_namespace() -> void:
	var cfg: GenConfig = Planet.cfg
	var signature_parts: Array = [
		CACHE_VERSION,
		cfg.cache_key(),
		cfg.world_seed,
		cfg.planet_radius,
		cfg.chunk_grid,
		cfg.detail_amplitude,
		cfg.detail_octaves,
		cfg.detail_base_frequency,
		str(Planet.coast_profile_points()),
	]
	var signature: String = String("|").join(signature_parts.map(func(v: Variant) -> String: return str(v)))
	var next_namespace: String = signature.sha256_text().substr(0, 24)

	_mutex.lock()
	_namespace = next_namespace
	_memory.clear()
	_last_use.clear()
	_request_queue.clear()
	_queued.clear()
	_clock = 0
	_disk_hits = 0
	_memory_hits = 0
	_tiles_built = 0
	_requests_dropped = 0
	_mutex.unlock()

	var root: String = ProjectSettings.globalize_path("user://%s/%s" % [CACHE_DIR, _namespace])
	DirAccess.make_dir_recursive_absolute(root)


func sample_height(d: Vector3, level: int, snap: Dictionary = {}) -> float:
	var h: float = sample_pristine(d, level)
	if snap.is_empty():
		h += Deltas.offset_at(d)
	else:
		h += Deltas.offset_at_snapshot(d, snap)
	return h


func sample_height_nonblocking(d: Vector3, level: int,
		snap: Dictionary = {}) -> float:
	var h: float = sample_pristine_nonblocking(d, level)
	if snap.is_empty():
		h += Deltas.offset_at(d)
	else:
		h += Deltas.offset_at_snapshot(d, snap)
	return h


func sample_pristine(d: Vector3, level: int) -> float:
	if not Planet.ready_state or Planet.cfg == null:
		return 0.0
	var used_level: int = clampi(level, 0, MAX_LEVEL)
	var sample: Dictionary = _sample_coordinates(d, used_level)
	var face: int = int(sample["face"])
	var cells: int = int(sample["cells"])
	var x0: int = int(sample["x0"])
	var y0: int = int(sample["y0"])
	var x1: int = int(sample["x1"])
	var y1: int = int(sample["y1"])
	var tx: float = float(sample["tx"])
	var ty: float = float(sample["ty"])

	_mutex.lock()
	var h00: float = _sample_vertex_locked(used_level, face, x0, y0, cells)
	var h10: float = _sample_vertex_locked(used_level, face, x1, y0, cells)
	var h01: float = _sample_vertex_locked(used_level, face, x0, y1, cells)
	var h11: float = _sample_vertex_locked(used_level, face, x1, y1, cells)
	_mutex.unlock()

	return lerpf(lerpf(h00, h10, tx), lerpf(h01, h11, tx), ty)


## RAM-only fallback lookup and request creation under one cache lock.
func sample_pristine_nonblocking(d: Vector3, level: int) -> float:
	if not Planet.ready_state or Planet.cfg == null:
		return 0.0
	var used_level: int = clampi(level, 0, MAX_LEVEL)
	var found: float = INF
	var found_level: int = MAX_LEVEL + 1

	_mutex.lock()
	for candidate: int in range(used_level, MAX_LEVEL + 1):
		var h: float = _sample_pristine_memory_locked(d, candidate)
		if is_finite(h):
			found = h
			found_level = candidate
			break

	var request_from: int = MAX_LEVEL if found_level > MAX_LEVEL else found_level - 1
	for candidate: int in range(request_from, used_level - 1, -1):
		_request_sample_locked(d, candidate, PRIORITY_VISIBLE)
	if _request_queue.size() > MAX_REQUEST_QUEUE + REQUEST_QUEUE_TRIM_SLACK:
		_trim_request_queue_locked()
	_mutex.unlock()

	if is_finite(found):
		return found
	return Planet.macro_height(d)


func prefetch_sample(d: Vector3, finest_level: int = 0,
		priority: float = PRIORITY_PREFETCH) -> void:
	if _shutting_down or not Planet.ready_state or Planet.cfg == null:
		return
	var used_level: int = clampi(finest_level, 0, MAX_LEVEL)
	_mutex.lock()
	for candidate: int in range(MAX_LEVEL, used_level - 1, -1):
		_request_sample_locked(d, candidate, priority)
	if _request_queue.size() > MAX_REQUEST_QUEUE + REQUEST_QUEUE_TRIM_SLACK:
		_trim_request_queue_locked()
	_mutex.unlock()


func request_sample(d: Vector3, level: int,
		priority: float = PRIORITY_VISIBLE) -> void:
	if _shutting_down or not Planet.ready_state or Planet.cfg == null:
		return
	_mutex.lock()
	_request_sample_locked(d, clampi(level, 0, MAX_LEVEL), priority)
	if _request_queue.size() > MAX_REQUEST_QUEUE + REQUEST_QUEUE_TRIM_SLACK:
		_trim_request_queue_locked()
	_mutex.unlock()


func request_samples(directions: Array[Vector3], level: int,
		priority: float = PRIORITY_VISIBLE) -> void:
	if _shutting_down or not Planet.ready_state or Planet.cfg == null \
			or directions.is_empty():
		return
	var used_level: int = clampi(level, 0, MAX_LEVEL)
	_mutex.lock()
	for d: Vector3 in directions:
		_request_sample_locked(d, used_level, priority)
	if _request_queue.size() > MAX_REQUEST_QUEUE + REQUEST_QUEUE_TRIM_SLACK:
		_trim_request_queue_locked()
	_mutex.unlock()


func resident_tile(level: int, face: int, tile_x: int, tile_y: int) -> PackedFloat32Array:
	if not Planet.ready_state or Planet.cfg == null:
		return PackedFloat32Array()
	var key: String = _memory_key(clampi(level, 0, MAX_LEVEL), face, tile_x, tile_y)
	_mutex.lock()
	var result := PackedFloat32Array()
	if _memory.has(key):
		result = _memory[key]
		_memory_hits += 1
		_touch_locked(key)
	_mutex.unlock()
	return result


func cells_per_face(level: int) -> int:
	if Planet.cfg == null:
		return 0
	return _cells_per_face(clampi(level, 0, MAX_LEVEL))


func is_sample_resident(d: Vector3, level: int) -> bool:
	if not Planet.ready_state or Planet.cfg == null:
		return false
	_mutex.lock()
	var h: float = _sample_pristine_memory_locked(d, clampi(level, 0, MAX_LEVEL))
	_mutex.unlock()
	return is_finite(h)


func level_for_spacing(metres: float) -> int:
	if Planet.cfg == null:
		return MAX_LEVEL
	var base: float = PI * 0.5 * Planet.cfg.planet_radius \
		/ (float(Planet.cfg.chunk_grid) * pow(2.0, float(TARGET_FINE_DEPTH)))
	if metres <= base:
		return 0
	var level: int = int(round(log(metres / base) / log(2.0)))
	return clampi(level, 0, MAX_LEVEL)


func stats() -> Dictionary:
	return {
		"namespace": _namespace,
		"memory_tiles": _memory.size(),
		"memory_hits": _memory_hits,
		"disk_hits": _disk_hits,
		"tiles_built": _tiles_built,
		"queued": _request_queue.size(),
		"in_flight": _async_in_flight,
		"bake_in_flight": _async_bake_in_flight,
		"dropped": _requests_dropped,
		"max_level": MAX_LEVEL,
	}


func _sample_coordinates(d: Vector3, level: int) -> Dictionary:
	var fuv: Array = CubeSphere.dir_to_face_uv(d.normalized())
	var face: int = int(fuv[0])
	var cells: int = _cells_per_face(level)
	var fx: float = clampf((float(fuv[1]) * 0.5 + 0.5) * float(cells), 0.0, float(cells))
	var fy: float = clampf((float(fuv[2]) * 0.5 + 0.5) * float(cells), 0.0, float(cells))
	var x0: int = int(floor(fx))
	var y0: int = int(floor(fy))
	return {
		"face": face,
		"cells": cells,
		"x0": x0,
		"y0": y0,
		"x1": mini(x0 + 1, cells),
		"y1": mini(y0 + 1, cells),
		"tx": fx - float(x0),
		"ty": fy - float(y0),
	}


func _sample_pristine_memory_locked(d: Vector3, level: int) -> float:
	var sample: Dictionary = _sample_coordinates(d, level)
	var face: int = int(sample["face"])
	var cells: int = int(sample["cells"])
	var x0: int = int(sample["x0"])
	var y0: int = int(sample["y0"])
	var x1: int = int(sample["x1"])
	var y1: int = int(sample["y1"])
	var tx: float = float(sample["tx"])
	var ty: float = float(sample["ty"])

	var h00: float = _sample_vertex_memory_locked(level, face, x0, y0, cells)
	var h10: float = _sample_vertex_memory_locked(level, face, x1, y0, cells)
	var h01: float = _sample_vertex_memory_locked(level, face, x0, y1, cells)
	var h11: float = _sample_vertex_memory_locked(level, face, x1, y1, cells)
	if not is_finite(h00) or not is_finite(h10) or not is_finite(h01) or not is_finite(h11):
		return INF
	return lerpf(lerpf(h00, h10, tx), lerpf(h01, h11, tx), ty)


func _try_sample_pristine_memory(d: Vector3, level: int) -> float:
	_mutex.lock()
	var h: float = _sample_pristine_memory_locked(d, level)
	_mutex.unlock()
	return h


func _request_sample(d: Vector3, level: int, priority: float) -> void:
	if _shutting_down or not Planet.ready_state or Planet.cfg == null:
		return
	_mutex.lock()
	_request_sample_locked(d, level, priority)
	if _request_queue.size() > MAX_REQUEST_QUEUE + REQUEST_QUEUE_TRIM_SLACK:
		_trim_request_queue_locked()
	_mutex.unlock()


func _request_sample_locked(d: Vector3, level: int, priority: float) -> void:
	var sample: Dictionary = _sample_coordinates(d, level)
	var face: int = int(sample["face"])
	var cells: int = int(sample["cells"])
	var vertices: Array[Vector2i] = [
		Vector2i(int(sample["x0"]), int(sample["y0"])),
		Vector2i(int(sample["x1"]), int(sample["y0"])),
		Vector2i(int(sample["x0"]), int(sample["y1"])),
		Vector2i(int(sample["x1"]), int(sample["y1"])),
	]
	for vertex: Vector2i in vertices:
		var addr: Vector2i = _tile_address_for_vertex(vertex.x, vertex.y, cells)
		_queue_tile_locked(level, face, addr.x, addr.y, cells, priority)


func _cells_per_face(level: int) -> int:
	var depth: int = TARGET_FINE_DEPTH - level
	return Planet.cfg.chunk_grid * (1 << depth)


func _tile_address_for_vertex(ix: int, iy: int, cells: int) -> Vector2i:
	var tile_count: int = int(ceil(float(cells) / float(TILE_CELLS)))
	return Vector2i(
		clampi(int(floor(float(maxi(ix, 1) - 1) / float(TILE_CELLS))), 0, tile_count - 1),
		clampi(int(floor(float(maxi(iy, 1) - 1) / float(TILE_CELLS))), 0, tile_count - 1))


func _memory_key(level: int, face: int, tile_x: int, tile_y: int) -> String:
	return "%d:%d:%d:%d" % [level, face, tile_x, tile_y]


func _request_key(cache_namespace: String, level: int, face: int,
		tile_x: int, tile_y: int) -> String:
	return "%s:%d:%d:%d:%d" % [cache_namespace, level, face, tile_x, tile_y]


func _request_score(priority: float, level: int) -> float:
	return priority + float(MAX_LEVEL - level) * 0.25


func _queue_tile_locked(level: int, face: int, tile_x: int, tile_y: int,
		cells: int, priority: float) -> void:
	var mem_key: String = _memory_key(level, face, tile_x, tile_y)
	if _memory.has(mem_key):
		return
	var req_key: String = _request_key(_namespace, level, face, tile_x, tile_y)
	var score: float = _request_score(priority, level)
	if _queued.has(req_key):
		_queued[req_key] = minf(float(_queued[req_key]), score)
		return
	_queued[req_key] = score
	_request_queue.append({
		"key": req_key,
		"namespace": _namespace,
		"level": level,
		"face": face,
		"tile_x": tile_x,
		"tile_y": tile_y,
		"cells": cells,
	})


func _sort_request_queue_locked() -> void:
	_request_queue.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		var pa: float = float(_queued.get(String(a["key"]), INF))
		var pb: float = float(_queued.get(String(b["key"]), INF))
		if absf(pa - pb) > 1e-6:
			return pa < pb
		return int(a["level"]) > int(b["level"]))


func _trim_request_queue_locked() -> void:
	_sort_request_queue_locked()
	while _request_queue.size() > MAX_REQUEST_QUEUE:
		var dropped: Dictionary = _request_queue.pop_back()
		_queued.erase(String(dropped["key"]))
		_requests_dropped += 1


func _pump_async_requests() -> void:
	if _shutting_down:
		return
	while _async_in_flight < MAX_ASYNC_TOTAL_IN_FLIGHT:
		var request: Dictionary = _take_startable_request()
		if request.is_empty():
			return
		var disk_backed: bool = bool(request.get("disk_backed", false))
		_start_async_request(request, disk_backed)


func _take_startable_request() -> Dictionary:
	var deferred: Array[Dictionary] = []
	var scanned := 0
	while scanned < MAX_REQUEST_START_SCAN:
		_mutex.lock()
		if _request_queue.is_empty():
			_mutex.unlock()
			break
		_sort_request_queue_locked()
		var request: Dictionary = _request_queue.pop_front()
		_mutex.unlock()
		scanned += 1

		var disk_backed: bool = _request_disk_backed(request)
		if disk_backed or _async_bake_in_flight < MAX_ASYNC_BAKE_IN_FLIGHT:
			request["disk_backed"] = disk_backed
			_requeue_deferred(deferred)
			return request
		deferred.append(request)

	_requeue_deferred(deferred)
	return {}


func _requeue_deferred(deferred: Array[Dictionary]) -> void:
	if deferred.is_empty():
		return
	_mutex.lock()
	for request: Dictionary in deferred:
		_request_queue.append(request)
	_mutex.unlock()


func _request_disk_backed(request: Dictionary) -> bool:
	if request.has("disk_hint"):
		return bool(request["disk_hint"])
	var cache_namespace: String = String(request["namespace"])
	var level: int = int(request["level"])
	var face: int = int(request["face"])
	var tile_x: int = int(request["tile_x"])
	var tile_y: int = int(request["tile_y"])
	var relative: String = _relative_path_for_namespace(cache_namespace, level, face, tile_x, tile_y)
	var disk_backed := false
	if FileAccess.file_exists("res://" + relative):
		disk_backed = true
	elif FileAccess.file_exists("user://" + relative):
		disk_backed = true
	request["disk_hint"] = disk_backed
	return disk_backed


func _start_async_request(request: Dictionary, disk_backed: bool) -> void:
	_async_in_flight += 1
	if not disk_backed:
		_async_bake_in_flight += 1
	var request_copy: Dictionary = request.duplicate(true)
	var counted_as_bake: bool = not disk_backed
	var task := func() -> void:
		var result: Dictionary = _load_or_build_async(request_copy)
		result["counted_as_bake"] = counted_as_bake
		_result_mutex.lock()
		_async_results.append(result)
		_result_mutex.unlock()
	var tid: int = WorkerThreadPool.add_task(task, false, "asterra_height_tile")
	_active_tasks[String(request["key"])] = tid


func _load_or_build_async(request: Dictionary) -> Dictionary:
	var cache_namespace: String = String(request["namespace"])
	var level: int = int(request["level"])
	var face: int = int(request["face"])
	var tile_x: int = int(request["tile_x"])
	var tile_y: int = int(request["tile_y"])
	var cells: int = int(request["cells"])
	var data: PackedFloat32Array = _load_tile_for_namespace(cache_namespace, level, face, tile_x, tile_y)
	var from_disk: bool = data.size() == TILE_VERTS * TILE_VERTS
	var built := false

	if not from_disk:
		data = _build_tile(level, face, tile_x, tile_y, cells)
		built = data.size() == TILE_VERTS * TILE_VERTS
		_mutex.lock()
		var still_current: bool = _namespace == cache_namespace
		_mutex.unlock()
		if built and still_current:
			_write_tile_for_namespace(cache_namespace, level, face, tile_x, tile_y, data)

	return {
		"key": String(request["key"]),
		"namespace": cache_namespace,
		"level": level,
		"face": face,
		"tile_x": tile_x,
		"tile_y": tile_y,
		"data": data,
		"disk": from_disk,
		"built": built,
	}


func _drain_async_results() -> void:
	while true:
		_result_mutex.lock()
		if _async_results.is_empty():
			_result_mutex.unlock()
			return
		var result: Dictionary = _async_results.pop_front()
		_result_mutex.unlock()

		var key: String = String(result["key"])
		var tid: int = int(_active_tasks.get(key, -1))
		if tid >= 0:
			WorkerThreadPool.wait_for_task_completion(tid)
			_active_tasks.erase(key)
		_async_in_flight = maxi(0, _async_in_flight - 1)
		if bool(result.get("counted_as_bake", false)):
			_async_bake_in_flight = maxi(0, _async_bake_in_flight - 1)

		_mutex.lock()
		_queued.erase(key)
		var current: bool = String(result["namespace"]) == _namespace
		var data: PackedFloat32Array = result["data"]
		if current and data.size() == TILE_VERTS * TILE_VERTS:
			var mem_key: String = _memory_key(int(result["level"]), int(result["face"]),
				int(result["tile_x"]), int(result["tile_y"]))
			_remember_locked(mem_key, data)
			if bool(result["disk"]):
				_disk_hits += 1
			elif bool(result["built"]):
				_tiles_built += 1
		_mutex.unlock()

		if current and data.size() == TILE_VERTS * TILE_VERTS:
			tile_ready.emit(int(result["level"]), int(result["face"]),
				int(result["tile_x"]), int(result["tile_y"]))

	_pump_async_requests()


func _sample_vertex_memory_locked(level: int, face: int, ix: int, iy: int,
		cells: int) -> float:
	var addr: Vector2i = _tile_address_for_vertex(ix, iy, cells)
	var local_x: int = ix - addr.x * TILE_CELLS
	var local_y: int = iy - addr.y * TILE_CELLS
	var key: String = _memory_key(level, face, addr.x, addr.y)
	if not _memory.has(key):
		return INF
	var tile: PackedFloat32Array = _memory[key]
	if local_x < 0 or local_x >= TILE_VERTS or local_y < 0 or local_y >= TILE_VERTS:
		return INF
	_memory_hits += 1
	_touch_locked(key)
	return tile[local_y * TILE_VERTS + local_x]


func _sample_vertex_locked(level: int, face: int, ix: int, iy: int, cells: int) -> float:
	var addr: Vector2i = _tile_address_for_vertex(ix, iy, cells)
	var local_x: int = ix - addr.x * TILE_CELLS
	var local_y: int = iy - addr.y * TILE_CELLS
	var tile: PackedFloat32Array = _get_tile_locked(level, face, addr.x, addr.y, cells)
	return tile[local_y * TILE_VERTS + local_x]


func _get_tile_locked(level: int, face: int, tile_x: int, tile_y: int,
		cells: int) -> PackedFloat32Array:
	var key: String = _memory_key(level, face, tile_x, tile_y)
	if _memory.has(key):
		_memory_hits += 1
		_touch_locked(key)
		return _memory[key]

	var loaded: PackedFloat32Array = _load_tile_for_namespace(_namespace, level, face, tile_x, tile_y)
	if loaded.size() == TILE_VERTS * TILE_VERTS:
		_disk_hits += 1
		_remember_locked(key, loaded)
		return loaded

	var built: PackedFloat32Array = _build_tile(level, face, tile_x, tile_y, cells)
	_tiles_built += 1
	_write_tile_for_namespace(_namespace, level, face, tile_x, tile_y, built)
	_remember_locked(key, built)
	return built


func _build_tile(level: int, face: int, tile_x: int, tile_y: int,
		cells: int) -> PackedFloat32Array:
	var heights := PackedFloat32Array()
	heights.resize(TILE_VERTS * TILE_VERTS)
	var start_x: int = tile_x * TILE_CELLS
	var start_y: int = tile_y * TILE_CELLS
	var radius: float = Planet.cfg.planet_radius
	var spacing: float = PI * 0.5 * radius / float(cells)
	var detail: TerrainDetail = Planet.make_detail()
	detail.set_sample_spacing(spacing)

	for y in TILE_VERTS:
		var gy: int = mini(start_y + y, cells)
		var v: float = -1.0 + 2.0 * float(gy) / float(cells)
		for x in TILE_VERTS:
			var gx: int = mini(start_x + x, cells)
			var u: float = -1.0 + 2.0 * float(gx) / float(cells)
			var dir: Vector3 = CubeSphere.face_uv_to_dir(face, u, v)
			heights[y * TILE_VERTS + x] = Planet.pristine_height(dir, detail)
	return heights


func _relative_path_for_namespace(cache_namespace: String, level: int, face: int,
		tile_x: int, tile_y: int) -> String:
	return "%s/%s/l%d/f%d/%d_%d.ghz" % [
		CACHE_DIR, cache_namespace, level, face, tile_x, tile_y]


func _load_tile_for_namespace(cache_namespace: String, level: int, face: int,
		tile_x: int, tile_y: int) -> PackedFloat32Array:
	var relative: String = _relative_path_for_namespace(cache_namespace, level, face, tile_x, tile_y)
	var prefixes := PackedStringArray(["res://", "user://"])
	for prefix: String in prefixes:
		var path: String = prefix + relative
		if not FileAccess.file_exists(path):
			continue
		var file: FileAccess = FileAccess.open_compressed(path, FileAccess.READ,
			FileAccess.COMPRESSION_ZSTD)
		if file == null:
			continue
		if int(file.get_32()) != MAGIC:
			continue
		if int(file.get_32()) != CACHE_VERSION:
			continue
		if int(file.get_32()) != level or int(file.get_32()) != face:
			continue
		if int(file.get_32()) != tile_x or int(file.get_32()) != tile_y:
			continue
		if int(file.get_32()) != TILE_VERTS:
			continue
		var count: int = int(file.get_32())
		if count != TILE_VERTS * TILE_VERTS:
			continue
		var result := PackedFloat32Array()
		result.resize(count)
		for i in count:
			result[i] = file.get_float()
		return result
	return PackedFloat32Array()


func _write_tile_for_namespace(cache_namespace: String, level: int, face: int,
		tile_x: int, tile_y: int, data: PackedFloat32Array) -> void:
	var relative: String = _relative_path_for_namespace(cache_namespace, level, face, tile_x, tile_y)
	var path: String = "user://" + relative
	var parent: String = path.get_base_dir()
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(parent))
	var file: FileAccess = FileAccess.open_compressed(path, FileAccess.WRITE,
		FileAccess.COMPRESSION_ZSTD)
	if file == null:
		return
	file.store_32(MAGIC)
	file.store_32(CACHE_VERSION)
	file.store_32(level)
	file.store_32(face)
	file.store_32(tile_x)
	file.store_32(tile_y)
	file.store_32(TILE_VERTS)
	file.store_32(data.size())
	for value: float in data:
		file.store_float(value)


func _remember_locked(key: String, tile: PackedFloat32Array) -> void:
	_memory[key] = tile
	_touch_locked(key)
	while _memory.size() > MEMORY_TILE_LIMIT:
		var oldest_key := ""
		var oldest_tick := 0x7fffffffffffffff
		for candidate: Variant in _last_use.keys():
			var tick: int = int(_last_use[candidate])
			if tick < oldest_tick:
				oldest_tick = tick
				oldest_key = String(candidate)
		if oldest_key.is_empty():
			break
		_memory.erase(oldest_key)
		_last_use.erase(oldest_key)


func _touch_locked(key: String) -> void:
	_clock += 1
	_last_use[key] = _clock


func _exit_tree() -> void:
	_shutting_down = true
	for tid_value: Variant in _active_tasks.values():
		var tid: int = int(tid_value)
		if tid >= 0:
			WorkerThreadPool.wait_for_task_completion(tid)
	_active_tasks.clear()
	_result_mutex.lock()
	_async_results.clear()
	_result_mutex.unlock()
