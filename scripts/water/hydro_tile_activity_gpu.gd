class_name HydroTileActivityGPU
extends Node
## Compact GPU summary pass for SparseHydroAtlasGPU.
##
## Physical boundary Q, predictive wetting Q and kinetic activity use each slot's
## quadtree-derived cell metric when tile metadata is supplied. The optional metadata
## arguments preserve the old single-resolution initialization contract for isolated
## callers while production binds the atlas metadata directly.

signal initialized
signal initialization_failed(error: Error)
signal classification_recorded(request_id: int)
signal summaries_ready(request_id: int, summaries: Array[Dictionary])
signal classification_failed(request_id: int, error: Error)
signal released

const SUMMARY_VEC4S := 5
const SUMMARY_BYTES_PER_TILE := SUMMARY_VEC4S * 16
const PARAM_BYTES := 32

var _state := RID()
var _occupancy := RID()
var _metadata := RID()
var _fallback_metadata := RID()
var _shader := RID()
var _pipeline := RID()
var _summary := RID()
var _params := RID()
var _uniform_set := RID()
var _capacity := 0
var _tile_resolution := 0
var _cell_size_m := 1.0
var _base_tile_level := -1
var _dry_eps := 1.0e-5
var _gravity := 9.81
var _initialized := false
var _init_pending := false
var _dispatch_pending := false
var _readback_pending := false
var _next_request_id := 1


func initialize(state_rid: RID, occupancy_rid: RID, capacity: int,
		tile_resolution: int, cell_size_m: float, dry_eps: float = 1.0e-5,
		gravity: float = 9.81, metadata_rid: RID = RID(),
		base_tile_level: int = -1) -> Error:
	if _init_pending or _dispatch_pending or _readback_pending or _initialized:
		return ERR_BUSY
	if not state_rid.is_valid() or not occupancy_rid.is_valid() \
			or capacity <= 0 or tile_resolution <= 0 or cell_size_m <= 0.0:
		return ERR_INVALID_PARAMETER
	if RenderingServer.get_rendering_device() == null:
		return ERR_UNAVAILABLE
	var shader_file: RDShaderFile = load("res://shaders/water/hydro_tile_activity.glsl")
	if shader_file == null:
		return ERR_CANT_OPEN
	var spirv := shader_file.get_spirv()
	if spirv == null:
		return ERR_CANT_CREATE

	_state = state_rid
	_occupancy = occupancy_rid
	_metadata = metadata_rid
	_capacity = capacity
	_tile_resolution = tile_resolution
	_cell_size_m = cell_size_m
	_base_tile_level = base_tile_level if metadata_rid.is_valid() else -1
	_dry_eps = maxf(dry_eps, 1.0e-8)
	_gravity = maxf(gravity, 1.0e-4)
	_init_pending = true
	RenderingServer.call_on_render_thread(
		Callable(self, &"_init_render_thread").bind(spirv))
	return OK


func initialized_ok() -> bool:
	return _initialized


func pending() -> bool:
	return _dispatch_pending or _readback_pending


func summary_rid() -> RID:
	return _summary if _initialized else RID()


func classify(request_readback: bool = false) -> int:
	if not _initialized or _dispatch_pending or _readback_pending:
		return -1
	var request_id := _next_request_id
	_next_request_id += 1
	_dispatch_pending = true
	_readback_pending = request_readback
	RenderingServer.call_on_render_thread(
		Callable(self, &"_classify_render_thread").bind(request_id, request_readback))
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

	var summary_bytes := PackedByteArray()
	summary_bytes.resize(_capacity * SUMMARY_BYTES_PER_TILE)
	var lod_enabled := 1.0 if _metadata.is_valid() and _base_tile_level >= 0 else 0.0
	var param_values := PackedFloat32Array([
		float(_tile_resolution), float(_capacity), _cell_size_m, _dry_eps,
		_gravity, float(maxi(_base_tile_level, 0)), lod_enabled, 0.0,
	])
	var summary := rd.storage_buffer_create(summary_bytes.size(), summary_bytes)
	var params := rd.storage_buffer_create(PARAM_BYTES, param_values.to_byte_array())
	var metadata := _metadata
	var fallback := RID()
	if not metadata.is_valid():
		var fallback_bytes := PackedByteArray()
		fallback_bytes.resize(_capacity * SparseHydroAtlasGPU.METADATA_INTS * 4)
		fallback = rd.storage_buffer_create(fallback_bytes.size(), fallback_bytes)
		metadata = fallback
	if not summary.is_valid() or not params.is_valid() or not metadata.is_valid():
		_free_many(rd, [fallback, summary, params, pipeline, shader])
		call_deferred("_finish_init", ERR_CANT_CREATE, {})
		return

	var set_rid := rd.uniform_set_create([
		_storage_uniform(0, _state),
		_storage_uniform(1, _occupancy),
		_storage_uniform(2, summary),
		_storage_uniform(3, params),
		_storage_uniform(4, metadata),
	], shader, 0)
	if not set_rid.is_valid():
		_free_many(rd, [fallback, summary, params, pipeline, shader])
		call_deferred("_finish_init", ERR_CANT_CREATE, {})
		return
	call_deferred("_finish_init", OK, {
		"shader": shader, "pipeline": pipeline, "summary": summary,
		"params": params, "set": set_rid, "fallback_metadata": fallback,
	})


func _finish_init(error: Error, bundle: Dictionary) -> void:
	_init_pending = false
	if error != OK:
		initialization_failed.emit(error)
		return
	_shader = bundle["shader"]
	_pipeline = bundle["pipeline"]
	_summary = bundle["summary"]
	_params = bundle["params"]
	_uniform_set = bundle["set"]
	_fallback_metadata = bundle["fallback_metadata"]
	_initialized = true
	initialized.emit()


func _classify_render_thread(request_id: int, request_readback: bool) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null or not _pipeline.is_valid() or not _uniform_set.is_valid():
		call_deferred("_finish_classification", request_id, ERR_UNAVAILABLE)
		return
	var compute := rd.compute_list_begin()
	rd.compute_list_bind_compute_pipeline(compute, _pipeline)
	rd.compute_list_bind_uniform_set(compute, _uniform_set, 0)
	rd.compute_list_dispatch(compute, _capacity, 1, 1)
	rd.compute_list_end()

	if request_readback:
		var callback := Callable(self, &"_on_summary_bytes").bind(request_id)
		var err := rd.buffer_get_data_async(_summary, callback,
			0, _capacity * SUMMARY_BYTES_PER_TILE)
		if err != OK:
			call_deferred("_finish_classification", request_id, err)
			return
	call_deferred("_finish_classification", request_id, OK)


func _finish_classification(request_id: int, error: Error) -> void:
	_dispatch_pending = false
	if error != OK:
		_readback_pending = false
		classification_failed.emit(request_id, error)
		return
	classification_recorded.emit(request_id)


func _on_summary_bytes(bytes: PackedByteArray, request_id: int) -> void:
	if bytes.size() < _capacity * SUMMARY_BYTES_PER_TILE:
		call_deferred("_publish_bad_readback", request_id)
		return
	var out: Array[Dictionary] = []
	for slot in _capacity:
		var o := slot * SUMMARY_BYTES_PER_TILE
		out.append({
			"slot": slot,
			"max_depth_m": bytes.decode_float(o + 0),
			"max_velocity_mps": bytes.decode_float(o + 4),
			"kinetic_energy_proxy": bytes.decode_float(o + 8),
			"invalid_cells": int(round(bytes.decode_float(o + 12))),
			"flux_west_m3s": bytes.decode_float(o + 16),
			"flux_east_m3s": bytes.decode_float(o + 20),
			"flux_south_m3s": bytes.decode_float(o + 24),
			"flux_north_m3s": bytes.decode_float(o + 28),
			"wet_cells": int(round(bytes.decode_float(o + 32))),
			"active": bytes.decode_float(o + 36) > 0.5,
			"wetting_west_m3s": bytes.decode_float(o + 48),
			"wetting_east_m3s": bytes.decode_float(o + 52),
			"wetting_south_m3s": bytes.decode_float(o + 56),
			"wetting_north_m3s": bytes.decode_float(o + 60),
			"surface_west_m": bytes.decode_float(o + 64),
			"surface_east_m": bytes.decode_float(o + 68),
			"surface_south_m": bytes.decode_float(o + 72),
			"surface_north_m": bytes.decode_float(o + 76),
		})
	call_deferred("_publish_summaries", request_id, out)


func _publish_summaries(request_id: int, summaries: Array[Dictionary]) -> void:
	_readback_pending = false
	summaries_ready.emit(request_id, summaries)


func _publish_bad_readback(request_id: int) -> void:
	_readback_pending = false
	classification_failed.emit(request_id, ERR_INVALID_DATA)


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
	var rids := [_uniform_set, _summary, _params, _fallback_metadata,
		_pipeline, _shader]
	_initialized = false
	_dispatch_pending = false
	_readback_pending = false
	_uniform_set = RID(); _summary = RID(); _params = RID()
	_fallback_metadata = RID(); _pipeline = RID(); _shader = RID()
	_metadata = RID(); _state = RID(); _occupancy = RID()
	RenderingServer.call_on_render_thread(
		Callable(self, &"_release_render_thread").bind(rids))
	released.emit()


func _release_render_thread(rids: Array) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd != null:
		_free_many(rd, rids)


func _exit_tree() -> void:
	release()
