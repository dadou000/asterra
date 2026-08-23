extends "res://scripts/terrain/planet_height_store.gd"
## Runtime-safe height store.
##
## The spherical visual renderer must never become an implicit CPU terrain
## generator. Its broad visible-cap requests use priority around -600 and are
## admitted only when the page is already in RAM or exists in the packaged/user
## cache. Collision (-1000) and the small local motion prefetch (positive priority)
## retain permission to bake missing pages.

const VISUAL_PRIORITY_MIN: float = -900.0
const VISUAL_PRIORITY_MAX: float = -100.0
const DISK_PRESENCE_LIMIT: int = 32768

var _disk_presence: Dictionary = {}
var _visual_uncached_skipped: int = 0
var _visual_cached_admitted: int = 0


func _configure_namespace() -> void:
	super._configure_namespace()
	_disk_presence.clear()
	_visual_uncached_skipped = 0
	_visual_cached_admitted = 0


func _queue_tile_locked(level: int, face: int, tile_x: int, tile_y: int,
		cells: int, priority: float) -> void:
	var mem_key: String = _memory_key(level, face, tile_x, tile_y)
	if _memory.has(mem_key):
		return

	if priority > VISUAL_PRIORITY_MIN and priority < VISUAL_PRIORITY_MAX:
		var presence_key: String = "%s:%d:%d:%d:%d" % [
			_namespace, level, face, tile_x, tile_y]
		var disk_backed: bool
		if _disk_presence.has(presence_key):
			disk_backed = bool(_disk_presence[presence_key])
		else:
			# Bound negative-result memoization during very long flights.
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

	super._queue_tile_locked(level, face, tile_x, tile_y, cells, priority)


func stats() -> Dictionary:
	var out: Dictionary = super.stats()
	out["visual_uncached_skipped"] = _visual_uncached_skipped
	out["visual_cached_admitted"] = _visual_cached_admitted
	out["visual_disk_only"] = true
	out["disk_presence_entries"] = _disk_presence.size()
	return out
