class_name HydroFrontierCandidatesGPU
extends Node
## GPU-generated compact active-frontier queue.
##
## Reads HydroTileActivityGPU summaries and SparseHydroAtlasGPU metadata. Every
## candidate snapshots stable (face, level, x, y) identity beside the transient
## source slot, allowing delayed readbacks to detect slot recycling safely.

signal initialized
signal initialization_failed(error: Error)
signal queue_recorded(request_id: int)
signal candidates_ready(request_id: int, candidates: Array[Dictionary], overflow: bool)
signal queue_failed(request_id: int, error: Error)
signal released

const HEADER_BYTES := 16
const ENTRY_BYTES := 32
const LOCAL_X := 64

var _summary := RID()
var _metadata := RID()
var _shader := RID()
var _pipeline := RID()
var _queue := RID()
var _params := RID()
var _uniform_set := RID()
var _capacity := 0
var _max_candidates := 0
var _threshold_m3s := 0.01
var _initialized := false
var _init_pending := false
var _dispatch_pending := false
var _readback_pending := false
var _next_request_id := 1


func initialize(summary_rid: RID, metadata_rid: RID, capacity: int,
		threshold_m3s: float = 0.01) -> Error:
	if _init_pending or _initialized or _dispatch_pending or _readback_pending:
		return ERR_BUSY
	if not summary_rid.is_valid() or not metadata_rid.is_valid() or capacity <= 0:
		return ERR_INVALID_PARAMETER
	if RenderingServer.get_rendering_device() == null:
		return ERR_UNAVAILABLE
	var shader_file: RDShaderFile = load("res://shaders/water/hydro_frontier_candidates.glsl")
	if shader_file == null:
		return ERR_CANT_OPEN
	var spirv := shader_file.get_spirv()
	if spirv == null:
		return ERR_CANT_CREATE

	_summary = summary_rid
	_metadata = metadata_rid
	_capacity = capacity
	_max_candidates = capacity * 4
	_threshold_m3s = maxf(threshold_m3s, 0.0)
	_init_pending = true
	RenderingServer.call_on_render_thread(
		Callable(self, &"_init_render_thread").bind(spirv))
	return OK


func initialized_ok() -> bool:
	return _initialized


func pending() -> bool:
	return _dispatch_pending or _readback_pending


func queue_rid() -> RID:
	return _queue if _initialized else RID()


func generate(request_readback: bool = false) -> int:
	if not _initialized or _dispatch_pending or _readback_pending:
		return -1
	var request_id := _next_request_id
	_next_request_id += 1
	_dispatch_pending = true
	_readback_pending = request_readback
	RenderingServer.call_on_render_thread(
		Callable(self, &"_generate_render_thread").bind(request_id, request_readback))
	return request_id


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

	var queue_bytes := PackedByteArray()
	queue_bytes.resize(HEADER_BYTES + _max_candidates * ENTRY_BYTES)
	var params_values := PackedFloat32Array([
		float(_capacity), _threshold_m3s, float(_max_candidates), 0.0,
	])
	var queue := rd.storage_buffer_create(queue_bytes.size(), queue_bytes)
	var params := rd.storage_buffer_create(16, params_values.to_byte_array())
	if not queue.is_valid() or not params.is_valid():
		_free_many(rd, [queue, params, pipeline, shader])
		call_deferred("_finish_init", ERR_CANT_CREATE, {})
		return
	var set_rid := rd.uniform_set_create([
		_storage_uniform(0, _summary),
		_storage_uniform(1, _metadata),
		_storage_uniform(2, queue),
		_storage_uniform(3, params),
	], shader, 0)
	if not set_rid.is_valid():
		_free_many(rd, [queue, params, pipeline, shader])
		call_deferred("_finish_init", ERR_CANT_CREATE, {})
		return
	call_deferred("_finish_init", OK, {
		"shader": shader, "pipeline": pipeline, "queue": queue,
		"params": params, "set": set_rid,
	})


func _finish_init(error: Error, bundle: Dictionary) -> void:
	_init_pending = false
	if error != OK:
		initialization_failed.emit(error)
		return
	_shader = bundle["shader"]
	_pipeline = bundle["pipeline"]
	_queue = bundle["queue"]
	_params = bundle["params"]
	_uniform_set = bundle["set"]
	_initialized = true
	initialized.emit()


func _generate_render_thread(request_id: int, request_readback: bool) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null or not _pipeline.is_valid() or not _uniform_set.is_valid():
		call_deferred("_finish_generate", request_id, ERR_UNAVAILABLE)
		return
	var clear_error := rd.buffer_clear(_queue, 0, HEADER_BYTES)
	if clear_error != OK:
		call_deferred("_finish_generate", request_id, clear_error)
		return
	var compute := rd.compute_list_begin()
	rd.compute_list_bind_compute_pipeline(compute, _pipeline)
	rd.compute_list_bind_uniform_set(compute, _uniform_set, 0)
	rd.compute_list_dispatch(compute,
		int(ceil(float(_capacity) / float(LOCAL_X))), 1, 1)
	rd.compute_list_end()

	if request_readback:
		var callback := Callable(self, &"_on_queue_bytes").bind(request_id)
		var err := rd.buffer_get_data_async(_queue, callback,
			0, HEADER_BYTES + _max_candidates * ENTRY_BYTES)
		if err != OK:
			call_deferred("_finish_generate", request_id, err)
			return
	call_deferred("_finish_generate", request_id, OK)


func _finish_generate(request_id: int, error: Error) -> void:
	_dispatch_pending = false
	if error != OK:
		_readback_pending = false
		queue_failed.emit(request_id, error)
		return
	queue_recorded.emit(request_id)


func _on_queue_bytes(bytes: PackedByteArray, request_id: int) -> void:
	if bytes.size() < HEADER_BYTES:
		call_deferred("_publish_bad_readback", request_id)
		return
	var raw_count := int(bytes.decode_u32(0))
	var overflow := bytes.decode_u32(4) != 0
	var count := mini(raw_count, _max_candidates)
	var candidates: Array[Dictionary] = []
	for i in count:
		var o := HEADER_BYTES + i * ENTRY_BYTES
		if o + ENTRY_BYTES > bytes.size():
			overflow = true
			break
		candidates.append({
			"slot": int(bytes.decode_u32(o + 0)),
			"direction": int(bytes.decode_u32(o + 4)),
			"flux_m3s": bytes.decode_float(o + 8),
			"face": int(bytes.decode_u32(o + 12)),
			"level": int(bytes.decode_u32(o + 16)),
			"x": int(bytes.decode_u32(o + 20)),
			"y": int(bytes.decode_u32(o + 24)),
		})
	call_deferred("_publish_candidates", request_id, candidates, overflow)


func _publish_candidates(request_id: int, candidates: Array[Dictionary], overflow: bool) -> void:
	_readback_pending = false
	candidates_ready.emit(request_id, candidates, overflow)


func _publish_bad_readback(request_id: int) -> void:
	_readback_pending = false
	queue_failed.emit(request_id, ERR_INVALID_DATA)


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
	var rids := [_uniform_set, _queue, _params, _pipeline, _shader]
	_initialized = false
	_dispatch_pending = false
	_readback_pending = false
	_uniform_set = RID(); _queue = RID(); _params = RID()
	_pipeline = RID(); _shader = RID()
	_summary = RID(); _metadata = RID()
	RenderingServer.call_on_render_thread(
		Callable(self, &"_release_render_thread").bind(rids))
	released.emit()


func _release_render_thread(rids: Array) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd != null:
		_free_many(rd, rids)


func _exit_tree() -> void:
	release()
