extends Node
## GPU-resident page cache for immutable ground-height tiles.
##
## GroundHeightStore owns disk/RAM residency. This node copies each completed
## 33x33 page into one of 256 persistent atlas slots exactly once, then publishes
## a tiny GPU hash table mapping (level, face, tile_x, tile_y) -> atlas slot.
## The geometry shader can therefore address world height pages directly without
## the old CPU-side 69x69 clipmap-window reconstruction/repacking step.
##
## Player edits remain a sparse overlay. Only atlas pages intersecting an edited
## region are rebuilt from their pristine RAM payload plus the current delta
## snapshot; ordinary travel never resamples page heights on the CPU.

const TILE_CELLS := 32
const TILE_SHIFT := 5
const HALF_TILE_CELLS := 16
const TILE_VERTS := TILE_CELLS + 1
const MAX_LEVEL := 6

const ATLAS_COLS := 16
const ATLAS_COL_SHIFT := 4
const ATLAS_ROWS := 16
const SLOT_COUNT := ATLAS_COLS * ATLAS_ROWS
const ATLAS_WIDTH := ATLAS_COLS * TILE_VERTS
const ATLAS_HEIGHT := ATLAS_ROWS * TILE_VERTS

# Low load factor keeps shader lookups short even with simple linear probing.
const PAGE_TABLE_CAPACITY := 2048
const PAGE_TABLE_MAX_PROBES := 16

var _atlas: DrawableTexture2D
var _page_table: ImageTexture
var _page_table_dirty := false

# key -> slot index. Slot dictionaries contain key + exact address + LRU tick.
var _key_to_slot: Dictionary = {}
var _slots: Array[Dictionary] = []
var _free_slots := PackedInt32Array()
var _clock := 0

var _pages_uploaded := 0
var _page_reuploads := 0
var _edit_reuploads := 0
var _evictions := 0
var _table_rebuilds := 0
var _table_insert_failures := 0
var _uploaded_texels := 0


func _ready() -> void:
	process_priority = 8
	GroundHeightStore.tile_ready.connect(_on_tile_ready)
	Planet.world_ready.connect(_on_world_ready)
	Planet.coast_profile_changed.connect(_on_coast_profile_changed)
	Deltas.region_changed.connect(_on_region_changed)
	_reset_cache()


func _process(_dt: float) -> void:
	if _page_table_dirty:
		_rebuild_page_table()


func atlas_texture() -> Texture2D:
	return _atlas


func page_table_texture() -> Texture2D:
	return _page_table


func ready_for_shader() -> bool:
	return _atlas != null and _page_table != null and not _key_to_slot.is_empty()


func atlas_size() -> Vector2:
	return Vector2(float(ATLAS_WIDTH), float(ATLAS_HEIGHT))


func table_capacity() -> int:
	return PAGE_TABLE_CAPACITY


func atlas_columns() -> int:
	return ATLAS_COLS


func slot_count() -> int:
	return SLOT_COUNT


## Touch all pages required by one bilinear sample. This makes visible/coarse
## coverage resistant to eviction by speculative pages arriving farther ahead.
func touch_sample(d: Vector3, level: int) -> bool:
	var addresses: Array[String] = _keys_for_sample(d, level)
	if addresses.is_empty():
		return false
	var all_present := true
	for key: String in addresses:
		if not _key_to_slot.has(key):
			all_present = false
			continue
		_touch_slot(int(_key_to_slot[key]))
	return all_present


func touch_samples(directions: Array[Vector3], level: int) -> bool:
	var all_present := true
	for d: Vector3 in directions:
		if not touch_sample(d, level):
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


func stats() -> Dictionary:
	return {
		"resident_pages": _key_to_slot.size(),
		"slot_capacity": SLOT_COUNT,
		"pages_uploaded": _pages_uploaded,
		"page_reuploads": _page_reuploads,
		"edit_reuploads": _edit_reuploads,
		"evictions": _evictions,
		"table_rebuilds": _table_rebuilds,
		"table_insert_failures": _table_insert_failures,
		"uploaded_texels": _uploaded_texels,
	}


func _on_world_ready(_fields: PlanetFields) -> void:
	_reset_cache()


func _on_coast_profile_changed() -> void:
	_reset_cache()


func _reset_cache() -> void:
	_key_to_slot.clear()
	_slots.clear()
	_slots.resize(SLOT_COUNT)
	_free_slots.clear()
	for slot: int in SLOT_COUNT:
		_slots[slot] = {}
		_free_slots.append(SLOT_COUNT - 1 - slot)
	_clock = 0

	_atlas = DrawableTexture2D.new()
	_atlas.setup(ATLAS_WIDTH, ATLAS_HEIGHT,
		DrawableTexture2D.DRAWABLE_FORMAT_RGBAF, Color(0.0, 0.0, 0.0, 1.0), false)

	var table_image: Image = Image.create(PAGE_TABLE_CAPACITY, 1, false, Image.FORMAT_RGBAF)
	_page_table = ImageTexture.create_from_image(table_image)
	_page_table_dirty = false

	_pages_uploaded = 0
	_page_reuploads = 0
	_edit_reuploads = 0
	_evictions = 0
	_table_rebuilds = 0
	_table_insert_failures = 0
	_uploaded_texels = 0


func _on_tile_ready(level: int, face: int, tile_x: int, tile_y: int) -> void:
	var data: PackedFloat32Array = GroundHeightStore.resident_tile(level, face, tile_x, tile_y)
	if data.size() != TILE_VERTS * TILE_VERTS:
		return
	_upload_page(level, face, tile_x, tile_y, data, false)


func _upload_page(level: int, face: int, tile_x: int, tile_y: int,
		data: PackedFloat32Array, edit_refresh: bool) -> void:
	var key: String = _page_key(level, face, tile_x, tile_y)
	var slot: int
	var is_new: bool = not _key_to_slot.has(key)
	if is_new:
		slot = _allocate_slot()
		if slot < 0:
			return
		_key_to_slot[key] = slot
		_slots[slot] = {
			"key": key,
			"level": level,
			"face": face,
			"tile_x": tile_x,
			"tile_y": tile_y,
			"last": 0,
		}
		_page_table_dirty = true
	else:
		slot = int(_key_to_slot[key])

	_touch_slot(slot)
	var image: Image = _page_image_with_deltas(level, face, tile_x, tile_y, data)
	if image == null:
		return
	var source: ImageTexture = ImageTexture.create_from_image(image)
	if source == null:
		return

	var atlas_x: int = (slot & (ATLAS_COLS - 1)) * TILE_VERTS
	var atlas_y: int = (slot >> ATLAS_COL_SHIFT) * TILE_VERTS
	_atlas.blit_rect(Rect2i(atlas_x, atlas_y, TILE_VERTS, TILE_VERTS), source)
	_uploaded_texels += TILE_VERTS * TILE_VERTS
	if is_new:
		_pages_uploaded += 1
	else:
		_page_reuploads += 1
	if edit_refresh:
		_edit_reuploads += 1


func _allocate_slot() -> int:
	if not _free_slots.is_empty():
		var index: int = _free_slots.size() - 1
		var slot: int = _free_slots[index]
		_free_slots.remove_at(index)
		return slot

	var oldest_slot := -1
	var oldest_tick := 0x7fffffffffffffff
	for slot: int in SLOT_COUNT:
		var meta: Dictionary = _slots[slot]
		if meta.is_empty():
			return slot
		var tick: int = int(meta.get("last", 0))
		if tick < oldest_tick:
			oldest_tick = tick
			oldest_slot = slot
	if oldest_slot < 0:
		return -1

	var old_key: String = String(_slots[oldest_slot].get("key", ""))
	if not old_key.is_empty():
		_key_to_slot.erase(old_key)
	_slots[oldest_slot] = {}
	_evictions += 1
	_page_table_dirty = true
	return oldest_slot


func _touch_slot(slot: int) -> void:
	if slot < 0 or slot >= _slots.size() or _slots[slot].is_empty():
		return
	_clock += 1
	_slots[slot]["last"] = _clock


func _rebuild_page_table() -> void:
	_page_table_dirty = false
	var packed := PackedFloat32Array()
	packed.resize(PAGE_TABLE_CAPACITY * 4)
	_table_insert_failures = 0

	for slot: int in SLOT_COUNT:
		var meta: Dictionary = _slots[slot]
		if meta.is_empty():
			continue
		var level: int = int(meta["level"])
		var face: int = int(meta["face"])
		var tile_x: int = int(meta["tile_x"])
		var tile_y: int = int(meta["tile_y"])
		var page_code: int = level * 6 + face + 1
		var start: int = _page_hash(page_code, tile_x, tile_y)
		var inserted := false
		for probe: int in PAGE_TABLE_MAX_PROBES:
			var table_index: int = (start + probe) % PAGE_TABLE_CAPACITY
			var p: int = table_index * 4
			if packed[p + 3] < 0.5:
				packed[p] = float(page_code)
				packed[p + 1] = float(tile_x)
				packed[p + 2] = float(tile_y)
				packed[p + 3] = float(slot + 1)
				inserted = true
				break
		if not inserted:
			_table_insert_failures += 1

	var image: Image = Image.create_from_data(PAGE_TABLE_CAPACITY, 1, false,
		Image.FORMAT_RGBAF, packed.to_byte_array())
	if _page_table == null:
		_page_table = ImageTexture.create_from_image(image)
	else:
		_page_table.update(image)
	_table_rebuilds += 1


static func _page_hash(page_code: int, tile_x: int, tile_y: int) -> int:
	# Values stay below signed 32-bit range for the current depth-16 lattice.
	var h: int = tile_x * 1973 + tile_y * 9277 + page_code * 26699
	var result: int = h % PAGE_TABLE_CAPACITY
	return result + PAGE_TABLE_CAPACITY if result < 0 else result


static func _page_key(level: int, face: int, tile_x: int, tile_y: int) -> String:
	return "%d:%d:%d:%d" % [level, face, tile_x, tile_y]


func _keys_for_sample(d: Vector3, level: int) -> Array[String]:
	var keys: Array[String] = []
	if Planet.cfg == null:
		return keys
	var used_level: int = clampi(level, 0, MAX_LEVEL)
	var fuv: Array = CubeSphere.dir_to_face_uv(d.normalized())
	var face: int = int(fuv[0])
	var cells: int = GroundHeightStore.cells_per_face(used_level)
	if cells <= 0:
		return keys
	var fx: float = clampf((float(fuv[1]) * 0.5 + 0.5) * float(cells), 0.0, float(cells))
	var fy: float = clampf((float(fuv[2]) * 0.5 + 0.5) * float(cells), 0.0, float(cells))
	var x0: int = int(floor(fx))
	var y0: int = int(floor(fy))
	var x1: int = mini(x0 + 1, cells)
	var y1: int = mini(y0 + 1, cells)
	var vertices: Array[Vector2i] = [
		Vector2i(x0, y0), Vector2i(x1, y0),
		Vector2i(x0, y1), Vector2i(x1, y1),
	]
	for vertex: Vector2i in vertices:
		var addr: Vector2i = _tile_address_for_vertex(vertex.x, vertex.y, cells)
		var key: String = _page_key(used_level, face, addr.x, addr.y)
		if not keys.has(key):
			keys.append(key)
	return keys


static func _tile_address_for_vertex(ix: int, iy: int, cells: int) -> Vector2i:
	var tile_count: int = (cells + TILE_CELLS - 1) >> TILE_SHIFT
	return Vector2i(
		clampi((maxi(ix, 1) - 1) >> TILE_SHIFT, 0, tile_count - 1),
		clampi((maxi(iy, 1) - 1) >> TILE_SHIFT, 0, tile_count - 1))


func _page_image_with_deltas(level: int, face: int, tile_x: int, tile_y: int,
		data: PackedFloat32Array) -> Image:
	var cells: int = GroundHeightStore.cells_per_face(level)
	if cells <= 0:
		return null
	var start_x: int = tile_x * TILE_CELLS
	var start_y: int = tile_y * TILE_CELLS
	var center_x: int = mini(start_x + HALF_TILE_CELLS, cells)
	var center_y: int = mini(start_y + HALF_TILE_CELLS, cells)
	var center_u: float = -1.0 + 2.0 * float(center_x) / float(cells)
	var center_v: float = -1.0 + 2.0 * float(center_y) / float(cells)
	var center_dir: Vector3 = CubeSphere.face_uv_to_dir(face, center_u, center_v)
	var page_angle: float = (PI * 0.5 * float(TILE_CELLS) / float(cells)) * 0.95
	var snap: Dictionary = Deltas.snapshot_for_bounds(center_dir, page_angle)
	if snap.is_empty():
		return Image.create_from_data(TILE_VERTS, TILE_VERTS, false,
			Image.FORMAT_RF, data.to_byte_array())

	var edited: PackedFloat32Array = data.duplicate()
	for local_y: int in TILE_VERTS:
		var gy: int = mini(start_y + local_y, cells)
		var v: float = -1.0 + 2.0 * float(gy) / float(cells)
		for local_x: int in TILE_VERTS:
			var gx: int = mini(start_x + local_x, cells)
			var u: float = -1.0 + 2.0 * float(gx) / float(cells)
			var d: Vector3 = CubeSphere.face_uv_to_dir(face, u, v)
			var index: int = local_y * TILE_VERTS + local_x
			edited[index] += Deltas.offset_at_snapshot(d, snap)
	return Image.create_from_data(TILE_VERTS, TILE_VERTS, false,
		Image.FORMAT_RF, edited.to_byte_array())


func _on_region_changed(center: Vector3, radius_m: float) -> void:
	if Planet.cfg == null or _key_to_slot.is_empty():
		return
	var radius: float = Planet.cfg.planet_radius
	var edit_dir: Vector3 = center.normalized()
	for slot: int in SLOT_COUNT:
		var meta: Dictionary = _slots[slot]
		if meta.is_empty():
			continue
		if not _page_intersects_region(meta, edit_dir, radius_m, radius):
			continue
		var level: int = int(meta["level"])
		var face: int = int(meta["face"])
		var tile_x: int = int(meta["tile_x"])
		var tile_y: int = int(meta["tile_y"])
		var data: PackedFloat32Array = GroundHeightStore.resident_tile(level, face, tile_x, tile_y)
		if data.size() == TILE_VERTS * TILE_VERTS:
			_upload_page(level, face, tile_x, tile_y, data, true)
		else:
			# The RAM LRU may have dropped this page. Remove the stale edited copy;
			# the renderer will request it again if it is still visible.
			var key: String = String(meta["key"])
			_key_to_slot.erase(key)
			_slots[slot] = {}
			_free_slots.append(slot)
			_page_table_dirty = true


func _page_intersects_region(meta: Dictionary, edit_dir: Vector3,
		edit_radius_m: float, radius: float) -> bool:
	var level: int = int(meta["level"])
	var face: int = int(meta["face"])
	var tile_x: int = int(meta["tile_x"])
	var tile_y: int = int(meta["tile_y"])
	var cells: int = GroundHeightStore.cells_per_face(level)
	if cells <= 0:
		return false
	var gx: int = mini(tile_x * TILE_CELLS + HALF_TILE_CELLS, cells)
	var gy: int = mini(tile_y * TILE_CELLS + HALF_TILE_CELLS, cells)
	var u: float = -1.0 + 2.0 * float(gx) / float(cells)
	var v: float = -1.0 + 2.0 * float(gy) / float(cells)
	var page_dir: Vector3 = CubeSphere.face_uv_to_dir(face, u, v)
	var page_radius_m: float = PI * 0.5 * radius * float(TILE_CELLS) / float(cells) * 0.95
	var total_angle: float = (edit_radius_m + page_radius_m) / radius
	return page_dir.dot(edit_dir) >= cos(total_angle)
