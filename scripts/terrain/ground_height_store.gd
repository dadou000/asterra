extends Node
## Persistent sparse height-tile store for the ground geometry clipmap.
##
## Runtime terrain geometry should mostly be an I/O problem, not a procedural
## synthesis problem. This store canonicalises every requested height onto the
## cube-sphere lattice used by the depth-16 terrain, groups those samples into
## small 32x32-cell tiles, and keeps the pristine result on disk.
##
## Reads prefer a packaged res:// cache first, then the writable user:// cache.
## That means the exact same runtime code works today as a bake-once cache and
## later as a fully precompiled world package produced by an offline world
## compiler. Player edits are deliberately NOT cached: callers add the current
## Deltas layer after the immutable base height has been read.
##
## There are deliberately two sampling paths:
##
##   sample_height()             - blocking, for correctness-critical users such
##                                 as the current collision prototype.
##   sample_height_nonblocking() - visual streaming path. It NEVER builds or
##                                 reads a missing tile on the caller thread. It
##                                 requests the data and returns the nearest
##                                 resident coarser representation immediately.
##
## That distinction is the first step toward the production rule that a missing
## visual tile is allowed to be temporarily coarse but is never allowed to stall
## the game to manufacture detail.

signal tile_ready(level: int, face: int, tile_x: int, tile_y: int)

const CACHE_VERSION := 1
const MAGIC := 0x41535448 # "ASTH"
const TARGET_FINE_DEPTH := 16
const MAX_LEVEL := 6
const TILE_CELLS := 32
const TILE_VERTS := TILE_CELLS + 1
const MEMORY_TILE_LIMIT := 256
const CACHE_DIR := "terrain_height_cache"
const MAX_ASYNC_IN_FLIGHT := 1

var _namespace := "unconfigured"
var _memory: Dictionary = {}            # tile key -> PackedFloat32Array
var _last_use: Dictionary = {}          # tile key -> monotonically increasing tick
var _clock := 0
var _mutex := Mutex.new()
var _disk_hits := 0
var _memory_hits := 0
var _tiles_built := 0

# Visual cache misses use exactly one low-priority background worker. During
# development it is allowed to synthesize an absent tile once and persist it.
# Shipping/precompiled worlds should almost exclusively hit disk instead.
var _request_queue: Array[Dictionary] = []
var _queued: Dictionary = {}            # namespace-aware request key -> true
var _async_in_flight := 0
var _active_tasks: Dictionary = {}      # request key -> WorkerThreadPool task id
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
	# Coast-profile offsets are part of pristine_height(), so a changed profile is
	# a different immutable surface and must never reuse old cached tiles.
	if Planet.ready_state and Planet.cfg != null:
		_configure_namespace()


func _configure_namespace() -> void:
	var cfg := Planet.cfg
	var signature_parts := [
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
	var signature := String("|").join(signature_parts.map(func(v): return str(v)))
	var next_namespace := signature.sha256_text().substr(0, 24)

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
	_mutex.unlock()

	# user:// is the fallback bake location. A future world compiler can place the
	# exact same directory tree under res:// and it will automatically win reads.
	var root := ProjectSettings.globalize_path("user://%s/%s" % [CACHE_DIR, _namespace])
	DirAccess.make_dir_recursive_absolute(root)


## Return terrain including the current edit layer. `level=0` is the ~0.75 m
## lattice, level 1 ~1.5 m, ... level 6 ~48 m.
##
## This is the legacy/blocking path. A cache miss may touch disk or synthesize a
## tile before returning. Keep it for correctness-critical systems until they get
## an explicit tile-residency state machine of their own.
func sample_height(d: Vector3, level: int, snap: Dictionary = {}) -> float:
	var h := sample_pristine(d, level)
	if snap.is_empty():
		h += Deltas.offset_at(d)
	else:
		h += Deltas.offset_at_snapshot(d, snap)
	return h


## Visual/non-blocking equivalent of sample_height(). A fine miss requests all
## required tiles in coarse-to-fine priority order and returns the nearest already
## resident parent level. If no ground tile is resident yet, macro height is a
## cheap always-available fallback.
func sample_height_nonblocking(d: Vector3, level: int,
		snap: Dictionary = {}) -> float:
	var h := sample_pristine_nonblocking(d, level)
	if snap.is_empty():
		h += Deltas.offset_at(d)
	else:
		h += Deltas.offset_at_snapshot(d, snap)
	return h


## Bilinear lookup from the canonical cube-face lattice. This keeps samples stable
## across tangent-frame reanchors and makes the cache independent of the route the
## player took through the world.
func sample_pristine(d: Vector3, level: int) -> float:
	if not Planet.ready_state or Planet.cfg == null:
		return 0.0
	var used_level := clampi(level, 0, MAX_LEVEL)
	var sample := _sample_coordinates(d, used_level)
	var face := int(sample["face"])
	var cells := int(sample["cells"])
	var x0 := int(sample["x0"])
	var y0 := int(sample["y0"])
	var x1 := int(sample["x1"])
	var y1 := int(sample["y1"])
	var tx := float(sample["tx"])
	var ty := float(sample["ty"])

	# One lock covers all four vertices. Cache misses are intentionally serialized:
	# a single low-priority baker is preferable to several workers duplicating the
	# same expensive terrain synthesis and starving the game thread.
	_mutex.lock()
	var h00 := _sample_vertex_locked(used_level, face, x0, y0, cells)
	var h10 := _sample_vertex_locked(used_level, face, x1, y0, cells)
	var h01 := _sample_vertex_locked(used_level, face, x0, y1, cells)
	var h11 := _sample_vertex_locked(used_level, face, x1, y1, cells)
	_mutex.unlock()

	return lerpf(lerpf(h00, h10, tx), lerpf(h01, h11, tx), ty)


func sample_pristine_nonblocking(d: Vector3, level: int) -> float:
	if not Planet.ready_state or Planet.cfg == null:
		return 0.0
	var used_level := clampi(level, 0, MAX_LEVEL)
	var found := INF
	var found_level := MAX_LEVEL + 1

	# Prefer the requested representation, then walk toward the 48 m backing
	# level. This loop only touches RAM and a mutex; no file access and no noise.
	for candidate in range(used_level, MAX_LEVEL + 1):
		var h := _try_sample_pristine_memory(d, candidate)
		if is_finite(h):
			found = h
			found_level = candidate
			break

	# Queue missing detail from coarse to fine. Coarse data becomes a useful
	# fallback quickly, then the same area progressively sharpens as parent levels
	# arrive. Request de-duplication means thousands of visual samples collapse to
	# a small number of tile jobs.
	var request_from := MAX_LEVEL if found_level > MAX_LEVEL else found_level - 1
	for candidate in range(request_from, used_level - 1, -1):
		_request_sample(d, candidate)

	if is_finite(found):
		return found
	return Planet.macro_height(d)


## Explicit prefetch hook for the next phase's velocity-aware streamer. It is
## safe to call from worker threads; requests are de-duplicated under the cache
## mutex and executed later by the single low-priority cache worker.
func prefetch_sample(d: Vector3, finest_level: int = 0) -> void:
	var used_level := clampi(finest_level, 0, MAX_LEVEL)
	for candidate in range(MAX_LEVEL, used_level - 1, -1):
		_request_sample(d, candidate)


## Map an arbitrary physical sample spacing to the closest stored level.
func level_for_spacing(metres: float) -> int:
	if Planet.cfg == null:
		return MAX_LEVEL
	var base := PI * 0.5 * Planet.cfg.planet_radius \
		/ (float(Planet.cfg.chunk_grid) * pow(2.0, float(TARGET_FINE_DEPTH)))
	if metres <= base:
		return 0
	var level := int(round(log(metres / base) / log(2.0)))
	return clampi(level, 0, MAX_LEVEL)


func stats() -> Dictionary:
	# Diagnostic-only snapshot. These counters can tolerate a one-update race.
	return {
		"namespace": _namespace,
		"memory_tiles": _memory.size(),
		"memory_hits": _memory_hits,
		"disk_hits": _disk_hits,
		"tiles_built": _tiles_built,
		"queued": _request_queue.size(),
		"in_flight": _async_in_flight,
	}


func _sample_coordinates(d: Vector3, level: int) -> Dictionary:
	var fuv := CubeSphere.dir_to_face_uv(d.normalized())
	var face := int(fuv[0])
	var cells := _cells_per_face(level)
	var fx := clampf((float(fuv[1]) * 0.5 + 0.5) * float(cells), 0.0, float(cells))
	var fy := clampf((float(fuv[2]) * 0.5 + 0.5) * float(cells), 0.0, float(cells))
	var x0 := int(floor(fx))
	var y0 := int(floor(fy))
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


func _try_sample_pristine_memory(d: Vector3, level: int) -> float:
	var sample := _sample_coordinates(d, level)
	var face := int(sample["face"])
	var cells := int(sample["cells"])
	var x0 := int(sample["x0"])
	var y0 := int(sample["y0"])
	var x1 := int(sample["x1"])
	var y1 := int(sample["y1"])
	var tx := float(sample["tx"])
	var ty := float(sample["ty"])

	_mutex.lock()
	var h00 := _sample_vertex_memory_locked(level, face, x0, y0, cells)
	var h10 := _sample_vertex_memory_locked(level, face, x1, y0, cells)
	var h01 := _sample_vertex_memory_locked(level, face, x0, y1, cells)
	var h11 := _sample_vertex_memory_locked(level, face, x1, y1, cells)
	_mutex.unlock()

	if not is_finite(h00) or not is_finite(h10) or not is_finite(h01) or not is_finite(h11):
		return INF
	return lerpf(lerpf(h00, h10, tx), lerpf(h01, h11, tx), ty)


func _request_sample(d: Vector3, level: int) -> void:
	if _shutting_down or not Planet.ready_state or Planet.cfg == null:
		return
	var sample := _sample_coordinates(d, level)
	var face := int(sample["face"])
	var cells := int(sample["cells"])
	var vertices := [
		Vector2i(int(sample["x0"]), int(sample["y0"])),
		Vector2i(int(sample["x1"]), int(sample["y0"])),
		Vector2i(int(sample["x0"]), int(sample["y1"])),
		Vector2i(int(sample["x1"]), int(sample["y1"])),
	]

	_mutex.lock()
	for vertex in vertices:
		var addr := _tile_address_for_vertex(vertex.x, vertex.y, cells)
		_queue_tile_locked(level, face, addr.x, addr.y, cells)
	_mutex.unlock()


func _cells_per_face(level: int) -> int:
	var depth := TARGET_FINE_DEPTH - level
	return Planet.cfg.chunk_grid * (1 << depth)


func _tile_address_for_vertex(ix: int, iy: int, cells: int) -> Vector2i:
	var tile_count := int(ceil(float(cells) / float(TILE_CELLS)))
	return Vector2i(
		clampi(int(floor(float(maxi(ix, 1) - 1) / float(TILE_CELLS))), 0, tile_count - 1),
		clampi(int(floor(float(maxi(iy, 1) - 1) / float(TILE_CELLS))), 0, tile_count - 1))


func _memory_key(level: int, face: int, tile_x: int, tile_y: int) -> String:
	return "%d:%d:%d:%d" % [level, face, tile_x, tile_y]


func _request_key(namespace: String, level: int, face: int,
		tile_x: int, tile_y: int) -> String:
	return "%s:%d:%d:%d:%d" % [namespace, level, face, tile_x, tile_y]


func _queue_tile_locked(level: int, face: int, tile_x: int, tile_y: int,
		cells: int) -> void:
	var mem_key := _memory_key(level, face, tile_x, tile_y)
	if _memory.has(mem_key):
		return
	var req_key := _request_key(_namespace, level, face, tile_x, tile_y)
	if _queued.has(req_key):
		return
	_queued[req_key] = true
	_request_queue.append({
		"key": req_key,
		"namespace": _namespace,
		"level": level,
		"face": face,
		"tile_x": tile_x,
		"tile_y": tile_y,
		"cells": cells,
	})


func _pump_async_requests() -> void:
	if _shutting_down or _async_in_flight >= MAX_ASYNC_IN_FLIGHT:
		return

	_mutex.lock()
	if _request_queue.is_empty():
		_mutex.unlock()
		return
	# Parent/coarse tiles have greater level numbers. Load those first so a useful
	# fallback appears before spending time on sub-metre detail.
	_request_queue.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		return int(a["level"]) > int(b["level"]))
	var request: Dictionary = _request_queue.pop_front()
	_mutex.unlock()

	_start_async_request(request)


func _start_async_request(request: Dictionary) -> void:
	_async_in_flight += 1
	var request_copy := request.duplicate(true)
	var task := func() -> void:
		var result := _load_or_build_async(request_copy)
		_result_mutex.lock()
		_async_results.append(result)
		_result_mutex.unlock()
	var tid := WorkerThreadPool.add_task(task, false, "asterra_height_tile")
	_active_tasks[String(request["key"])] = tid


func _load_or_build_async(request: Dictionary) -> Dictionary:
	var namespace := String(request["namespace"])
	var level := int(request["level"])
	var face := int(request["face"])
	var tile_x := int(request["tile_x"])
	var tile_y := int(request["tile_y"])
	var cells := int(request["cells"])
	var data := _load_tile_for_namespace(namespace, level, face, tile_x, tile_y)
	var from_disk := data.size() == TILE_VERTS * TILE_VERTS
	var built := false

	if not from_disk:
		data = _build_tile(level, face, tile_x, tile_y, cells)
		built = data.size() == TILE_VERTS * TILE_VERTS
		# A rebake/profile change can invalidate the namespace while this low-priority
		# task is running. Never persist output into a namespace that is no longer
		# current.
		_mutex.lock()
		var still_current := _namespace == namespace
		_mutex.unlock()
		if built and still_current:
			_write_tile_for_namespace(namespace, level, face, tile_x, tile_y, data)

	return {
		"key": String(request["key"]),
		"namespace": namespace,
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

		var key := String(result["key"])
		var tid := int(_active_tasks.get(key, -1))
		if tid >= 0:
			WorkerThreadPool.wait_for_task_completion(tid)
			_active_tasks.erase(key)
		_async_in_flight = maxi(0, _async_in_flight - 1)

		_mutex.lock()
		_queued.erase(key)
		var current := String(result["namespace"]) == _namespace
		var data: PackedFloat32Array = result["data"]
		if current and data.size() == TILE_VERTS * TILE_VERTS:
			var mem_key := _memory_key(int(result["level"]), int(result["face"]),
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

		# Start another request immediately instead of waiting one rendered frame.
		_pump_async_requests()


func _sample_vertex_memory_locked(level: int, face: int, ix: int, iy: int,
		cells: int) -> float:
	var addr := _tile_address_for_vertex(ix, iy, cells)
	var local_x := ix - addr.x * TILE_CELLS
	var local_y := iy - addr.y * TILE_CELLS
	var key := _memory_key(level, face, addr.x, addr.y)
	if not _memory.has(key):
		return INF
	var tile: PackedFloat32Array = _memory[key]
	if local_x < 0 or local_x >= TILE_VERTS or local_y < 0 or local_y >= TILE_VERTS:
		return INF
	_memory_hits += 1
	_touch_locked(key)
	return tile[local_y * TILE_VERTS + local_x]


func _sample_vertex_locked(level: int, face: int, ix: int, iy: int, cells: int) -> float:
	var addr := _tile_address_for_vertex(ix, iy, cells)
	var local_x := ix - addr.x * TILE_CELLS
	var local_y := iy - addr.y * TILE_CELLS
	var tile := _get_tile_locked(level, face, addr.x, addr.y, cells)
	return tile[local_y * TILE_VERTS + local_x]


func _get_tile_locked(level: int, face: int, tile_x: int, tile_y: int,
		cells: int) -> PackedFloat32Array:
	var key := _memory_key(level, face, tile_x, tile_y)
	if _memory.has(key):
		_memory_hits += 1
		_touch_locked(key)
		return _memory[key]

	var loaded := _load_tile_for_namespace(_namespace, level, face, tile_x, tile_y)
	if loaded.size() == TILE_VERTS * TILE_VERTS:
		_disk_hits += 1
		_remember_locked(key, loaded)
		return loaded

	var built := _build_tile(level, face, tile_x, tile_y, cells)
	_tiles_built += 1
	_write_tile_for_namespace(_namespace, level, face, tile_x, tile_y, built)
	_remember_locked(key, built)
	return built


func _build_tile(level: int, face: int, tile_x: int, tile_y: int,
		cells: int) -> PackedFloat32Array:
	var heights := PackedFloat32Array()
	heights.resize(TILE_VERTS * TILE_VERTS)
	var start_x := tile_x * TILE_CELLS
	var start_y := tile_y * TILE_CELLS
	var radius := Planet.cfg.planet_radius
	var spacing := PI * 0.5 * radius / float(cells)
	var detail := Planet.make_detail()
	detail.set_sample_spacing(spacing)

	for y in TILE_VERTS:
		var gy := mini(start_y + y, cells)
		var v := -1.0 + 2.0 * float(gy) / float(cells)
		for x in TILE_VERTS:
			var gx := mini(start_x + x, cells)
			var u := -1.0 + 2.0 * float(gx) / float(cells)
			var dir := CubeSphere.face_uv_to_dir(face, u, v)
			heights[y * TILE_VERTS + x] = Planet.pristine_height(dir, detail)
	return heights


func _relative_path_for_namespace(namespace: String, level: int, face: int,
		tile_x: int, tile_y: int) -> String:
	return "%s/%s/l%d/f%d/%d_%d.ghz" % [
		CACHE_DIR, namespace, level, face, tile_x, tile_y]


func _load_tile_for_namespace(namespace: String, level: int, face: int,
		tile_x: int, tile_y: int) -> PackedFloat32Array:
	var relative := _relative_path_for_namespace(namespace, level, face, tile_x, tile_y)
	# Prefer a shipped/precompiled world cache; fall back to bake-once local data.
	for prefix in ["res://", "user://"]:
		var path := prefix + relative
		if not FileAccess.file_exists(path):
			continue
		var file := FileAccess.open_compressed(path, FileAccess.READ,
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
		var count := int(file.get_32())
		if count != TILE_VERTS * TILE_VERTS:
			continue
		var result := PackedFloat32Array()
		result.resize(count)
		for i in count:
			result[i] = file.get_float()
		return result
	return PackedFloat32Array()


func _write_tile_for_namespace(namespace: String, level: int, face: int,
		tile_x: int, tile_y: int, data: PackedFloat32Array) -> void:
	var relative := _relative_path_for_namespace(namespace, level, face, tile_x, tile_y)
	var path := "user://" + relative
	var parent := path.get_base_dir()
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(parent))
	var file := FileAccess.open_compressed(path, FileAccess.WRITE,
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
	for value in data:
		file.store_float(value)


func _remember_locked(key: String, tile: PackedFloat32Array) -> void:
	_memory[key] = tile
	_touch_locked(key)
	while _memory.size() > MEMORY_TILE_LIMIT:
		var oldest_key := ""
		var oldest_tick := 0x7fffffffffffffff
		for candidate in _last_use.keys():
			var tick := int(_last_use[candidate])
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
	for tid_value in _active_tasks.values():
		var tid := int(tid_value)
		if tid >= 0:
			WorkerThreadPool.wait_for_task_completion(tid)
	_active_tasks.clear()
	_result_mutex.lock()
	_async_results.clear()
	_result_mutex.unlock()
