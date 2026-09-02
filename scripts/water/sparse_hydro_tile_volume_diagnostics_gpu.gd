class_name SparseHydroTileVolumeDiagnosticsGPU
extends Node
## Compact occupancy-aware volume diagnostic for one sparse atlas slot.
##
## Used by fine->coarse collapse. The authoritative atlas state remains on GPU; the
## CPU receives only one four-byte FP32 volume after a two-pass reduction. The
## helper owns no atlas state/occupancy lifetime.

signal initialized
signal initialization_failed(error: Error)
signal volume_ready(request_id: int, slot: int, volume_m3: float)
signal readback_failed(request_id: int, slot: int, error: Error)
signal released

const LOCAL_SIZE := 256
const PARAM_BYTES := 32

var _state := RID()
var _occupancy := RID()
var _reduce_shader := RID()
var _reduce_pipeline := RID()
var _final_shader := RID()
var _final_pipeline := RID()
var _partials := RID()
var _result := RID()
var _params := RID()
var _reduce_set := RID()
var _final_set := RID()

var _capacity := 0
var _tile_resolution := 0
var _cells_per_tile := 0
var _cell_size_m := 1.0
var _partial_count := 0
var _initialized := false
var _init_pending := false
var _readback_pending := false
var _next_request_id := 1


func initialize(state_rid: RID, occupancy_rid: RID, capacity: int,
		tile_resolution: int, cell_size_m: float) -> Error:
	if _initialized or _init_pending or _readback_pending:
		return ERR_BUSY
	if not state_rid.is_valid() or not occupancy_rid.is_valid() \
			or capacity <= 0 or tile_resolution <= 0 \
			or not is_finite(cell_size_m) or cell_size_m <= 0.0:
		return ERR_INVALID_PARAMETER
	if RenderingServer.get_rendering_device() == null:
		return ERR_UNAVAILABLE
	var reduce_file: RDShaderFile = load(
		"res://shaders/water/sparse_hydro_tile_mass_reduce.glsl")
	var final_file: RDShaderFile = load(
		"res://shaders/water/sparse_hydro_mass_finalize.glsl")
	if reduce_file == null or final_file == null:
		return ERR_CANT_OPEN
	var reduce_spirv := reduce_file.get_spirv()
	var final_spirv := final_file.get_spirv()
	if reduce_spirv == null or final_spirv == null:
		return ERR_CANT_CREATE

	_state = state_rid
	_occupancy = occupancy_rid
	_capacity = capacity
	_tile_resolution = tile_resolution
	_cells_per_tile = tile_resolution * tile_resolution
	_cell_size_m = cell_size_m
	_partial_count = int(ceil(float(_cells_per_tile) / float(LOCAL_SIZE)))
	_init_pending = true
	RenderingServer.call_on_render_thread(Callable(self, &"_init_render_thread").bind(
		reduce_spirv, final_spirv, _make_params(0)))
	return OK


func initialize_from_atlas(atlas: SparseHydroAtlasGPU,
		use_state_b: bool = false) -> Error:
	if atlas == null or not atlas.initialized_ok():
		return ERR_INVALID_PARAMETER
	return initialize(atlas.state_b_rid() if use_state_b else atlas.state_a_rid(),
		atlas.occupancy_rid(), atlas.capacity, atlas.tile_resolution,
		atlas.cell_size_m)


func initialized_ok() -> bool:
	return _initialized


func pending() -> bool:
	return _readback_pending


func request_volume(slot: int) -> int:
	if not _initialized or _readback_pending or slot < 0 or slot >= _capacity:
		return -1
	var request_id := _next_request_id
	_next_request_id += 1
	_readback_pending = true
	RenderingServer.call_on_render_thread(
		Callable(self, &"_request_render_thread").bind(request_id, slot))
	return request_id


func stats() -> Dictionary:
	return {
		"initialized": _initialized,
		"pending": _readback_pending,
		"capacity": _capacity,
		"tile_resolution": _tile_resolution,
		"cells_per_tile": _cells_per_tile,
		"cell_size_m": _cell_size_m,
		"partial_count": _partial_count,
		"gpu_bytes": maxi(_partial_count, 1) * 4 + 4 + PARAM_BYTES,
	}


func _make_params(slot: int) -> PackedByteArray:
	var bytes := PackedByteArray()
	bytes.resize(PARAM_BYTES)
	bytes.encode_u32(0, _cells_per_tile)
	bytes.encode_u32(4, _capacity)
	bytes.encode_u32(8, maxi(slot, 0))
	bytes.encode_u32(12, _partial_count)
	bytes.encode_float(16, _cell_size_m)
	bytes.encode_float(20, 0.0)
	bytes.encode_float(24, 0.0)
	bytes.encode_float(28, 0.0)
	return bytes


func _init_render_thread(reduce_spirv: RDShaderSPIRV,
		final_spirv: RDShaderSPIRV, param_bytes: PackedByteArray) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null:
		call_deferred(&"_finish_init", ERR_UNAVAILABLE, {})
		return
	var resources: Array = []
	var reduce_shader := rd.shader_create_from_spirv(reduce_spirv)
	if not reduce_shader.is_valid():
		call_deferred(&"_finish_init", ERR_CANT_CREATE, {})
		return
	resources.append(reduce_shader)
	var final_shader := rd.shader_create_from_spirv(final_spirv)
	if not final_shader.is_valid():
		_free_many(rd, resources)
		call_deferred(&"_finish_init", ERR_CANT_CREATE, {})
		return
	resources.append(final_shader)
	var reduce_pipeline := rd.compute_pipeline_create(reduce_shader)
	if not reduce_pipeline.is_valid():
		_free_many(rd, resources)
		call_deferred(&"_finish_init", ERR_CANT_CREATE, {})
		return
	resources.append(reduce_pipeline)
	var final_pipeline := rd.compute_pipeline_create(final_shader)
	if not final_pipeline.is_valid():
		_free_many(rd, resources)
		call_deferred(&"_finish_init", ERR_CANT_CREATE, {})
		return
	resources.append(final_pipeline)

	var partial_bytes := PackedByteArray()
	partial_bytes.resize(maxi(_partial_count, 1) * 4)
	var result_bytes := PackedByteArray()
	result_bytes.resize(4)
	var partials := rd.storage_buffer_create(partial_bytes.size(), partial_bytes)
	var result := rd.storage_buffer_create(result_bytes.size(), result_bytes)
	var params := rd.storage_buffer_create(PARAM_BYTES, param_bytes)
	if not partials.is_valid() or not result.is_valid() or not params.is_valid():
		_free_many(rd, resources + [partials, result, params])
		call_deferred(&"_finish_init", ERR_CANT_CREATE, {})
		return
	resources.append(partials)
	resources.append(result)
	resources.append(params)

	var reduce_set := rd.uniform_set_create([
		_storage_uniform(0, _state),
		_storage_uniform(1, _occupancy),
		_storage_uniform(2, partials),
		_storage_uniform(3, params),
	], reduce_shader, 0)
	if not reduce_set.is_valid():
		_free_many(rd, resources)
		call_deferred(&"_finish_init", ERR_CANT_CREATE, {})
		return
	resources.append(reduce_set)
	var final_set := rd.uniform_set_create([
		_storage_uniform(0, partials),
		_storage_uniform(1, result),
		_storage_uniform(2, params),
	], final_shader, 0)
	if not final_set.is_valid():
		_free_many(rd, resources)
		call_deferred(&"_finish_init", ERR_CANT_CREATE, {})
		return

	call_deferred(&"_finish_init", OK, {
		"reduce_shader": reduce_shader,
		"reduce_pipeline": reduce_pipeline,
		"final_shader": final_shader,
		"final_pipeline": final_pipeline,
		"partials": partials,
		"result": result,
		"params": params,
		"reduce_set": reduce_set,
		"final_set": final_set,
	})


func _finish_init(error: Error, bundle: Dictionary) -> void:
	_init_pending = false
	if error != OK:
		_initialized = false
		initialization_failed.emit(error)
		return
	_reduce_shader = bundle["reduce_shader"]
	_reduce_pipeline = bundle["reduce_pipeline"]
	_final_shader = bundle["final_shader"]
	_final_pipeline = bundle["final_pipeline"]
	_partials = bundle["partials"]
	_result = bundle["result"]
	_params = bundle["params"]
	_reduce_set = bundle["reduce_set"]
	_final_set = bundle["final_set"]
	_initialized = true
	initialized.emit()


func _request_render_thread(request_id: int, slot: int) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null or not _all_valid():
		call_deferred(&"_fail_readback", request_id, slot, ERR_UNAVAILABLE)
		return
	var slot_bytes := PackedByteArray()
	slot_bytes.resize(4)
	slot_bytes.encode_u32(0, slot)
	var update_error := rd.buffer_update(_params, 8, 4, slot_bytes)
	if update_error != OK:
		call_deferred(&"_fail_readback", request_id, slot, update_error)
		return

	var compute := rd.compute_list_begin()
	rd.compute_list_bind_compute_pipeline(compute, _reduce_pipeline)
	rd.compute_list_bind_uniform_set(compute, _reduce_set, 0)
	rd.compute_list_dispatch(compute, maxi(_partial_count, 1), 1, 1)
	rd.compute_list_add_barrier(compute)
	rd.compute_list_bind_compute_pipeline(compute, _final_pipeline)
	rd.compute_list_bind_uniform_set(compute, _final_set, 0)
	rd.compute_list_dispatch(compute, 1, 1, 1)
	rd.compute_list_end()
	var callback := Callable(self, &"_on_volume_bytes").bind(request_id, slot)
	var err := rd.buffer_get_data_async(_result, callback, 0, 4)
	if err != OK:
		call_deferred(&"_fail_readback", request_id, slot, err)


func _on_volume_bytes(bytes: PackedByteArray, request_id: int, slot: int) -> void:
	if bytes.size() < 4:
		call_deferred(&"_fail_readback", request_id, slot, ERR_INVALID_DATA)
		return
	call_deferred(&"_publish_volume", request_id, slot, bytes.decode_float(0))


func _publish_volume(request_id: int, slot: int, volume_m3: float) -> void:
	_readback_pending = false
	volume_ready.emit(request_id, slot, volume_m3)


func _fail_readback(request_id: int, slot: int, error: Error) -> void:
	_readback_pending = false
	readback_failed.emit(request_id, slot, error)


func _all_valid() -> bool:
	return _state.is_valid() and _occupancy.is_valid() \
		and _reduce_pipeline.is_valid() and _final_pipeline.is_valid() \
		and _partials.is_valid() and _result.is_valid() and _params.is_valid() \
		and _reduce_set.is_valid() and _final_set.is_valid()


func _storage_uniform(binding: int, rid: RID) -> RDUniform:
	var uniform := RDUniform.new()
	uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
	uniform.binding = binding
	uniform.add_id(rid)
	return uniform


func _free_many(rd: RenderingDevice, values: Array) -> void:
	for value in values:
		if value is RID:
			var rid: RID = value
			if rid.is_valid():
				rd.free_rid(rid)


func release() -> void:
	if not _initialized and not _reduce_shader.is_valid():
		return
	var rids := [_reduce_set, _final_set, _partials, _result, _params,
		_reduce_pipeline, _final_pipeline, _reduce_shader, _final_shader]
	_initialized = false
	_init_pending = false
	_readback_pending = false
	_reduce_set = RID(); _final_set = RID()
	_partials = RID(); _result = RID(); _params = RID()
	_reduce_pipeline = RID(); _final_pipeline = RID()
	_reduce_shader = RID(); _final_shader = RID()
	_state = RID(); _occupancy = RID()
	RenderingServer.call_on_render_thread(
		Callable(self, &"_release_render_thread").bind(rids))
	released.emit()


func _release_render_thread(rids: Array) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd != null:
		_free_many(rd, rids)


func _exit_tree() -> void:
	release()
