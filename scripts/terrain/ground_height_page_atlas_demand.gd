extends "res://scripts/terrain/ground_height_page_atlas.gd"
## Demand-driven GPU height atlas.
##
## The base atlas used to upload every GroundHeightStore completion, including
## stale/speculative pages that were no longer visible. That polluted the small
## GPU cache and could evict a visible page. Worse, once an evicted page still
## existed in GroundHeightStore RAM, requesting it again produced no tile_ready
## signal, so the GPU copy could remain missing forever.
##
## This layer fixes both failure modes:
## - only pages touched by the active renderer are admitted from tile_ready;
## - a missing GPU page is immediately rehydrated from the RAM tile cache;
## - recently touched pages are protected from LRU eviction when possible.

const WANTED_TTL_MS: int = 1800
const PRUNE_INTERVAL_MS: int = 1000

var _wanted_until: Dictionary = {}
var _next_prune_msec: int = 0
var _ignored_ready: int = 0
var _ram_rehydrates: int = 0
var _forced_protected_evictions: int = 0


func _process(_dt: float) -> void:
	var now: int = Time.get_ticks_msec()
	if now < _next_prune_msec:
		return
	_next_prune_msec = now + PRUNE_INTERVAL_MS
	var expired: Array[String] = []
	for key_value: Variant in _wanted_until.keys():
		var key: String = String(key_value)
		if int(_wanted_until.get(key, 0)) < now:
			expired.append(key)
	for key: String in expired:
		_wanted_until.erase(key)


func _on_world_ready(fields: PlanetFields) -> void:
	_wanted_until.clear()
	_next_prune_msec = 0
	super._on_world_ready(fields)


func _on_coast_profile_changed() -> void:
	_wanted_until.clear()
	_next_prune_msec = 0
	super._on_coast_profile_changed()


func touch_sample(d: Vector3, level: int) -> bool:
	var addresses: Array[String] = _keys_for_sample(d, level)
	if addresses.is_empty():
		return false
	var all_present: bool = true
	for key: String in addresses:
		_mark_wanted(key)
		if not _key_to_slot.has(key):
			if not _rehydrate_from_ram(key):
				all_present = false
				continue
		if _key_to_slot.has(key):
			_touch_slot(int(_key_to_slot[key]))
		else:
			all_present = false
	return all_present


func has_sample(d: Vector3, level: int) -> bool:
	var addresses: Array[String] = _keys_for_sample(d, level)
	if addresses.is_empty():
		return false
	for key: String in addresses:
		if not _key_to_slot.has(key):
			return false
	return true


func _on_tile_ready(level: int, face: int, tile_x: int, tile_y: int) -> void:
	var key: String = _page_key(level, face, tile_x, tile_y)
	# Collision/prefetch/obsolete visual work should warm RAM/disk, not evict the
	# pages the current camera is actually drawing.
	if not _is_wanted(key):
		_ignored_ready += 1
		return
	var data: PackedFloat32Array = GroundHeightStore.resident_tile(level, face, tile_x, tile_y)
	if data.size() != TILE_VERTS * TILE_VERTS:
		return
	_upload_page(level, face, tile_x, tile_y, data, false)


func _mark_wanted(key: String) -> void:
	_wanted_until[key] = Time.get_ticks_msec() + WANTED_TTL_MS


func _is_wanted(key: String) -> bool:
	var until: int = int(_wanted_until.get(key, 0))
	if until <= 0:
		return false
	if until < Time.get_ticks_msec():
		_wanted_until.erase(key)
		return false
	return true


func _rehydrate_from_ram(key: String) -> bool:
	var parts: PackedStringArray = key.split(":")
	if parts.size() != 4:
		return false
	var level: int = parts[0].to_int()
	var face: int = parts[1].to_int()
	var tile_x: int = parts[2].to_int()
	var tile_y: int = parts[3].to_int()
	var data: PackedFloat32Array = GroundHeightStore.resident_tile(level, face, tile_x, tile_y)
	if data.size() != TILE_VERTS * TILE_VERTS:
		return false
	_upload_page(level, face, tile_x, tile_y, data, false)
	if _key_to_slot.has(key):
		_ram_rehydrates += 1
		return true
	return false


## Prefer evicting a page that the renderer has not touched recently. With the
## local clipmap's working set this should make visible-page eviction effectively
## disappear even while stale I/O results are finishing in the background.
func _allocate_slot() -> int:
	if not _free_slots.is_empty():
		var index: int = _free_slots.size() - 1
		var free_slot: int = _free_slots[index]
		_free_slots.remove_at(index)
		return free_slot

	var oldest_unwanted_slot: int = -1
	var oldest_unwanted_tick: int = 0x7fffffffffffffff
	var oldest_any_slot: int = -1
	var oldest_any_tick: int = 0x7fffffffffffffff
	for slot: int in SLOT_COUNT:
		var meta: Dictionary = _slots[slot]
		if meta.is_empty():
			return slot
		var tick: int = int(meta.get("last", 0))
		if tick < oldest_any_tick:
			oldest_any_tick = tick
			oldest_any_slot = slot
		var key: String = String(meta.get("key", ""))
		if not _is_wanted(key) and tick < oldest_unwanted_tick:
			oldest_unwanted_tick = tick
			oldest_unwanted_slot = slot

	var chosen: int = oldest_unwanted_slot
	if chosen < 0:
		chosen = oldest_any_slot
		_forced_protected_evictions += 1
	if chosen < 0:
		return -1

	var old_key: String = String(_slots[chosen].get("key", ""))
	if not old_key.is_empty():
		_key_to_slot.erase(old_key)
		_remove_table_entry(old_key)
	_slots[chosen] = {}
	_evictions += 1
	return chosen


func stats() -> Dictionary:
	var result: Dictionary = super.stats()
	result["wanted_pages"] = _wanted_until.size()
	result["ignored_ready"] = _ignored_ready
	result["ram_rehydrates"] = _ram_rehydrates
	result["forced_protected_evictions"] = _forced_protected_evictions
	return result
