class_name HydroTileActivityCompactedGPU
extends HydroTileActivityCachedGPU
## Temporal-production activity facade with GPU active-record compaction.
##
## The fused HydroLODCFLCacheGPU still owns the canonical 80-byte-per-slot activity
## buffer. This facade scans only that compact summary buffer (never water cells),
## appends active slots into a dense 84-byte record queue, and reads back only the
## exact active payload after a 16-byte header read. The same dense queue/indirect
## command is exposed to compact frontier generation.

const COMPACT_HEADER_BYTES := 16
const COMPACT_ENTRY_WORDS := 21
const COMPACT_ENTRY_BYTES := COMPACT_ENTRY_WORDS * 4
const COMPACT_INDIRECT_BYTES := 16
const COMPACT_LOCAL_X := 64

var _compact_shader := RID()
var _compact_pipeline := RID()
var _finalize_shader := RID()
var _finalize_pipeline := RID()
var _compact_queue := RID()
var _compact_indirect := RID()
var _compact_params := RID()
var _compact_set := RID()
var _finalize_set := RID()
var _compact_init_pending := false
var _compact_request_id := -1
var _last_compact_count := 0


func initialize_compacted(summary_rid: RID, capacity: int) -> Error:
	if _initialized or _init_pending or _dispatch_pending or _readback_pending \
			or _compact_init_pending:
		return ERR_BUSY
	if not summary_rid.is_valid() or capacity <= 0:
		return ERR_INVALID_PARAMETER
	if RenderingServer.get_rendering_device() == null:
		return ERR_UNAVAILABLE
	var compact_file: RDShaderFile = load(
		"res://shaders/water/hydro_activity_compact.glsl")
	var finalize_file: RDShaderFile = load(
		"res://shaders/water/hydro_activity_compact_finalize.glsl")
	if compact_file == null or finalize_file == null:
		return ERR_CANT_OPEN
	var compact_spirv := compact_file.get_spirv()
	var finalize_spirv := finalize_file.get_spirv()
	if compact_spirv == null or finalize_spirv == null:
		return ERR_CANT_CREATE

	_summary = summary_rid
	_capacity = capacity
	_external_summary = true
	_compact_init_pending = true
	RenderingServer.call_on_render_thread(
		Callable(self, &"_init_compacted_render_thread").bind(
			compact_spirv, finalize_spirv))
	return OK


func compact_queue_rid() -> RID:
	return _compact_queue if _initialized else RID()


func compact_indirect_rid() -> RID:
	return _compact_indirect if _initialized else RID()


func classify(request_readback: bool = false) -> int:
	if not _initialized or not _external_summary or _dispatch_pending \
			or _readback_pending or not _summary.is_valid() \
			or not _compact_queue.is_valid() or not _compact_indirect.is_valid():
		return -1
	var request_id := _next_request_id
	_next_request_id += 1
	_compact_request_id = request_id
	_dispatch_pending = true
	_readback_pending = request_readback
	RenderingServer.call_on_render_thread(
		Callable(self, &"_compact_render_thread").bind(request_id, request_readback))
	return request_id


func compacted_stats() -> Dictionary:
	return {
		"initialized": _initialized,
		"external_summary_buffer": _external_summary,
		"cell_compute_dispatch": false,
		"summary_slot_compaction_dispatch": true,
		"header_first_exact_payload_readback": true,
		"full_capacity_activity_readback": false,
		"compact_entry_bytes": COMPACT_ENTRY_BYTES,
		"compact_capacity": _capacity,
		"last_compact_count": _last_compact_count,
		"gpu_compact_queue_bytes": COMPACT_HEADER_BYTES + _capacity * COMPACT_ENTRY_BYTES,
	}


func _init_compacted_render_thread(compact_spirv: RDShaderSPIRV,
		finalize_spirv: RDShaderSPIRV) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null:
		call_deferred(&"_finish_compacted_init", ERR_UNAVAILABLE, {})
		return
	var compact_shader := rd.shader_create_from_spirv(compact_spirv)
	var finalize_shader := rd.shader_create_from_spirv(finalize_spirv)
	if not compact_shader.is_valid() or not finalize_shader.is_valid():
		_free_many(rd, [finalize_shader, compact_shader])
		call_deferred(&"_finish_compacted_init", ERR_CANT_CREATE, {})
		return
	var compact_pipeline := rd.compute_pipeline_create(compact_shader)
	var finalize_pipeline := rd.compute_pipeline_create(finalize_shader)
	if not compact_pipeline.is_valid() or not finalize_pipeline.is_valid():
		_free_many(rd, [finalize_pipeline, compact_pipeline,
			finalize_shader, compact_shader])
		call_deferred(&"_finish_compacted_init", ERR_CANT_CREATE, {})
		return

	var queue_bytes := PackedByteArray()
	queue_bytes.resize(COMPACT_HEADER_BYTES + _capacity * COMPACT_ENTRY_BYTES)
	var indirect_bytes := PackedByteArray()
	indirect_bytes.resize(COMPACT_INDIRECT_BYTES)
	var params_bytes := PackedInt32Array([_capacity, _capacity, 0, 0]).to_byte_array()
	var compact_queue := rd.storage_buffer_create(queue_bytes.size(), queue_bytes)
	var compact_indirect := rd.storage_buffer_create(COMPACT_INDIRECT_BYTES,
		indirect_bytes, RenderingDevice.STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT)
	var compact_params := rd.storage_buffer_create(16, params_bytes)
	if not compact_queue.is_valid() or not compact_indirect.is_valid() \
			or not compact_params.is_valid():
		_free_many(rd, [compact_params, compact_indirect, compact_queue,
			finalize_pipeline, compact_pipeline, finalize_shader, compact_shader])
		call_deferred(&"_finish_compacted_init", ERR_CANT_CREATE, {})
		return

	var compact_set := rd.uniform_set_create([
		_storage_uniform(0, _summary),
		_storage_uniform(1, compact_queue),
		_storage_uniform(2, compact_params),
	], compact_shader, 0)
	var finalize_set := rd.uniform_set_create([
		_storage_uniform(0, compact_queue),
		_storage_uniform(1, compact_indirect),
		_storage_uniform(2, compact_params),
	], finalize_shader, 0)
	if not compact_set.is_valid() or not finalize_set.is_valid():
		_free_many(rd, [finalize_set, compact_set, compact_params,
			compact_indirect, compact_queue, finalize_pipeline, compact_pipeline,
			finalize_shader, compact_shader])
		call_deferred(&"_finish_compacted_init", ERR_CANT_CREATE, {})
		return

	call_deferred(&"_finish_compacted_init", OK, {
		"compact_shader": compact_shader,
		"compact_pipeline": compact_pipeline,
		"finalize_shader": finalize_shader,
		"finalize_pipeline": finalize_pipeline,
		"compact_queue": compact_queue,
		"compact_indirect": compact_indirect,
		"compact_params": compact_params,
		"compact_set": compact_set,
		"finalize_set": finalize_set,
	})


func _finish_compacted_init(error: Error, bundle: Dictionary) -> void:
	_compact_init_pending = false
	if error != OK:
		_external_summary = false
		_summary = RID()
		_capacity = 0
		initialization_failed.emit(error)
		return
	_compact_shader = bundle["compact_shader"]
	_compact_pipeline = bundle["compact_pipeline"]
	_finalize_shader = bundle["finalize_shader"]
	_finalize_pipeline = bundle["finalize_pipeline"]
	_compact_queue = bundle["compact_queue"]
	_compact_indirect = bundle["compact_indirect"]
	_compact_params = bundle["compact_params"]
	_compact_set = bundle["compact_set"]
	_finalize_set = bundle["finalize_set"]
	_initialized = true
	initialized.emit()


func _compact_render_thread(request_id: int, request_readback: bool) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null or not _compact_pipeline.is_valid() or not _compact_set.is_valid() \
			or not _finalize_pipeline.is_valid() or not _finalize_set.is_valid():
		call_deferred(&"_finish_classification", request_id, ERR_UNAVAILABLE)
		return
	var clear_error := rd.buffer_clear(_compact_queue, 0, COMPACT_HEADER_BYTES)
	if clear_error != OK:
		call_deferred(&"_finish_classification", request_id, clear_error)
		return

	var compute := rd.compute_list_begin()
	rd.compute_list_bind_compute_pipeline(compute, _compact_pipeline)
	rd.compute_list_bind_uniform_set(compute, _compact_set, 0)
	rd.compute_list_dispatch(compute,
		maxi(int(ceil(float(_capacity) / float(COMPACT_LOCAL_X))), 1), 1, 1)
	rd.compute_list_add_barrier(compute)
	rd.compute_list_bind_compute_pipeline(compute, _finalize_pipeline)
	rd.compute_list_bind_uniform_set(compute, _finalize_set, 0)
	rd.compute_list_dispatch(compute, 1, 1, 1)
	rd.compute_list_end()

	if request_readback:
		var callback := Callable(self, &"_on_compact_header").bind(request_id)
		var err := rd.buffer_get_data_async(
			_compact_queue, callback, 0, COMPACT_HEADER_BYTES)
		if err != OK:
			call_deferred(&"_finish_classification", request_id, err)
			return
	call_deferred(&"_finish_classification", request_id, OK)


func _on_compact_header(bytes: PackedByteArray, request_id: int) -> void:
	if request_id != _compact_request_id or not _readback_pending:
		return
	if bytes.size() < COMPACT_HEADER_BYTES:
		call_deferred(&"_publish_bad_readback", request_id)
		return
	var raw_count := int(bytes.decode_u32(0))
	var overflow := bytes.decode_u32(4) != 0
	var count := mini(raw_count, _capacity)
	_last_compact_count = count
	if overflow or raw_count > _capacity:
		call_deferred(&"_publish_bad_readback", request_id)
		return
	if count <= 0:
		call_deferred(&"_publish_summaries", request_id, [])
		return
	RenderingServer.call_on_render_thread(
		Callable(self, &"_read_compact_entries_render_thread").bind(request_id, count))


func _read_compact_entries_render_thread(request_id: int, count: int) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null or not _compact_queue.is_valid():
		call_deferred(&"_publish_bad_readback", request_id)
		return
	var callback := Callable(self, &"_on_compact_entries").bind(request_id, count)
	var err := rd.buffer_get_data_async(_compact_queue, callback,
		COMPACT_HEADER_BYTES, count * COMPACT_ENTRY_BYTES)
	if err != OK:
		call_deferred(&"_publish_bad_readback", request_id)


func _on_compact_entries(bytes: PackedByteArray, request_id: int, count: int) -> void:
	if request_id != _compact_request_id or not _readback_pending:
		return
	if bytes.size() < count * COMPACT_ENTRY_BYTES:
		call_deferred(&"_publish_bad_readback", request_id)
		return
	var out: Array[Dictionary] = []
	for i in count:
		var o := i * COMPACT_ENTRY_BYTES
		var s := o + 4
		out.append({
			"slot": int(bytes.decode_u32(o + 0)),
			"max_depth_m": bytes.decode_float(s + 0),
			"max_velocity_mps": bytes.decode_float(s + 4),
			"kinetic_energy_proxy": bytes.decode_float(s + 8),
			"invalid_cells": int(round(bytes.decode_float(s + 12))),
			"flux_west_m3s": bytes.decode_float(s + 16),
			"flux_east_m3s": bytes.decode_float(s + 20),
			"flux_south_m3s": bytes.decode_float(s + 24),
			"flux_north_m3s": bytes.decode_float(s + 28),
			"wet_cells": int(round(bytes.decode_float(s + 32))),
			"active": bytes.decode_float(s + 36) > 0.5,
			"wetting_west_m3s": bytes.decode_float(s + 48),
			"wetting_east_m3s": bytes.decode_float(s + 52),
			"wetting_south_m3s": bytes.decode_float(s + 56),
			"wetting_north_m3s": bytes.decode_float(s + 60),
			"surface_west_m": bytes.decode_float(s + 64),
			"surface_east_m": bytes.decode_float(s + 68),
			"surface_south_m": bytes.decode_float(s + 72),
			"surface_north_m": bytes.decode_float(s + 76),
		})
	call_deferred(&"_publish_summaries", request_id, out)


func release() -> void:
	if not _initialized and not _compact_shader.is_valid() and not _external_summary:
		return
	var rids := [_finalize_set, _compact_set, _compact_params, _compact_indirect,
		_compact_queue, _finalize_pipeline, _compact_pipeline,
		_finalize_shader, _compact_shader]
	_initialized = false
	_init_pending = false
	_compact_init_pending = false
	_dispatch_pending = false
	_readback_pending = false
	_external_summary = false
	_compact_request_id = -1
	_last_compact_count = 0
	_summary = RID(); _capacity = 0
	_finalize_set = RID(); _compact_set = RID(); _compact_params = RID()
	_compact_indirect = RID(); _compact_queue = RID()
	_finalize_pipeline = RID(); _compact_pipeline = RID()
	_finalize_shader = RID(); _compact_shader = RID()
	_state = RID(); _occupancy = RID(); _metadata = RID(); _fallback_metadata = RID()
	_shader = RID(); _pipeline = RID(); _params = RID(); _uniform_set = RID()
	RenderingServer.call_on_render_thread(
		Callable(self, &"_release_compacted_render_thread").bind(rids))
	released.emit()


func _release_compacted_render_thread(rids: Array) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd != null:
		_free_many(rd, rids)


func _exit_tree() -> void:
	release()
