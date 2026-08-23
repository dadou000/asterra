extends "res://scripts/terrain/planet_height_store.gd"
## Runtime-safe height store.
##
## The spherical visual renderer must never become an implicit CPU terrain
## generator. Its broad visible-cap requests use priority around -600 and are
## admitted only when the page is already in RAM or exists in the packaged/user
## cache. Collision (-1000) keeps permission to bake. Predictive local prefetch
## may bake too, but is hard backpressured so it cannot build a huge sortable queue.

const VISUAL_PRIORITY_MIN: float = -900.0
const VISUAL_PRIORITY_MAX: float = -100.0
const DISK_PRESENCE_LIMIT: int = 32768
const PREFETCH_PRIORITY_MIN: float = 10.0
const PREFETCH_QUEUE_SOFT_LIMIT: int = 96

# Cached visual pages are tiny compressed files. More I/O lanes dramatically
# reduce the time the wireframe spends showing parent/orbit geometry while still
# keeping procedural synthesis on the single bake lane enforced by the base store.
const PLAYABLE_MAX_ASYNC_TOTAL_IN_FLIGHT: int = 20

var _disk_presence: Dictionary = {}
var _visual_uncached_skipped: int = 0
var _visual_cached_admitted: int = 0
var _prefetch_backpressure_dropped: int = 0


func _configure_namespace() -> void:
	super._configure_namespace()
	_disk_presence.clear()
	_visual_uncached_skipped = 0
	_visual_cached_admitted = 0
	_prefetch_backpressure_dropped = 0


func _queue_tile_locked(level: int, face: int, tile_x: int, tile_y: int,
		cells: int, priority: float) -> void:
	var mem_key: String = _memory_key(level, face, tile_x, tile_y)
	if _memory.has(mem_key):
		return

	# Broad renderer requests are cache discovery only. Never synthesize missing
	# visual pages while the game is running.
	if priority > VISUAL_PRIORITY_MIN and priority < VISUAL_PRIORITY_MAX:
		var presence_key: String = "%s:%d:%d:%d:%d" % [
			_namespace, level, face, tile_x, tile_y]
		var disk_backed: bool
		if _disk_presence.has(presence_key):
			disk_backed = bool(_disk_presence[presence_key])
		else:
			if _disk_presence.size() >= DISK_PRESENCE_LIMIT:
				_disk_presence.clear()
			var relative: String = _relative_path_for_namespace(
				_namespace, level, face, tile_x, tile_y)
			disk_backed = FileAccess.file_exists("res://" + relative) \
				or FileAccess.file_exists("user://" + relative)
			_disk_presence[presence_key] = disk_backed
		if not disk_backed:
			_visual_uncached_skipped += 1
			return
		_visual_cached_admitted += 1

	# Predictive prefetch is expendable. Once enough work is queued to keep the
	# single bake lane busy, discard new speculative pages rather than letting the
	# O(n log n) priority queue sort grow into a long-session stall.
	if priority >= PREFETCH_PRIORITY_MIN \
			and _request_queue.size() >= PREFETCH_QUEUE_SOFT_LIMIT:
		_prefetch_backpressure_dropped += 1
		return

	super._queue_tile_locked(level, face, tile_x, tile_y, cells, priority)


## One-lock prioritized batch path used by the visual clipmap. The renderer sends
## centre-out priorities so nearby pages retire before distant annulus pages rather
## than appearing in arbitrary rectangular patches.
func request_samples_prioritized(directions: Array[Vector3], level: int,
		priorities: PackedFloat32Array) -> void:
	if _shutting_down or not Planet.ready_state or Planet.cfg == null \
			or directions.is_empty():
		return
	var used_level: int = clampi(level, 0, MAX_LEVEL)
	_mutex.lock()
	for i: int in directions.size():
		var priority: float = PRIORITY_VISIBLE
		if i < priorities.size():
			priority = priorities[i]
		_request_sample_locked(directions[i], used_level, priority)
	if _request_queue.size() > MAX_REQUEST_QUEUE + REQUEST_QUEUE_TRIM_SLACK:
		_trim_request_queue_locked()
	_mutex.unlock()


## More parallelism is used only for already cached disk pages. `_take_startable_request`
## still limits procedural baking to the base store's single bake lane.
func _pump_async_requests() -> void:
	if _shutting_down:
		return
	while _async_in_flight < PLAYABLE_MAX_ASYNC_TOTAL_IN_FLIGHT:
		var request: Dictionary = _take_startable_request()
		if request.is_empty():
			return
		var disk_backed: bool = bool(request.get("disk_backed", false))
		_start_async_request(request, disk_backed)


## Fast path for the existing .ghz format. The base implementation calls
## get_float() 1089 times per 33x33 page. Reading the payload once and converting
## the packed bytes removes most per-page script-call overhead during refinement.
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
		var bytes: PackedByteArray = file.get_buffer(count * 4)
		if bytes.size() != count * 4:
			continue
		var result: PackedFloat32Array = bytes.to_float32_array()
		if result.size() == count:
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
	file.store_buffer(data.to_byte_array())


func stats() -> Dictionary:
	var out: Dictionary = super.stats()
	out["visual_uncached_skipped"] = _visual_uncached_skipped
	out["visual_cached_admitted"] = _visual_cached_admitted
	out["visual_disk_only"] = true
	out["disk_presence_entries"] = _disk_presence.size()
	out["prefetch_backpressure_dropped"] = _prefetch_backpressure_dropped
	out["prefetch_queue_soft_limit"] = PREFETCH_QUEUE_SOFT_LIMIT
	out["playable_io_limit"] = PLAYABLE_MAX_ASYNC_TOTAL_IN_FLIGHT
	out["bulk_page_io"] = true
	return out
