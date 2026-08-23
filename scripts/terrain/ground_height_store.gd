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

const CACHE_VERSION := 1
const MAGIC := 0x41535448 # "ASTH"
const TARGET_FINE_DEPTH := 16
const MAX_LEVEL := 6
const TILE_CELLS := 32
const TILE_VERTS := TILE_CELLS + 1
const MEMORY_TILE_LIMIT := 256
const CACHE_DIR := "terrain_height_cache"

var _namespace := "unconfigured"
var _memory: Dictionary = {}            # tile key -> PackedFloat32Array
var _last_use: Dictionary = {}          # tile key -> monotonically increasing tick
var _clock := 0
var _mutex := Mutex.new()
var _disk_hits := 0
var _memory_hits := 0
var _tiles_built := 0


func _ready() -> void:
	Planet.world_ready.connect(_on_world_ready)
	Planet.coast_profile_changed.connect(_on_coast_profile_changed)
	if Planet.ready_state and Planet.cfg != null:
		_configure_namespace()


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
func sample_height(d: Vector3, level: int, snap: Dictionary = {}) -> float:
	var h := sample_pristine(d, level)
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
	var fuv := CubeSphere.dir_to_face_uv(d.normalized())
	var face := int(fuv[0])
	var cells := _cells_per_face(used_level)
	var fx := clampf((float(fuv[1]) * 0.5 + 0.5) * float(cells), 0.0, float(cells))
	var fy := clampf((float(fuv[2]) * 0.5 + 0.5) * float(cells), 0.0, float(cells))
	var x0 := int(floor(fx))
	var y0 := int(floor(fy))
	var x1 := mini(x0 + 1, cells)
	var y1 := mini(y0 + 1, cells)
	var tx := fx - float(x0)
	var ty := fy - float(y0)

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
	# Diagnostic-only snapshot. Avoid taking the cache mutex from the render thread;
	# these integer counters can tolerate a one-update race.
	return {
		"namespace": _namespace,
		"memory_tiles": _memory.size(),
		"memory_hits": _memory_hits,
		"disk_hits": _disk_hits,
		"tiles_built": _tiles_built,
	}


func _cells_per_face(level: int) -> int:
	var depth := TARGET_FINE_DEPTH - level
	return Planet.cfg.chunk_grid * (1 << depth)


func _sample_vertex_locked(level: int, face: int, ix: int, iy: int, cells: int) -> float:
	var tile_count := int(ceil(float(cells) / float(TILE_CELLS)))
	# Boundary vertices belong to the tile on their lower side so the final face
	# edge is represented by local index TILE_CELLS instead of creating a phantom
	# extra tile containing only one row/column.
	var tile_x := clampi(int(floor(float(maxi(ix, 1) - 1) / float(TILE_CELLS))),
		0, tile_count - 1)
	var tile_y := clampi(int(floor(float(maxi(iy, 1) - 1) / float(TILE_CELLS))),
		0, tile_count - 1)
	var local_x := ix - tile_x * TILE_CELLS
	var local_y := iy - tile_y * TILE_CELLS
	var tile := _get_tile_locked(level, face, tile_x, tile_y, cells)
	return tile[local_y * TILE_VERTS + local_x]


func _get_tile_locked(level: int, face: int, tile_x: int, tile_y: int,
		cells: int) -> PackedFloat32Array:
	var key := "%d:%d:%d:%d" % [level, face, tile_x, tile_y]
	if _memory.has(key):
		_memory_hits += 1
		_touch_locked(key)
		return _memory[key]

	var loaded := _load_tile_locked(level, face, tile_x, tile_y)
	if loaded.size() == TILE_VERTS * TILE_VERTS:
		_disk_hits += 1
		_remember_locked(key, loaded)
		return loaded

	var built := _build_tile_locked(level, face, tile_x, tile_y, cells)
	_tiles_built += 1
	_write_tile_locked(level, face, tile_x, tile_y, built)
	_remember_locked(key, built)
	return built


func _build_tile_locked(level: int, face: int, tile_x: int, tile_y: int,
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


func _relative_path(level: int, face: int, tile_x: int, tile_y: int) -> String:
	return "%s/%s/l%d/f%d/%d_%d.ghz" % [
		CACHE_DIR, _namespace, level, face, tile_x, tile_y]


func _load_tile_locked(level: int, face: int, tile_x: int, tile_y: int) -> PackedFloat32Array:
	var relative := _relative_path(level, face, tile_x, tile_y)
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


func _write_tile_locked(level: int, face: int, tile_x: int, tile_y: int,
		data: PackedFloat32Array) -> void:
	var relative := _relative_path(level, face, tile_x, tile_y)
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
