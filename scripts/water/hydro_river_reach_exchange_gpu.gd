class_name HydroRiverReachExchangeGPU
extends Node
## Compact operator-split river exchange dispatcher.
##
## One 64-byte record describes one resident sparse river member. Boundary flags
## select whether that member owns the external upstream and/or downstream mouth.
## Multi-tile clusters therefore exchange only at their two outer ends while normal
## sparse SWE carries water across internal member interfaces.

signal initialized
signal initialization_failed(error: Error)
signal exchange_ready(request_id: int, results: Array[Dictionary])
signal exchange_failed(request_id: int, error: Error)
signal released

const RECORD_BYTES := 64
const RESULT_BYTES := 16
const PARAM_BYTES := 32
const FLAG_UPSTREAM := 1
const FLAG_DOWNSTREAM := 2

var max_records := 256

var _atlas: SparseHydroAtlasGPU
var _shader := RID()
var _pipeline := RID()
var _records := RID()
var _results := RID()
var _params := RID()
var _uniform_set := RID()
var _initialized := false
var _init_pending := false
var _exchange_pending := false
var _next_request_id := 1
var _pending_cells := PackedInt32Array()


func initialize(atlas: SparseHydroAtlasGPU, p_max_records: int = 256) -> Error:
	if _initialized or _init_pending or _exchange_pending:
		return ERR_BUSY
	if atlas == null or not atlas.initialized_ok() or p_max_records <= 0:
		return ERR_INVALID_PARAMETER
	if RenderingServer.get_rendering_device() == null:
		return ERR_UNAVAILABLE
	var shader_file: RDShaderFile = load("res://shaders/water/hydro_river_reach_exchange.glsl")
	if shader_file == null or shader_file.get_spirv() == null:
		return ERR_CANT_OPEN
	_atlas = atlas
	max_records = p_max_records
	_init_pending = true
	RenderingServer.call_on_render_thread(Callable(self, &"_init_render_thread").bind(
		shader_file.get_spirv()))
	return OK


func initialized_ok() -> bool:
	return _initialized


func pending() -> bool:
	return _exchange_pending


## Record contract:
## {cell, slot, center_cell:Vector2, direction_cell:Vector2, half_width_m,
##  add_volume_m3, exchange_dt_s, mouth_cells, add_velocity:Vector2,
##  upstream_enabled=true, downstream_enabled=true}
func exchange(records: Array[Dictionary]) -> int:
	if not _initialized or _exchange_pending or records.is_empty() \
			or records.size() > max_records:
		return -1
	var bytes := PackedByteArray()
	bytes.resize(records.size() * RECORD_BYTES)
	_pending_cells.resize(records.size())
	for i in records.size():
		var rec := records[i]
		var slot := int(rec.get("slot", -1))
		var cell := int(rec.get("cell", -1))
		if slot < 0 or slot >= _atlas.capacity or cell < 0:
			return -1
		var center: Vector2 = rec.get("center_cell", Vector2.ZERO)
		var direction: Vector2 = rec.get("direction_cell", Vector2.ZERO)
		var velocity: Vector2 = rec.get("add_velocity", Vector2.ZERO)
		if direction.length_squared() <= 1.0e-12:
			return -1
		direction = direction.normalized()
		var flags := 0
		if bool(rec.get("upstream_enabled", true)):
			flags |= FLAG_UPSTREAM
		if bool(rec.get("downstream_enabled", true)):
			flags |= FLAG_DOWNSTREAM
		_pending_cells[i] = cell
		var o := i * RECORD_BYTES
		bytes.encode_u32(o + 0, slot)
		bytes.encode_u32(o + 4, flags); bytes.encode_u32(o + 8, 0); bytes.encode_u32(o + 12, 0)
		bytes.encode_float(o + 16, center.x); bytes.encode_float(o + 20, center.y)
		bytes.encode_float(o + 24, direction.x); bytes.encode_float(o + 28, direction.y)
		bytes.encode_float(o + 32, maxf(float(rec.get("half_width_m", 0.0)), 0.0))
		bytes.encode_float(o + 36, maxf(float(rec.get("add_volume_m3", 0.0)), 0.0))
		bytes.encode_float(o + 40, maxf(float(rec.get("exchange_dt_s", 0.0)), 0.0))
		bytes.encode_float(o + 44, clampf(float(rec.get("mouth_cells", 1.25)), 0.75, 3.0))
		bytes.encode_float(o + 48, velocity.x); bytes.encode_float(o + 52, velocity.y)
		bytes.encode_float(o + 56, 0.0); bytes.encode_float(o + 60, 0.0)

	var params := PackedByteArray()
	params.resize(PARAM_BYTES)
	params.encode_u32(0, records.size())
	params.encode_u32(4, _atlas.tile_resolution)
	params.encode_u32(8, _atlas.capacity)
	params.encode_u32(12, _atlas.cells_per_tile())
	params.encode_float(16, _atlas.cell_size_m)

	var request_id := _next_request_id
	_next_request_id += 1
	_exchange_pending = true
	RenderingServer.call_on_render_thread(Callable(self, &"_exchange_render_thread").bind(
		request_id, records.size(), bytes, params))
	return request_id


func gpu_bytes_estimate() -> int:
	return max_records * (RECORD_BYTES + RESULT_BYTES) + PARAM_BYTES


func _init_render_thread(spirv: RDShaderSPIRV) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null:
		call_deferred(&"_finish_init", ERR_UNAVAILABLE, {})
		return
	var shader := rd.shader_create_from_spirv(spirv)
	var pipeline := rd.compute_pipeline_create(shader) if shader.is_valid() else RID()
	var record_bytes := PackedByteArray(); record_bytes.resize(max_records * RECORD_BYTES)
	var result_bytes := PackedByteArray(); result_bytes.resize(max_records * RESULT_BYTES)
	var param_bytes := PackedByteArray(); param_bytes.resize(PARAM_BYTES)
	var records := rd.storage_buffer_create(record_bytes.size(), record_bytes)
	var results := rd.storage_buffer_create(result_bytes.size(), result_bytes)
	var params := rd.storage_buffer_create(param_bytes.size(), param_bytes)
	if not shader.is_valid() or not pipeline.is_valid() or not records.is_valid() \
			or not results.is_valid() or not params.is_valid():
		_free_many(rd, [params, results, records, pipeline, shader])
		call_deferred(&"_finish_init", ERR_CANT_CREATE, {})
		return
	var set_rid := rd.uniform_set_create([
		_storage_uniform(0, _atlas.state_a_rid()),
		_storage_uniform(1, _atlas.state_b_rid()),
		_storage_uniform(2, _atlas.occupancy_rid()),
		_storage_uniform(3, records),
		_storage_uniform(4, results),
		_storage_uniform(5, params),
	], shader, 0)
	if not set_rid.is_valid():
		_free_many(rd, [params, results, records, pipeline, shader])
		call_deferred(&"_finish_init", ERR_CANT_CREATE, {})
		return
	call_deferred(&"_finish_init", OK, {
		"shader": shader, "pipeline": pipeline, "records": records,
		"results": results, "params": params, "set": set_rid,
	})


func _finish_init(error: Error, bundle: Dictionary) -> void:
	_init_pending = false
	if error != OK:
		initialization_failed.emit(error)
		return
	_shader = bundle["shader"]
	_pipeline = bundle["pipeline"]
	_records = bundle["records"]
	_results = bundle["results"]
	_params = bundle["params"]
	_uniform_set = bundle["set"]
	_initialized = true
	initialized.emit()


func _exchange_render_thread(request_id: int, record_count: int,
		record_bytes: PackedByteArray, param_bytes: PackedByteArray) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null or not _pipeline.is_valid() or not _uniform_set.is_valid():
		call_deferred(&"_exchange_error", request_id, ERR_UNAVAILABLE)
		return
	var err := rd.buffer_update(_records, 0, record_count * RECORD_BYTES, record_bytes)
	if err == OK:
		err = rd.buffer_update(_params, 0, PARAM_BYTES, param_bytes)
	if err != OK:
		call_deferred(&"_exchange_error", request_id, err)
		return
	var compute := rd.compute_list_begin()
	rd.compute_list_bind_compute_pipeline(compute, _pipeline)
	rd.compute_list_bind_uniform_set(compute, _uniform_set, 0)
	rd.compute_list_dispatch(compute, record_count, 1, 1)
	rd.compute_list_end()
	var callback := Callable(self, &"_on_result_bytes").bind(request_id, record_count)
	err = rd.buffer_get_data_async(_results, callback, 0, record_count * RESULT_BYTES)
	if err != OK:
		call_deferred(&"_exchange_error", request_id, err)


func _on_result_bytes(bytes: PackedByteArray, request_id: int, record_count: int) -> void:
	if bytes.size() < record_count * RESULT_BYTES:
		call_deferred(&"_exchange_error", request_id, ERR_INVALID_DATA)
		return
	var out: Array[Dictionary] = []
	for i in record_count:
		var o := i * RESULT_BYTES
		out.append({
			"cell": int(_pending_cells[i]),
			"added_m3": maxf(bytes.decode_float(o + 0), 0.0),
			"removed_m3": maxf(bytes.decode_float(o + 4), 0.0),
			"measured_downstream_q_m3s": maxf(bytes.decode_float(o + 8), 0.0),
			"mouth_or_status": bytes.decode_float(o + 12),
		})
	call_deferred(&"_exchange_success", request_id, out)


func _exchange_success(request_id: int, results: Array[Dictionary]) -> void:
	_exchange_pending = false
	_pending_cells = PackedInt32Array()
	exchange_ready.emit(request_id, results)


func _exchange_error(request_id: int, error: Error) -> void:
	_exchange_pending = false
	_pending_cells = PackedInt32Array()
	exchange_failed.emit(request_id, error)


func _storage_uniform(binding: int, rid: RID) -> RDUniform:
	var u := RDUniform.new()
	u.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
	u.binding = binding
	u.add_id(rid)
	return u


func _free_many(rd: RenderingDevice, values: Array) -> void:
	for value in values:
		if value is RID:
			var rid: RID = value
			if rid.is_valid(): rd.free_rid(rid)


func release() -> void:
	if not _initialized and not _shader.is_valid():
		return
	var rids := [_uniform_set, _records, _results, _params, _pipeline, _shader]
	_initialized = false
	_exchange_pending = false
	_uniform_set = RID(); _records = RID(); _results = RID(); _params = RID()
	_pipeline = RID(); _shader = RID(); _atlas = null
	_pending_cells = PackedInt32Array()
	RenderingServer.call_on_render_thread(Callable(self, &"_release_render_thread").bind(rids))
	released.emit()


func _release_render_thread(rids: Array) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd != null: _free_many(rd, rids)


func _exit_tree() -> void:
	release()
