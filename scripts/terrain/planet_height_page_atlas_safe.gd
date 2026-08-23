extends "res://scripts/terrain/planet_height_page_atlas.gd"
## Hardware-safe page-table variant for the spherical terrain renderer.
##
## Keeps the 4096-page atlas and an 8192-entry page table, bounds eviction scans,
## batches page-table publication, and encodes a short per-page refinement fade in
## the fractional part of the positive slot metadata. The shader uses that fade to
## morph newly resident fine geometry from its parent instead of snapping in as a
## rectangular 33x33 height block.

const SAFE_PAGE_TABLE_CAPACITY: int = 8192
const SAFE_PAGE_TABLE_MAX_PROBES: int = 12
const EVICTION_SCAN_BUDGET: int = 256
const TABLE_FLUSH_INTERVAL_MS: int = 33
const PAGE_REFINEMENT_FADE_MS: int = 550
const PAGE_BLEND_ENCODING_SCALE: float = 0.875

var _eviction_cursor: int = 0
var _table_dirty: bool = false
var _next_table_flush_msec: int = 0
var _table_blend := PackedFloat32Array()
# table index -> upload start msec
var _fading_indices: Dictionary = {}


func _ready() -> void:
	process_priority = 8
	GroundHeightStore.tile_ready.connect(_on_tile_ready)
	Planet.world_ready.connect(_on_world_ready)
	Planet.coast_profile_changed.connect(_on_coast_profile_changed)
	Deltas.region_changed.connect(_on_region_changed)
	_reset_cache()


func _process(dt: float) -> void:
	# Preserve wanted-page TTL pruning from the base atlas.
	super._process(dt)
	var now: int = Time.get_ticks_msec()
	_advance_page_fades(now)
	if not _table_dirty or now < _next_table_flush_msec:
		return
	_next_table_flush_msec = now + TABLE_FLUSH_INTERVAL_MS
	_flush_table_batch()


func table_capacity() -> int:
	return SAFE_PAGE_TABLE_CAPACITY


func table_max_probes() -> int:
	return SAFE_PAGE_TABLE_MAX_PROBES


func page_blend_encoding_scale() -> float:
	return PAGE_BLEND_ENCODING_SCALE


func _reset_cache() -> void:
	_key_to_slot.clear()
	_slots.clear()
	_slots.resize(SLOT_COUNT)
	_free_slots.clear()
	for slot: int in SLOT_COUNT:
		_slots[slot] = {}
		_free_slots.append(SLOT_COUNT - 1 - slot)
	_clock = 0
	_eviction_cursor = 0
	_wanted_until.clear()
	_next_prune_msec = 0
	_table_dirty = false
	_next_table_flush_msec = 0
	_fading_indices.clear()

	_atlas = DrawableTexture2D.new()
	_atlas.setup(ATLAS_WIDTH, ATLAS_HEIGHT,
		DrawableTexture2D.DRAWABLE_FORMAT_RGBAF, Color(0.0, 0.0, 0.0, 1.0), false)

	_reset_table_cpu()
	_page_table = DrawableTexture2D.new()
	_page_table.setup(SAFE_PAGE_TABLE_CAPACITY, 1,
		DrawableTexture2D.DRAWABLE_FORMAT_RGBAF, Color(0.0, 0.0, 0.0, 0.0), false)

	_pages_uploaded = 0
	_page_reuploads = 0
	_edit_reuploads = 0
	_evictions = 0
	_forced_protected_evictions = 0
	_ignored_ready = 0
	_ram_rehydrates = 0
	_table_cell_updates = 0
	_table_full_rebuilds = 0
	_table_insert_failures = 0
	_uploaded_texels = 0


func _reset_table_cpu() -> void:
	_table_codes.resize(SAFE_PAGE_TABLE_CAPACITY)
	_table_codes.fill(0)
	_table_x.resize(SAFE_PAGE_TABLE_CAPACITY)
	_table_x.fill(0)
	_table_y.resize(SAFE_PAGE_TABLE_CAPACITY)
	_table_y.fill(0)
	_table_slots.resize(SAFE_PAGE_TABLE_CAPACITY)
	_table_slots.fill(0)
	_table_blend.resize(SAFE_PAGE_TABLE_CAPACITY)
	_table_blend.fill(0.0)
	_key_to_table_index.clear()
	_table_tombstones = 0
	_fading_indices.clear()


func _allocate_slot() -> int:
	if not _free_slots.is_empty():
		var index: int = _free_slots.size() - 1
		var free_slot: int = _free_slots[index]
		_free_slots.remove_at(index)
		return free_slot

	var best_unwanted: int = -1
	var best_unwanted_tick: int = 0x7fffffffffffffff
	var best_any: int = -1
	var best_any_tick: int = 0x7fffffffffffffff
	var scan_count: int = mini(EVICTION_SCAN_BUDGET, SLOT_COUNT)
	for offset: int in scan_count:
		var slot: int = (_eviction_cursor + offset) % SLOT_COUNT
		var meta: Dictionary = _slots[slot]
		if meta.is_empty():
			_eviction_cursor = (slot + 1) % SLOT_COUNT
			return slot
		var tick: int = int(meta.get("last", 0))
		if tick < best_any_tick:
			best_any_tick = tick
			best_any = slot
		var key: String = String(meta.get("key", ""))
		if not _is_wanted(key) and tick < best_unwanted_tick:
			best_unwanted_tick = tick
			best_unwanted = slot

	var chosen: int = best_unwanted
	if chosen < 0:
		chosen = best_any
		_forced_protected_evictions += 1
	if chosen < 0:
		return -1

	_eviction_cursor = (chosen + 1) % SLOT_COUNT
	var old_key: String = String(_slots[chosen].get("key", ""))
	if not old_key.is_empty():
		_key_to_slot.erase(old_key)
		_remove_table_entry(old_key)
	_slots[chosen] = {}
	_evictions += 1
	return chosen


func _insert_table_entry(key: String, level: int, face: int,
		tile_x: int, tile_y: int, slot: int) -> bool:
	var page_code: int = level * 6 + face + 1
	var start: int = _safe_page_hash(page_code, tile_x, tile_y)
	var first_tombstone: int = -1

	for probe: int in SAFE_PAGE_TABLE_MAX_PROBES:
		var table_index: int = (start + probe) % SAFE_PAGE_TABLE_CAPACITY
		var state: int = _table_slots[table_index]
		if state > 0:
			if _table_codes[table_index] == page_code \
					and _table_x[table_index] == tile_x \
					and _table_y[table_index] == tile_y:
				_table_slots[table_index] = slot + 1
				_key_to_table_index[key] = table_index
				_mark_table_dirty()
				return true
			continue
		if state < 0:
			if first_tombstone < 0:
				first_tombstone = table_index
			continue

		var target: int = first_tombstone if first_tombstone >= 0 else table_index
		_set_table_entry_cpu(target, key, page_code, tile_x, tile_y, slot + 1)
		if first_tombstone >= 0:
			_table_tombstones = maxi(_table_tombstones - 1, 0)
		_mark_table_dirty()
		return true

	if first_tombstone >= 0:
		_set_table_entry_cpu(first_tombstone, key, page_code, tile_x, tile_y, slot + 1)
		_table_tombstones = maxi(_table_tombstones - 1, 0)
		_mark_table_dirty()
		return true

	_table_insert_failures += 1
	return false


func _insert_table_entry_cpu_only(key: String, level: int, face: int,
		tile_x: int, tile_y: int, slot: int) -> bool:
	var page_code: int = level * 6 + face + 1
	var start: int = _safe_page_hash(page_code, tile_x, tile_y)
	for probe: int in SAFE_PAGE_TABLE_MAX_PROBES:
		var table_index: int = (start + probe) % SAFE_PAGE_TABLE_CAPACITY
		if _table_slots[table_index] == 0:
			_set_table_entry_cpu(table_index, key, page_code, tile_x, tile_y, slot + 1)
			return true
	return false


func _set_table_entry_cpu(index: int, key: String, page_code: int,
		tile_x: int, tile_y: int, slot_plus_one: int) -> void:
	_table_codes[index] = page_code
	_table_x[index] = tile_x
	_table_y[index] = tile_y
	_table_slots[index] = slot_plus_one
	_table_blend[index] = 0.0
	_key_to_table_index[key] = index
	_fading_indices[index] = Time.get_ticks_msec()


func _remove_table_entry(key: String) -> void:
	var index: int = int(_key_to_table_index.get(key, -1))
	if index < 0 or index >= SAFE_PAGE_TABLE_CAPACITY:
		_key_to_table_index.erase(key)
		return
	_key_to_table_index.erase(key)
	if _table_slots[index] <= 0:
		return
	_table_codes[index] = 0
	_table_x[index] = 0
	_table_y[index] = 0
	_table_slots[index] = -1
	_table_blend[index] = 0.0
	_fading_indices.erase(index)
	_table_tombstones += 1
	_mark_table_dirty()


func _advance_page_fades(now: int) -> void:
	if _fading_indices.is_empty():
		return
	var finished: Array[int] = []
	var changed := false
	for index_value: Variant in _fading_indices.keys():
		var index: int = int(index_value)
		if index < 0 or index >= SAFE_PAGE_TABLE_CAPACITY or _table_slots[index] <= 0:
			finished.append(index)
			continue
		var started: int = int(_fading_indices[index])
		var t: float = clampf(float(now - started) / float(PAGE_REFINEMENT_FADE_MS), 0.0, 1.0)
		# Smoothstep avoids visible acceleration at either end of the geometry morph.
		var smooth_t: float = t * t * (3.0 - 2.0 * t)
		if absf(_table_blend[index] - smooth_t) > 0.005:
			_table_blend[index] = smooth_t
			changed = true
		if t >= 1.0:
			_table_blend[index] = 1.0
			finished.append(index)
	for index: int in finished:
		_fading_indices.erase(index)
	if changed or not finished.is_empty():
		_mark_table_dirty()


func _mark_table_dirty() -> void:
	_table_dirty = true
	_table_cell_updates += 1


func _flush_table_batch() -> void:
	if _page_table == null:
		return
	var table_image: Image = Image.create(SAFE_PAGE_TABLE_CAPACITY, 1, false, Image.FORMAT_RGBAF)
	for index: int in SAFE_PAGE_TABLE_CAPACITY:
		var state: int = _table_slots[index]
		if state == 0:
			continue
		var encoded_state: float = float(state)
		if state > 0:
			encoded_state += clampf(_table_blend[index], 0.0, 1.0) * PAGE_BLEND_ENCODING_SCALE
		table_image.set_pixel(index, 0, Color(
			float(_table_codes[index]), float(_table_x[index]),
			float(_table_y[index]), encoded_state))
	var source: ImageTexture = ImageTexture.create_from_image(table_image)
	if source == null:
		return
	_page_table.blit_rect(Rect2i(0, 0, SAFE_PAGE_TABLE_CAPACITY, 1), source,
		Color.WHITE, 0, _direct_copy_material())
	_table_dirty = false


func _rebuild_page_table_full() -> void:
	_reset_table_cpu()
	for slot: int in SLOT_COUNT:
		var meta: Dictionary = _slots[slot]
		if meta.is_empty():
			continue
		var key: String = String(meta["key"])
		if not _insert_table_entry_cpu_only(key, int(meta["level"]), int(meta["face"]),
				int(meta["tile_x"]), int(meta["tile_y"]), slot):
			_table_insert_failures += 1
	# A compaction must not make every already-resident page visibly refine again.
	_table_blend.fill(1.0)
	_fading_indices.clear()
	_flush_table_batch()
	_table_full_rebuilds += 1


static func _safe_page_hash(page_code: int, tile_x: int, tile_y: int) -> int:
	var h: int = tile_x * 1973 + tile_y * 9277 + page_code * 26699
	var result: int = h % SAFE_PAGE_TABLE_CAPACITY
	return result + SAFE_PAGE_TABLE_CAPACITY if result < 0 else result


func stats() -> Dictionary:
	var out: Dictionary = super.stats()
	out["fading_pages"] = _fading_indices.size()
	out["page_fade_ms"] = PAGE_REFINEMENT_FADE_MS
	return out
