class_name SparseHydroAtlasGPU
extends Node
## Phase 3 GPU storage for transient hydrology tiles.
##
## Slots are fixed-size contiguous ranges in SSBOs. Stable HydroTileKey identity
## is managed separately by HydroTilePool; recycling a slot never changes world
## identity. This first atlas deliberately favors simple, inspectable FP32 storage
## over packing tricks.

signal initialized
signal initialization_failed(error: Error)
signal released

const STATE_FLOATS := 4
const SOURCE_FLOATS := 4

var capacity := 0
var tile_resolution := 0
var cell_size_m := 1.0

var _state_a := RID()
var _state_b := RID()
var _sources := RID()
var _occupancy := RID()
var _initialized := false
var _init_pending := false


func initialize(p_capacity: int, p_tile_resolution: int, p_cell_size_m: float,
		initial_state := PackedFloat32Array(), initial_occupancy := PackedInt32Array()) -> Error:
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

	var zero_state := PackedFloat32Array()
	zero_state.resize(cells * STATE_FLOATS)
	var zero_sources := PackedFloat32Array()
	zero_sources.resize(cells * SOURCE_FLOATS)
	_init_pending = true
	RenderingServer.call_on_render_thread(Callable(self, &"_init_render_thread").bind(
		state_values.to_byte_array(), zero_state.to_byte_array(),
		zero_sources.to_byte_array(), occupancy_values.to_byte_array()))
	return OK


func initialized_ok() -> bool:
	return _initialized


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


func gpu_bytes_estimate() -> int:
	var cells := total_cell_count()
	return cells * (STATE_FLOATS * 4 * 2 + SOURCE_FLOATS * 4) + capacity * 4


func stats() -> Dictionary:
	return {
		"initialized": _initialized,
		"capacity": capacity,
		"tile_resolution": tile_resolution,
		"cell_size_m": cell_size_m,
		"cells_per_tile": cells_per_tile(),
		"total_cells": total_cell_count(),
		"gpu_bytes": gpu_bytes_estimate(),
	}


## Debug/bootstrap upload for one slot. Production active tiles will be initialized
## by GPU reconstruction/prolongation rather than regular CPU uploads.
func debug_upload_slot(slot: int, values: PackedFloat32Array) -> Error:
	if not _initialized or slot < 0 or slot >= capacity:
		return ERR_INVALID_PARAMETER
	if values.size() != cells_per_tile() * STATE_FLOATS:
		return ERR_INVALID_PARAMETER
	var offset := slot * cells_per_tile() * STATE_FLOATS * 4
	RenderingServer.call_on_render_thread(Callable(self, &"_update_buffer_render_thread").bind(
		_state_a, offset, values.to_byte_array()))
	return OK


func set_occupancy(values: PackedInt32Array) -> Error:
	if not _initialized or values.size() != capacity:
		return ERR_INVALID_PARAMETER
	RenderingServer.call_on_render_thread(Callable(self, &"_update_buffer_render_thread").bind(
		_occupancy, 0, values.to_byte_array()))
	return OK


func _init_render_thread(state_a_bytes: PackedByteArray, state_b_bytes: PackedByteArray,
		source_bytes: PackedByteArray, occupancy_bytes: PackedByteArray) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null:
		call_deferred("_finish_init", ERR_UNAVAILABLE, {})
		return
	var a := rd.storage_buffer_create(state_a_bytes.size(), state_a_bytes)
	var b := rd.storage_buffer_create(state_b_bytes.size(), state_b_bytes)
	var src := rd.storage_buffer_create(source_bytes.size(), source_bytes)
	var occ := rd.storage_buffer_create(occupancy_bytes.size(), occupancy_bytes)
	var created := [a, b, src, occ]
	for rid in created:
		if not (rid as RID).is_valid():
			_free_many(rd, created)
			call_deferred("_finish_init", ERR_CANT_CREATE, {})
			return
	call_deferred("_finish_init", OK, {
		"state_a": a, "state_b": b, "sources": src, "occupancy": occ,
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
	_initialized = true
	initialized.emit()


func _update_buffer_render_thread(rid: RID, offset: int, bytes: PackedByteArray) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd != null and rid.is_valid():
		var err := rd.buffer_update(rid, offset, bytes.size(), bytes)
		if err != OK:
			push_error("SparseHydroAtlasGPU: buffer update failed (%d)" % int(err))


func _free_many(rd: RenderingDevice, values: Array) -> void:
	for value in values:
		if value is RID:
			var rid: RID = value
			if rid.is_valid():
				rd.free_rid(rid)


func release() -> void:
	if not _initialized and not _state_a.is_valid():
		return
	var rids := [_state_a, _state_b, _sources, _occupancy]
	_initialized = false
	_state_a = RID(); _state_b = RID(); _sources = RID(); _occupancy = RID()
	RenderingServer.call_on_render_thread(Callable(self, &"_release_render_thread").bind(rids))
	released.emit()


func _release_render_thread(rids: Array) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd != null:
		_free_many(rd, rids)


func _exit_tree() -> void:
	release()
