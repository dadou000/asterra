class_name HydroVolumeDiagnosticsGPU
extends Node
## Debug/soak helper that measures total water volume entirely on the global GPU.
## Only the final four-byte result is read back asynchronously.

signal initialized
signal initialization_failed(error: Error)
signal volume_ready(request_id: int, volume_m3: float)
signal readback_failed(request_id: int, error: Error)
signal released

const LOCAL_X := 8
const LOCAL_Y := 8

var _state := RID()
var _reduce_shader := RID()
var _reduce_pipeline := RID()
var _final_shader := RID()
var _final_pipeline := RID()
var _partials := RID()
var _result := RID()
var _params := RID()
var _reduce_set := RID()
var _final_set := RID()

var _width := 0
var _height := 0
var _dx := 1.0
var _groups_x := 0
var _groups_y := 0
var _partial_count := 0
var _initialized := false
var _init_pending := false
var _readback_pending := false
var _next_request_id := 1


func initialize(state_rid: RID, width: int, height: int, cell_size_m: float) -> Error:
	if _init_pending or _readback_pending:
		return ERR_BUSY
	if not state_rid.is_valid() or width <= 0 or height <= 0 or cell_size_m <= 0.0:
		return ERR_INVALID_PARAMETER
	if RenderingServer.get_rendering_device() == null:
		return ERR_UNAVAILABLE
	var reduce_file: RDShaderFile = load("res://shaders/water/hydro_mass_reduce.glsl")
	var final_file: RDShaderFile = load("res://shaders/water/hydro_mass_finalize.glsl")
	if reduce_file == null or final_file == null:
		return ERR_CANT_OPEN

	_state = state_rid
	_width = width
	_height = height
	_dx = cell_size_m
	_groups_x = int(ceil(float(width) / float(LOCAL_X)))
	_groups_y = int(ceil(float(height) / float(LOCAL_Y)))
	_partial_count = _groups_x * _groups_y
	_init_pending = true
	RenderingServer.call_on_render_thread(Callable(self, &"_init_render_thread").bind(
		reduce_file.get_spirv(), final_file.get_spirv()))
	return OK


func initialized_ok() -> bool:
	return _initialized


func pending() -> bool:
	return _readback_pending


func request_volume() -> int:
	if not _initialized or _readback_pending:
		return -1
	var request_id := _next_request_id
	_next_request_id += 1
	_readback_pending = true
	RenderingServer.call_on_render_thread(
		Callable(self, &"_request_render_thread").bind(request_id))
	return request_id


## Render-thread only.
func _init_render_thread(reduce_spirv: RDShaderSPIRV, final_spirv: RDShaderSPIRV) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null:
		call_deferred("_finish_init", ERR_UNAVAILABLE, {})
		return
	var resources: Array = []
	var reduce_shader := rd.shader_create_from_spirv(reduce_spirv)
	if not reduce_shader.is_valid():
		call_deferred("_finish_init", ERR_CANT_CREATE, {})
		return
	resources.append(reduce_shader)
	var final_shader := rd.shader_create_from_spirv(final_spirv)
	if not final_shader.is_valid():
		_free_many(rd, resources)
		call_deferred("_finish_init", ERR_CANT_CREATE, {})
		return
	resources.append(final_shader)

	var reduce_pipeline := rd.compute_pipeline_create(reduce_shader)
	if not reduce_pipeline.is_valid():
		_free_many(rd, resources)
		call_deferred("_finish_init", ERR_CANT_CREATE, {})
		return
	resources.append(reduce_pipeline)
	var final_pipeline := rd.compute_pipeline_create(final_shader)
	if not final_pipeline.is_valid():
		_free_many(rd, resources)
		call_deferred("_finish_init", ERR_CANT_CREATE, {})
		return
	resources.append(final_pipeline)

	var partial_bytes := PackedByteArray()
	partial_bytes.resize(_partial_count * 4)
	var result_bytes := PackedByteArray()
	result_bytes.resize(4)
	var params_values := PackedFloat32Array([
		float(_width), float(_height), _dx, float(_partial_count),
	])
	var partials := rd.storage_buffer_create(partial_bytes.size(), partial_bytes)
	if not partials.is_valid():
		_free_many(rd, resources)
		call_deferred("_finish_init", ERR_CANT_CREATE, {})
		return
	resources.append(partials)
	var result := rd.storage_buffer_create(result_bytes.size(), result_bytes)
	if not result.is_valid():
		_free_many(rd, resources)
		call_deferred("_finish_init", ERR_CANT_CREATE, {})
		return
	resources.append(result)
	var params := rd.storage_buffer_create(16, params_values.to_byte_array())
	if not params.is_valid():
		_free_many(rd, resources)
		call_deferred("_finish_init", ERR_CANT_CREATE, {})
		return
	resources.append(params)

	var reduce_set := rd.uniform_set_create([
		_storage_uniform(0, _state), _storage_uniform(1, partials),
		_storage_uniform(2, params),
	], reduce_shader, 0)
	if not reduce_set.is_valid():
		_free_many(rd, resources)
		call_deferred("_finish_init", ERR_CANT_CREATE, {})
		return
	resources.append(reduce_set)
	var final_set := rd.uniform_set_create([
		_storage_uniform(0, partials), _storage_uniform(1, result),
		_storage_uniform(2, params),
	], final_shader, 0)
	if not final_set.is_valid():
		_free_many(rd, resources)
		call_deferred("_finish_init", ERR_CANT_CREATE, {})
		return

	call_deferred("_finish_init", OK, {
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
		initialization_failed.emit(error)
		return
	_reduce_shader = bundle.reduce_shader
	_reduce_pipeline = bundle.reduce_pipeline
	_final_shader = bundle.final_shader
	_final_pipeline = bundle.final_pipeline
	_partials = bundle.partials
	_result = bundle.result
	_params = bundle.params
	_reduce_set = bundle.reduce_set
	_final_set = bundle.final_set
	_initialized = true
	initialized.emit()


## Render-thread only.
func _request_render_thread(request_id: int) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null or not _all_valid():
		call_deferred("_fail_readback", request_id, ERR_UNAVAILABLE)
		return
	var compute := rd.compute_list_begin()
	rd.compute_list_bind_compute_pipeline(compute, _reduce_pipeline)
	rd.compute_list_bind_uniform_set(compute, _reduce_set, 0)
	rd.compute_list_dispatch(compute, _groups_x, _groups_y, 1)
	rd.compute_list_add_barrier(compute)
	rd.compute_list_bind_compute_pipeline(compute, _final_pipeline)
	rd.compute_list_bind_uniform_set(compute, _final_set, 0)
	rd.compute_list_dispatch(compute, 1, 1, 1)
	rd.compute_list_end()

	var callback := Callable(self, &"_on_volume_bytes").bind(request_id)
	var err := rd.buffer_get_data_async(_result, callback, 0, 4)
	if err != OK:
		call_deferred("_fail_readback", request_id, err)


func _on_volume_bytes(bytes: PackedByteArray, request_id: int) -> void:
	if bytes.size() < 4:
		call_deferred("_fail_readback", request_id, ERR_INVALID_DATA)
		return
	call_deferred("_publish_volume", request_id, bytes.decode_float(0))


func _publish_volume(request_id: int, volume_m3: float) -> void:
	_readback_pending = false
	volume_ready.emit(request_id, volume_m3)


func _fail_readback(request_id: int, error: Error) -> void:
	_readback_pending = false
	readback_failed.emit(request_id, error)


func _all_valid() -> bool:
	return _state.is_valid() and _reduce_pipeline.is_valid() and _final_pipeline.is_valid() \
		and _partials.is_valid() and _result.is_valid() and _params.is_valid() \
		and _reduce_set.is_valid() and _final_set.is_valid()


func _storage_uniform(binding: int, buffer: RID) -> RDUniform:
	var u := RDUniform.new()
	u.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
	u.binding = binding
	u.add_id(buffer)
	return u


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
	_readback_pending = false
	_reduce_set = RID(); _final_set = RID()
	_partials = RID(); _result = RID(); _params = RID()
	_reduce_pipeline = RID(); _final_pipeline = RID()
	_reduce_shader = RID(); _final_shader = RID()
	RenderingServer.call_on_render_thread(Callable(self, &"_release_render_thread").bind(rids))
	released.emit()


## Render-thread only.
func _release_render_thread(rids: Array) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd != null:
		_free_many(rd, rids)


func _exit_tree() -> void:
	release()
