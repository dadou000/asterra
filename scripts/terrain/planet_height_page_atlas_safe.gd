extends "res://scripts/terrain/planet_height_page_atlas.gd"
## Hardware-safe page-table variant for the spherical terrain renderer.
##
## Godot's DrawableTexture2D backend rejects any dimension above 16383. The
## original 16384x1 page table therefore never acquired a valid RID, which caused
## the repeated `uninitialized RID` errors and forced every shader lookup onto a
## broken resource. This override keeps the 4096-page height atlas but uses an
## 8192-entry table and a shorter probe budget.

const SAFE_PAGE_TABLE_CAPACITY: int = 8192
const SAFE_PAGE_TABLE_MAX_PROBES: int = 12


func _ready() -> void:
	# Do not run the parent _ready(): it constructs the invalid 16384-wide table
	# before virtual dispatch can be relied upon. Reproduce only its connections,
	# then initialize the hardware-safe resources directly.
	process_priority = 8
	GroundHeightStore.tile_ready.connect(_on_tile_ready)
	Planet.world_ready.connect(_on_world_ready)
	Planet.coast_profile_changed.connect(_on_coast_profile_changed)
	Deltas.region_changed.connect(_on_region_changed)
	_reset_cache()


func table_capacity() -> int:
	return SAFE_PAGE_TABLE_CAPACITY


func table_max_probes() -> int:
	return SAFE_PAGE_TABLE_MAX_PROBES


func _reset_cache() -> void:
	_key_to_slot.clear()
	_slots.clear()
	_slots.resize(SLOT_COUNT)
	_free_slots.clear()
	for slot: int in SLOT_COUNT:
		_slots[slot] = {}
		_free_slots.append(SLOT_COUNT - 1 - slot)
	_clock = 0
	_wanted_until.clear()
	_next_prune_msec = 0

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
	_key_to_table_index.clear()
	_table_tombstones = 0


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
				_write_table_cell_safe(table_index)
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
		_write_table_cell_safe(target)
		return true

	if first_tombstone >= 0:
		_set_table_entry_cpu(first_tombstone, key, page_code, tile_x, tile_y, slot + 1)
		_table_tombstones = maxi(_table_tombstones - 1, 0)
		_write_table_cell_safe(first_tombstone)
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
	_table_tombstones += 1
	_write_table_cell_safe(index)


func _write_table_cell_safe(index: int) -> void:
	if _page_table == null or index < 0 or index >= SAFE_PAGE_TABLE_CAPACITY:
		return
	var patch: Image = Image.create(1, 1, false, Image.FORMAT_RGBAF)
	patch.set_pixel(0, 0, Color(
		float(_table_codes[index]), float(_table_x[index]),
		float(_table_y[index]), float(_table_slots[index])))
	var source: ImageTexture = ImageTexture.create_from_image(patch)
	if source == null:
		return
	_page_table.blit_rect(Rect2i(index, 0, 1, 1), source,
		Color.WHITE, 0, _direct_copy_material())
	_table_cell_updates += 1


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

	var table_image: Image = Image.create(SAFE_PAGE_TABLE_CAPACITY, 1, false, Image.FORMAT_RGBAF)
	for index: int in SAFE_PAGE_TABLE_CAPACITY:
		if _table_slots[index] <= 0:
			continue
		table_image.set_pixel(index, 0, Color(
			float(_table_codes[index]), float(_table_x[index]),
			float(_table_y[index]), float(_table_slots[index])))
	var source: ImageTexture = ImageTexture.create_from_image(table_image)
	var replacement := DrawableTexture2D.new()
	replacement.setup(SAFE_PAGE_TABLE_CAPACITY, 1,
		DrawableTexture2D.DRAWABLE_FORMAT_RGBAF, Color(0.0, 0.0, 0.0, 0.0), false)
	if source != null:
		replacement.blit_rect(Rect2i(0, 0, SAFE_PAGE_TABLE_CAPACITY, 1), source,
			Color.WHITE, 0, _direct_copy_material())
	_page_table = replacement
	_table_full_rebuilds += 1


static func _safe_page_hash(page_code: int, tile_x: int, tile_y: int) -> int:
	var h: int = tile_x * 1973 + tile_y * 9277 + page_code * 26699
	var result: int = h % SAFE_PAGE_TABLE_CAPACITY
	return result + SAFE_PAGE_TABLE_CAPACITY if result < 0 else result
