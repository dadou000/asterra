class_name SparseHydroAtlasGPU
extends Node
## GPU storage for transient hydrology tiles.
##
## Slots are fixed-size contiguous ranges in SSBOs. Stable HydroTileKey identity
## is mirrored into a portable uvec4-style metadata buffer as (face, level, x, y);
## the packed 64-bit Morton ID remains a CPU/save identity and is not required by
## shaders. Recycling a slot never changes world identity outside this binding.
##
## `cell_size_m` is the H0/base-level metric. Phase 4 may place coarser quadtree
## levels in the same atlas; their physical cell size is derived from metadata:
##
##   dx(level) = H0_dx * 2^(base_tile_level - level)
##
## This keeps one bounded atlas/pool while making physical metrics representation-
## aware. Standalone legacy tests may leave base_tile_level unset and retain the
## old single-dx behavior.

signal initialized
signal initialization_failed(error: Error)
signal released

const STATE_FLOATS := 4
const SOURCE_FLOATS := 4
const METADATA_INTS := 4

var capacity := 0
var tile_resolution := 0
var cell_size_m := 1.0
var base_tile_level := -1

var _state_a := RID()
var _state_b := RID()
var _sources := RID()
var _occupancy := RID()
var _tile_metadata := RID()
var _initialized := false
var _init_pending := false


func initialize(p_capacity: int, p_tile_resolution: int, p_cell_size_m: float,
		initial_state := PackedFloat32Array(), initial_occupancy := PackedInt32Array(),
		initial_metadata := PackedInt32Array()) -> Error:
	if _init_pending or _initialized:
		return ERR_BUSY
	if p_capacity <= 0 or p_tile_resolution <= 0 or p_cell_size_m <= 0.0:
		return ERR_INVALID_PARAMETER
	if RenderingServer.get_rendering_device() == null:
		return ERR_UNAVAILABLE

	capacity = p_capacity
	tile_resolution = p_tile_resolution
	cell_size_m = p_cell_size_m
	var cells := total_cell_count()
	var state_values: PackedFloat32Array = initial_state
	if state_values.is_empty():
		state_values = PackedFloat32Array()
		state_values.resize(cells * STATE_FLOATS)
	elif state_values.size() != cells * STATE_FLOATS:
		return ERR_INVALID_PARAMETER

	var occupancy_values: PackedInt32Array = initial_occupancy
	if occupancy_values.is_empty():
		occupancy_values = PackedInt32Array()
		occupancy_values.resize(capacity)
	elif occupancy_values.size() != capacity:
		return ERR_INVALID_PARAMETER

	var metadata_values: PackedInt32Array = initial_metadata
	if metadata_values.is_empty():
		metadata_values = _empty_metadata_values()
	elif metadata_values.size() != capacity * METADATA_INTS:
		return ERR_INVALID_PARAMETER

	var zero_state := PackedFloat32Array()
	zero_state.resize(cells * STATE_FLOATS)
	var zero_sources := PackedFloat32Array()
	zero_sources.resize(cells * SOURCE_FLOATS)
	_init_pending = true
	RenderingServer.call_on_render_thread(Callable(self, &"_init_render_thread").bind(
		state_values.to_byte_array(), zero_state.to_byte_array(),
		zero_sources.to_byte_array(), occupancy_values.to_byte_array(),
		metadata_values.to_byte_array()))
	return OK


func initialized_ok() -> bool:
	return _initialized


## Production sets this immediately after atlas creation and before any solver or
## source layer is initialized. H0 is the finest physical level owned by this atlas.
func set_base_tile_level(level: int) -> Error:
	if level < 0 or level > HydroTileKey.MAX_LEVEL:
		return ERR_INVALID_PARAMETER
	if _initialized:
		# Changing the metric while any published slot exists would reinterpret water
		# volume. It is only safe before allocation/publication.
		var empty := true
		# Occupancy is GPU-resident, so use the invariant that production calls this
		# before scheduler binding. Re-setting the same value remains idempotent.
		if base_tile_level >= 0 and base_tile_level != level:
			empty = false
		if not empty:
			return ERR_BUSY
	base_tile_level = level
	return OK


func hydrolod_enabled() -> bool:
	return base_tile_level >= 0


func physical_lod_for_level(level: int) -> int:
	if base_tile_level < 0:
		return 0
	return maxi(base_tile_level - clampi(level, 0, HydroTileKey.MAX_LEVEL), 0)


func cell_size_for_level(level: int) -> float:
	if base_tile_level < 0:
		return cell_size_m
	var delta := base_tile_level - clampi(level, 0, HydroTileKey.MAX_LEVEL)
	return cell_size_m * pow(2.0, float(delta))


func cell_area_for_level(level: int) -> float:
	var dx := cell_size_for_level(level)
	return dx * dx


func tile_width_for_level(level: int) -> float:
	return cell_size_for_level(level) * float(maxi(tile_resolution, 1))


func total_cell_count() -> int:
	return capacity * tile_resolution * tile_resolution


func cells_per_tile() -> int:
	return tile_resolution * tile_resolution


func state_a_rid() -> RID:
	return _state_a if _initialized else RID()


func state_b_rid() -> RID:
	return _state_b if _initialized else RID()


func source_rid() -> RID:
	return _sources if _initialized else RID()


func occupancy_rid() -> RID:
	return _occupancy if _initialized else RID()


func tile_metadata_rid() -> RID:
	return _tile_metadata if _initialized else RID()


func gpu_bytes_estimate() -> int:
	var cells := total_cell_count()
	return cells * (STATE_FLOATS * 4 * 2 + SOURCE_FLOATS * 4) \
		+ capacity * (4 + METADATA_INTS * 4)


func stats() -> Dictionary:
	return {
		"initialized": _initialized,
		"capacity": capacity,
		"tile_resolution": tile_resolution,
		"cell_size_m": cell_size_m,
		"base_tile_level": base_tile_level,
		"hydrolod_metrics": hydrolod_enabled(),
		"cells_per_tile": cells_per_tile(),
		"total_cells": total_cell_count(),
		"gpu_bytes": gpu_bytes_estimate(),
		"metadata_bytes": capacity * METADATA_INTS * 4,
	}


## Debug/bootstrap upload for one slot. Writes only canonical A and is retained for
## older tests. Production frontier allocation should use stage_slot_state().
func debug_upload_slot(slot: int, values: PackedFloat32Array) -> Error:
	if not _initialized or slot < 0 or slot >= capacity:
		return ERR_INVALID_PARAMETER
	if values.size() != cells_per_tile() * STATE_FLOATS:
		return ERR_INVALID_PARAMETER
	var offset := slot * cells_per_tile() * STATE_FLOATS * 4
	RenderingServer.call_on_render_thread(Callable(self, &"_update_buffer_render_thread").bind(
		_state_a, offset, values.to_byte_array()))
	return OK


## Initialize an ALLOCATING slot while it is still unpublished. Both A and B are
## written so no stale state from the prior owner can leak into later ping-pong or
## diagnostics. The values normally contain dry h/hu/hv plus the destination bed.
func stage_slot_state(slot: int, values: PackedFloat32Array) -> Error:
	if not _initialized or slot < 0 or slot >= capacity:
		return ERR_INVALID_PARAMETER
	if values.size() != cells_per_tile() * STATE_FLOATS:
		return ERR_INVALID_PARAMETER
	var offset := slot * cells_per_tile() * STATE_FLOATS * 4
	var bytes := values.to_byte_array()
	RenderingServer.call_on_render_thread(Callable(self, &"_stage_slot_render_thread").bind(
		offset, bytes))
	return OK


func set_occupancy(values: PackedInt32Array) -> Error:
	if not _initialized or values.size() != capacity:
		return ERR_INVALID_PARAMETER
	RenderingServer.call_on_render_thread(Callable(self, &"_update_buffer_render_thread").bind(
		_occupancy, 0, values.to_byte_array()))
	return OK


## Atomically from the policy layer's point of view, publish metadata before
## setting occupancy. A GPU classifier can never observe active=1 with the prior
## tile identity after slot recycling.
func bind_slot_key(slot: int, key: HydroTileKey) -> Error:
	if not _initialized or key == null or slot < 0 or slot >= capacity:
		return ERR_INVALID_PARAMETER
	var metadata := PackedInt32Array([key.face, key.level, key.x, key.y])
	RenderingServer.call_on_render_thread(Callable(self, &"_bind_slot_render_thread").bind(
		slot, metadata.to_byte_array()))
	return OK


## Clear occupancy before invalidating metadata so in-flight summary/frontier
## passes cannot treat a sentinel metadata row as a live tile.
func unbind_slot(slot: int) -> Error:
	if not _initialized or slot < 0 or slot >= capacity:
		return ERR_INVALID_PARAMETER
	var metadata := PackedInt32Array([-1, 0, 0, 0])
	RenderingServer.call_on_render_thread(Callable(self, &"_unbind_slot_render_thread").bind(
		slot, metadata.to_byte_array()))
	return OK


## Full synchronization helper used during bootstrap/test. Incremental production
## streaming should use bind_slot_key()/unbind_slot() as allocations change.
func sync_pool(pool: HydroTilePool) -> Error:
	if not _initialized or pool == null or pool.capacity != capacity:
		return ERR_INVALID_PARAMETER
	var occupancy := PackedInt32Array()
	occupancy.resize(capacity)
	var metadata := _empty_metadata_values()
	for slot in capacity:
		var id := pool.id_for_slot(slot)
		if id < 0:
			continue
		var key := HydroTileKey.unpack(id)
		# ALLOCATING slots intentionally stay invisible until activate_reserved()
		# emits tile_woken and the identity bridge publishes them.
		var record := pool.record(id)
		if int(record.get("state", HydroTilePool.TileState.ALLOCATING)) \
				== HydroTilePool.TileState.ALLOCATING:
			continue
		occupancy[slot] = 1
		var o := slot * METADATA_INTS
		metadata[o] = key.face
		metadata[o + 1] = key.level
		metadata[o + 2] = key.x
		metadata[o + 3] = key.y
	RenderingServer.call_on_render_thread(Callable(self, &"_sync_identity_render_thread").bind(
		metadata.to_byte_array(), occupancy.to_byte_array()))
	return OK


func _empty_metadata_values() -> PackedInt32Array:
	var values := PackedInt32Array()
	values.resize(capacity * METADATA_INTS)
	for slot in capacity:
		values[slot * METADATA_INTS] = -1
	return values


func _init_render_thread(state_a_bytes: PackedByteArray, state_b_bytes: PackedByteArray,
		source_bytes: PackedByteArray, occupancy_bytes: PackedByteArray,
		metadata_bytes: PackedByteArray) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null:
		call_deferred("_finish_init", ERR_UNAVAILABLE, {})
		return
	var a := rd.storage_buffer_create(state_a_bytes.size(), state_a_bytes)
	var b := rd.storage_buffer_create(state_b_bytes.size(), state_b_bytes)
	var src := rd.storage_buffer_create(source_bytes.size(), source_bytes)
	var occ := rd.storage_buffer_create(occupancy_bytes.size(), occupancy_bytes)
	var meta := rd.storage_buffer_create(metadata_bytes.size(), metadata_bytes)
	var created := [a, b, src, occ, meta]
	for value in created:
		var rid: RID = value
		if not rid.is_valid():
			_free_many(rd, created)
			call_deferred("_finish_init", ERR_CANT_CREATE, {})
			return
	call_deferred("_finish_init", OK, {
		"state_a": a, "state_b": b, "sources": src,
		"occupancy": occ, "tile_metadata": meta,
	})


func _finish_init(error: Error, bundle: Dictionary) -> void:
	_init_pending = false
	if error != OK:
		_initialized = false
		initialization_failed.emit(error)
		return
	_state_a = bundle["state_a"]
	_state_b = bundle["state_b"]
	_sources = bundle["sources"]
	_occupancy = bundle["occupancy"]
	_tile_metadata = bundle["tile_metadata"]
	_initialized = true
	initialized.emit()


func _update_buffer_render_thread(rid: RID, offset: int, bytes: PackedByteArray) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd != null and rid.is_valid():
		var err := rd.buffer_update(rid, offset, bytes.size(), bytes)
		if err != OK:
			push_error("SparseHydroAtlasGPU: buffer update failed (%d)" % int(err))


func _stage_slot_render_thread(offset: int, bytes: PackedByteArray) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null:
		return
	var err := rd.buffer_update(_state_a, offset, bytes.size(), bytes)
	if err != OK:
		push_error("SparseHydroAtlasGPU: staged A state upload failed (%d)" % int(err))
		return
	err = rd.buffer_update(_state_b, offset, bytes.size(), bytes)
	if err != OK:
		push_error("SparseHydroAtlasGPU: staged B state upload failed (%d)" % int(err))


func _bind_slot_render_thread(slot: int, metadata_bytes: PackedByteArray) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null:
		return
	var metadata_offset := slot * METADATA_INTS * 4
	var err := rd.buffer_update(_tile_metadata, metadata_offset,
		metadata_bytes.size(), metadata_bytes)
	if err != OK:
		push_error("SparseHydroAtlasGPU: metadata bind failed (%d)" % int(err))
		return
	var occupied := PackedInt32Array([1]).to_byte_array()
	err = rd.buffer_update(_occupancy, slot * 4, 4, occupied)
	if err != OK:
		push_error("SparseHydroAtlasGPU: occupancy bind failed (%d)" % int(err))


func _unbind_slot_render_thread(slot: int, metadata_bytes: PackedByteArray) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null:
		return
	var empty := PackedInt32Array([0]).to_byte_array()
	var err := rd.buffer_update(_occupancy, slot * 4, 4, empty)
	if err != OK:
		push_error("SparseHydroAtlasGPU: occupancy unbind failed (%d)" % int(err))
		return
	var metadata_offset := slot * METADATA_INTS * 4
	err = rd.buffer_update(_tile_metadata, metadata_offset,
		metadata_bytes.size(), metadata_bytes)
	if err != OK:
		push_error("SparseHydroAtlasGPU: metadata unbind failed (%d)" % int(err))


func _sync_identity_render_thread(metadata_bytes: PackedByteArray,
		occupancy_bytes: PackedByteArray) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null:
		return
	# Disable all ownership first; publish full metadata; then publish occupancy.
	var clear_error := rd.buffer_clear(_occupancy, 0, capacity * 4)
	if clear_error != OK:
		push_error("SparseHydroAtlasGPU: identity occupancy clear failed (%d)" % int(clear_error))
		return
	var err := rd.buffer_update(_tile_metadata, 0, metadata_bytes.size(), metadata_bytes)
	if err != OK:
		push_error("SparseHydroAtlasGPU: identity metadata sync failed (%d)" % int(err))
		return
	err = rd.buffer_update(_occupancy, 0, occupancy_bytes.size(), occupancy_bytes)
	if err != OK:
		push_error("SparseHydroAtlasGPU: identity occupancy sync failed (%d)" % int(err))


func _free_many(rd: RenderingDevice, values: Array) -> void:
	for value in values:
		if value is RID:
			var rid: RID = value
			if rid.is_valid():
				rd.free_rid(rid)


func release() -> void:
	if not _initialized and not _state_a.is_valid():
		return
	var rids := [_state_a, _state_b, _sources, _occupancy, _tile_metadata]
	_initialized = false
	_state_a = RID(); _state_b = RID(); _sources = RID()
	_occupancy = RID(); _tile_metadata = RID()
	base_tile_level = -1
	RenderingServer.call_on_render_thread(Callable(self, &"_release_render_thread").bind(rids))
	released.emit()


func _release_render_thread(rids: Array) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd != null:
		_free_many(rd, rids)


func _exit_tree() -> void:
	release()
