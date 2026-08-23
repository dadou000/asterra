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


func stats() -> Dictionary:
	var out: Dictionary = super.stats()
	out["visual_uncached_skipped"] = _visual_uncached_skipped
	out["visual_cached_admitted"] = _visual_cached_admitted
	out["visual_disk_only"] = true
	out["disk_presence_entries"] = _disk_presence.size()
	out["prefetch_backpressure_dropped"] = _prefetch_backpressure_dropped
	out["prefetch_queue_soft_limit"] = PREFETCH_QUEUE_SOFT_LIMIT
	return out
