class_name HydroSourceTermsGPU
extends Node
## Rebuilds SparseHydroAtlasGPU.source_rid() from compact aggregated entries.
##
## The full source-term grid is never uploaded from CPU. update_entries() clears the
## GPU buffer then uploads at most `max_entries` 32-byte records containing one
## occupied slot/cell and its depth/momentum rates.

signal initialized
signal initialization_failed(error: Error)
signal update_recorded(request_id: int, entry_count: int)
signal update_failed(request_id: int, error: Error)
signal released

const LOCAL_X := 64
const ENTRY_BYTES := 32
const PARAM_BYTES := 16

var max_entries := 8192

var _atlas: SparseHydroAtlasGPU
var _shader := RID()
var _pipeline := RID()
var _entries := RID()
var _params := RID()
var _uniform_set := RID()
var _initialized := false
var _init_pending := false
var _update_pending := false
var _next_request_id := 1


func initialize(atlas: SparseHydroAtlasGPU, p_max_entries: int = 8192) -> Error:
	if _initialized or _init_pending or _update_pending:
		return ERR_BUSY
	if atlas == null or not atlas.initialized_ok() or p_max_entries <= 0:
		return ERR_INVALID_PARAMETER
	if RenderingServer.get_rendering_device() == null:
		return ERR_UNAVAILABLE
	var shader_file: RDShaderFile = load("res://shaders/water/hydro_source_terms.glsl")
	if shader_file == null:
		return ERR_CANT_OPEN
	var spirv := shader_file.get_spirv()
	if spirv == null:
		return ERR_CANT_CREATE
	_atlas = atlas
	max_entries = p_max_entries
	_init_pending = true
	RenderingServer.call_on_render_thread(
		Callable(self, &"_init_render_thread").bind(spirv))
	return OK


func initialized_ok() -> bool:
	return _initialized


func pending() -> bool:
	return _update_pending


## Entry contract:
## {
##   "slot": int,
##   "cell": int,
##   "add_depth_rate_mps": float,
##   "remove_depth_rate_mps": float,
##   "hu_rate": float,
##   "hv_rate": float,
## }
##
## Callers MUST aggregate duplicate slot/cell pairs before this method.
func update_entries(entries: Array[Dictionary]) -> int:
	if not _initialized or _update_pending or entries.size() > max_entries:
		return -1
	var packed := PackedByteArray()
	packed.resize(maxi(entries.size(), 1) * ENTRY_BYTES)
	for i in entries.size():
		var e := entries[i]
		var o := i * ENTRY_BYTES
		packed.encode_u32(o + 0, maxi(int(e.get("slot", -1)), 0))
		packed.encode_u32(o + 4, maxi(int(e.get("cell", -1)), 0))
		packed.encode_u32(o + 8, 0)
		packed.encode_u32(o + 12, 0)
		packed.encode_float(o + 16, maxf(float(e.get("add_depth_rate_mps", 0.0)), 0.0))
		packed.encode_float(o + 20, maxf(float(e.get("remove_depth_rate_mps", 0.0)), 0.0))
		packed.encode_float(o + 24, float(e.get("hu_rate", 0.0)))
		packed.encode_float(o + 28, float(e.get("hv_rate", 0.0)))

	var request_id := _next_request_id
	_next_request_id += 1
	_update_pending = true
	var params := PackedInt32Array([
		entries.size(), _atlas.cells_per_tile(), _atlas.capacity,
		_atlas.total_cell_count(),
	]).to_byte_array()
	RenderingServer.call_on_render_thread(Callable(self, &"_update_render_thread").bind(
		request_id, entries.size(), packed, params))
	return request_id


func gpu_bytes_estimate() -> int:
	return max_entries * ENTRY_BYTES + PARAM_BYTES


func _init_render_thread(spirv: RDShaderSPIRV) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null:
		call_deferred("_finish_init", ERR_UNAVAILABLE, {})
		return
	var shader := rd.shader_create_from_spirv(spirv)
	if not shader.is_valid():
		call_deferred("_finish_init", ERR_CANT_CREATE, {})
		return
	var pipeline := rd.compute_pipeline_create(shader)
	if not pipeline.is_valid():
		rd.free_rid(shader)
		call_deferred("_finish_init", ERR_CANT_CREATE, {})
		return
	var entry_bytes := PackedByteArray()
	entry_bytes.resize(max_entries * ENTRY_BYTES)
	var param_bytes := PackedByteArray()
	param_bytes.resize(PARAM_BYTES)
	var entry_buffer := rd.storage_buffer_create(entry_bytes.size(), entry_bytes)
	var params := rd.storage_buffer_create(param_bytes.size(), param_bytes)
	if not entry_buffer.is_valid() or not params.is_valid():
		_free_many(rd, [entry_buffer, params, pipeline, shader])
		call_deferred("_finish_init", ERR_CANT_CREATE, {})
		return
	var set_rid := rd.uniform_set_create([
		_storage_uniform(0, entry_buffer),
		_storage_uniform(1, _atlas.source_rid()),
		_storage_uniform(2, _atlas.occupancy_rid()),
		_storage_uniform(3, params),
	], shader, 0)
	if not set_rid.is_valid():
		_free_many(rd, [entry_buffer, params, pipeline, shader])
		call_deferred("_finish_init", ERR_CANT_CREATE, {})
		return
	call_deferred("_finish_init", OK, {
		"shader": shader, "pipeline": pipeline, "entries": entry_buffer,
		"params": params, "set": set_rid,
	})


func _finish_init(error: Error, bundle: Dictionary) -> void:
	_init_pending = false
	if error != OK:
		initialization_failed.emit(error)
		return
	_shader = bundle["shader"]
	_pipeline = bundle["pipeline"]
	_entries = bundle["entries"]
	_params = bundle["params"]
	_uniform_set = bundle["set"]
	_initialized = true
	initialized.emit()


func _update_render_thread(request_id: int, entry_count: int,
		entry_bytes: PackedByteArray, param_bytes: PackedByteArray) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null or not _pipeline.is_valid() or not _uniform_set.is_valid():
		call_deferred("_finish_update", request_id, entry_count, ERR_UNAVAILABLE)
		return
	var clear_error := rd.buffer_clear(_atlas.source_rid(), 0,
		_atlas.total_cell_count() * SparseHydroAtlasGPU.SOURCE_FLOATS * 4)
	if clear_error != OK:
		call_deferred("_finish_update", request_id, entry_count, clear_error)
		return
	if entry_count > 0:
		var upload_size := entry_count * ENTRY_BYTES
		var err := rd.buffer_update(_entries, 0, upload_size, entry_bytes)
		if err != OK:
			call_deferred("_finish_update", request_id, entry_count, err)
			return
		err = rd.buffer_update(_params, 0, PARAM_BYTES, param_bytes)
		if err != OK:
			call_deferred("_finish_update", request_id, entry_count, err)
			return
		var compute := rd.compute_list_begin()
		rd.compute_list_bind_compute_pipeline(compute, _pipeline)
		rd.compute_list_bind_uniform_set(compute, _uniform_set, 0)
		rd.compute_list_dispatch(compute,
			int(ceil(float(entry_count) / float(LOCAL_X))), 1, 1)
		rd.compute_list_end()
	call_deferred("_finish_update", request_id, entry_count, OK)


func _finish_update(request_id: int, entry_count: int, error: Error) -> void:
	_update_pending = false
	if error != OK:
		update_failed.emit(request_id, error)
	else:
		update_recorded.emit(request_id, entry_count)


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
			if rid.is_valid():
				rd.free_rid(rid)


func release() -> void:
	if not _initialized and not _shader.is_valid():
		return
	var rids := [_uniform_set, _entries, _params, _pipeline, _shader]
	_initialized = false
	_update_pending = false
	_uniform_set = RID(); _entries = RID(); _params = RID()
	_pipeline = RID(); _shader = RID()
	_atlas = null
	RenderingServer.call_on_render_thread(
		Callable(self, &"_release_render_thread").bind(rids))
	released.emit()


func _release_render_thread(rids: Array) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd != null:
		_free_many(rd, rids)


func _exit_tree() -> void:
	release()
