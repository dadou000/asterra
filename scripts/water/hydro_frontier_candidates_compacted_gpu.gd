class_name HydroFrontierCandidatesCompactedGPU
extends HydroFrontierCandidatesGPU
## Temporal-production frontier generator over HydroTileActivityCompactedGPU.
##
## Dispatch count comes from the activity compactor's GPU indirect command, so only
## active activity records are visited. Candidate readback is header-first and then
## exact-payload, removing the old capacity*4 candidate transfer every cycle.

var _activity_compact := RID()
var _activity_indirect := RID()
var _compact_request_id := -1
var _last_candidate_count := 0


func initialize_compacted(activity_compact_rid: RID, activity_indirect_rid: RID,
		metadata_rid: RID, capacity: int, threshold_m3s: float = 0.01,
		state_rid: RID = RID(), tile_resolution: int = 0) -> Error:
	if _init_pending or _initialized or _dispatch_pending or _readback_pending:
		return ERR_BUSY
	if not activity_compact_rid.is_valid() or not activity_indirect_rid.is_valid() \
			or not metadata_rid.is_valid() or capacity <= 0:
		return ERR_INVALID_PARAMETER
	if RenderingServer.get_rendering_device() == null:
		return ERR_UNAVAILABLE
	var shader_file: RDShaderFile = load(
		"res://shaders/water/hydro_frontier_candidates_compact.glsl")
	if shader_file == null:
		return ERR_CANT_OPEN
	var spirv := shader_file.get_spirv()
	if spirv == null:
		return ERR_CANT_CREATE

	_activity_compact = activity_compact_rid
	_activity_indirect = activity_indirect_rid
	_summary = activity_compact_rid
	_metadata = metadata_rid
	_state = state_rid if state_rid.is_valid() else activity_compact_rid
	_capacity = capacity
	_max_candidates = capacity * 4
	_threshold_m3s = maxf(threshold_m3s, 0.0)
	_tile_resolution = maxi(tile_resolution, 0) if state_rid.is_valid() else 0
	_init_pending = true
	RenderingServer.call_on_render_thread(
		Callable(self, &"_init_compacted_frontier_render_thread").bind(spirv))
	return OK


func generate(request_readback: bool = false) -> int:
	if not _initialized or _dispatch_pending or _readback_pending \
			or not _activity_indirect.is_valid():
		return -1
	var request_id := _next_request_id
	_next_request_id += 1
	_compact_request_id = request_id
	_dispatch_pending = true
	_readback_pending = request_readback
	RenderingServer.call_on_render_thread(
		Callable(self, &"_generate_compacted_render_thread").bind(
			request_id, request_readback))
	return request_id


func compacted_stats() -> Dictionary:
	return {
		"initialized": _initialized,
		"active_record_indirect_dispatch": true,
		"full_capacity_summary_scan": false,
		"header_first_exact_candidate_readback": true,
		"full_capacity_candidate_readback": false,
		"last_candidate_count": _last_candidate_count,
		"max_candidates": _max_candidates,
	}


func _init_compacted_frontier_render_thread(spirv: RDShaderSPIRV) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null:
		call_deferred(&"_finish_init", ERR_UNAVAILABLE, {})
		return
	var shader := rd.shader_create_from_spirv(spirv)
	if not shader.is_valid():
		call_deferred(&"_finish_init", ERR_CANT_CREATE, {})
		return
	var pipeline := rd.compute_pipeline_create(shader)
	if not pipeline.is_valid():
		rd.free_rid(shader)
		call_deferred(&"_finish_init", ERR_CANT_CREATE, {})
		return

	var queue_bytes := PackedByteArray()
	queue_bytes.resize(HEADER_BYTES + _max_candidates * ENTRY_BYTES)
	var params_values := PackedFloat32Array([
		float(_capacity), _threshold_m3s, float(_max_candidates), float(_tile_resolution),
	])
	var queue := rd.storage_buffer_create(queue_bytes.size(), queue_bytes)
	var params := rd.storage_buffer_create(16, params_values.to_byte_array())
	if not queue.is_valid() or not params.is_valid():
		_free_many(rd, [queue, params, pipeline, shader])
		call_deferred(&"_finish_init", ERR_CANT_CREATE, {})
		return
	var set_rid := rd.uniform_set_create([
		_storage_uniform(0, _activity_compact),
		_storage_uniform(1, _metadata),
		_storage_uniform(2, queue),
		_storage_uniform(3, params),
		_storage_uniform(4, _state),
	], shader, 0)
	if not set_rid.is_valid():
		_free_many(rd, [queue, params, pipeline, shader])
		call_deferred(&"_finish_init", ERR_CANT_CREATE, {})
		return
	call_deferred(&"_finish_init", OK, {
		"shader": shader, "pipeline": pipeline, "queue": queue,
		"params": params, "set": set_rid,
	})


func _generate_compacted_render_thread(request_id: int,
		request_readback: bool) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null or not _pipeline.is_valid() or not _uniform_set.is_valid() \
			or not _activity_indirect.is_valid():
		call_deferred(&"_finish_generate", request_id, ERR_UNAVAILABLE)
		return
	var clear_error := rd.buffer_clear(_queue, 0, HEADER_BYTES)
	if clear_error != OK:
		call_deferred(&"_finish_generate", request_id, clear_error)
		return
	var compute := rd.compute_list_begin()
	rd.compute_list_bind_compute_pipeline(compute, _pipeline)
	rd.compute_list_bind_uniform_set(compute, _uniform_set, 0)
	rd.compute_list_dispatch_indirect(compute, _activity_indirect, 0)
	rd.compute_list_end()

	if request_readback:
		var callback := Callable(self, &"_on_compacted_queue_header").bind(request_id)
		var err := rd.buffer_get_data_async(_queue, callback, 0, HEADER_BYTES)
		if err != OK:
			call_deferred(&"_finish_generate", request_id, err)
			return
	call_deferred(&"_finish_generate", request_id, OK)


func _on_compacted_queue_header(bytes: PackedByteArray, request_id: int) -> void:
	if request_id != _compact_request_id or not _readback_pending:
		return
	if bytes.size() < HEADER_BYTES:
		call_deferred(&"_publish_bad_readback", request_id)
		return
	var raw_count := int(bytes.decode_u32(0))
	var overflow := bytes.decode_u32(4) != 0
	var count := mini(raw_count, _max_candidates)
	_last_candidate_count = count
	if count <= 0:
		call_deferred(&"_publish_candidates", request_id, [], overflow)
		return
	RenderingServer.call_on_render_thread(
		Callable(self, &"_read_compacted_queue_entries_render_thread").bind(
			request_id, count, overflow))


func _read_compacted_queue_entries_render_thread(request_id: int, count: int,
		overflow: bool) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null or not _queue.is_valid():
		call_deferred(&"_publish_bad_readback", request_id)
		return
	var callback := Callable(self, &"_on_compacted_queue_entries").bind(
		request_id, count, overflow)
	var err := rd.buffer_get_data_async(
		_queue, callback, HEADER_BYTES, count * ENTRY_BYTES)
	if err != OK:
		call_deferred(&"_publish_bad_readback", request_id)


func _on_compacted_queue_entries(bytes: PackedByteArray, request_id: int,
		count: int, overflow: bool) -> void:
	if request_id != _compact_request_id or not _readback_pending:
		return
	if bytes.size() < count * ENTRY_BYTES:
		call_deferred(&"_publish_bad_readback", request_id)
		return
	var candidates: Array[Dictionary] = []
	for i in count:
		var o := i * ENTRY_BYTES
		var flags := int(bytes.decode_u32(o + 28))
		candidates.append({
			"slot": int(bytes.decode_u32(o + 0)),
			"direction": int(bytes.decode_u32(o + 4)),
			"flux_m3s": bytes.decode_float(o + 8),
			"face": int(bytes.decode_u32(o + 12)),
			"level": int(bytes.decode_u32(o + 16)),
			"x": int(bytes.decode_u32(o + 20)),
			"y": int(bytes.decode_u32(o + 24)),
			"predictive_wetting": (flags & 1) != 0,
			"source_surface_m": bytes.decode_float(o + 32),
		})
	call_deferred(&"_publish_candidates", request_id, candidates, overflow)


func release() -> void:
	_activity_compact = RID()
	_activity_indirect = RID()
	_compact_request_id = -1
	_last_candidate_count = 0
	super.release()


func _exit_tree() -> void:
	release()
