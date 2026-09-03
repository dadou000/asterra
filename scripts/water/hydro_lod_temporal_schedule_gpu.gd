class_name HydroLODTemporalScheduleGPU
extends Node
## Fine-clock temporal scheduler for physical HydroLOD subcycling.
##
## Hn advances every 2^n H0 CFL ticks. A forced synchronization on the final
## recorded tick keeps every level at the same physical time when advance() returns.

signal initialized
signal initialization_failed(error: Error)
signal released

const PARAM_BYTES := 16
const MAX_SUPPORTED_LOD := 7

var maximum_physical_lod := 4

var _prepare_shader := RID()
var _prepare_pipeline := RID()
var _commit_shader := RID()
var _commit_pipeline := RID()
var _params := RID()
var _prepare_set := RID()
var _commit_set := RID()
var _initialized := false
var _init_pending := false


func initialize(control_rid: RID, p_maximum_physical_lod: int = 4) -> Error:
	if _initialized or _init_pending:
		return ERR_BUSY
	if not control_rid.is_valid() or RenderingServer.get_rendering_device() == null:
		return ERR_INVALID_PARAMETER
	maximum_physical_lod = clampi(p_maximum_physical_lod, 0, MAX_SUPPORTED_LOD)
	var prepare_file: RDShaderFile = load(
		"res://shaders/water/hydro_lod_temporal_prepare.glsl")
	var commit_file: RDShaderFile = load(
		"res://shaders/water/hydro_lod_temporal_commit.glsl")
	if prepare_file == null or commit_file == null:
		return ERR_CANT_OPEN
	var prepare_spirv := prepare_file.get_spirv()
	var commit_spirv := commit_file.get_spirv()
	if prepare_spirv == null or commit_spirv == null:
		return ERR_CANT_CREATE
	_init_pending = true
	RenderingServer.call_on_render_thread(
		Callable(self, &"_init_render_thread").bind(
			prepare_spirv, commit_spirv, control_rid))
	return OK


func initialized_ok() -> bool:
	return _initialized


func record_prepare(rd: RenderingDevice, compute: int, iteration: int) -> void:
	if not _initialized:
		return
	rd.compute_list_bind_compute_pipeline(compute, _prepare_pipeline)
	rd.compute_list_bind_uniform_set(compute, _prepare_set, 0)
	var push := PackedInt32Array([iteration, 0, 0, 0]).to_byte_array()
	rd.compute_list_set_push_constant(compute, push, push.size())
	rd.compute_list_dispatch(compute, 1, 1, 1)
	rd.compute_list_add_barrier(compute)


func record_commit(rd: RenderingDevice, compute: int) -> void:
	if not _initialized:
		return
	rd.compute_list_bind_compute_pipeline(compute, _commit_pipeline)
	rd.compute_list_bind_uniform_set(compute, _commit_set, 0)
	rd.compute_list_dispatch(compute, 1, 1, 1)
	rd.compute_list_add_barrier(compute)


func stats() -> Dictionary:
	return {
		"initialized": _initialized,
		"maximum_physical_lod": maximum_physical_lod,
		"ratio_h0": 1,
		"ratio_h1": 2 if maximum_physical_lod >= 1 else 0,
		"ratio_h2": 4 if maximum_physical_lod >= 2 else 0,
		"ratio_h3": 8 if maximum_physical_lod >= 3 else 0,
		"ratio_h4": 16 if maximum_physical_lod >= 4 else 0,
		"forced_sync_on_advance_end": true,
		"gpu_bytes": PARAM_BYTES,
	}


func _init_render_thread(prepare_spirv: RDShaderSPIRV,
		commit_spirv: RDShaderSPIRV, control_rid: RID) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null:
		call_deferred(&"_finish_init", ERR_UNAVAILABLE, {})
		return
	var prepare_shader := rd.shader_create_from_spirv(prepare_spirv)
	var commit_shader := rd.shader_create_from_spirv(commit_spirv)
	if not prepare_shader.is_valid() or not commit_shader.is_valid():
		_free_many(rd, [commit_shader, prepare_shader])
		call_deferred(&"_finish_init", ERR_CANT_CREATE, {})
		return
	var prepare_pipeline := rd.compute_pipeline_create(prepare_shader)
	var commit_pipeline := rd.compute_pipeline_create(commit_shader)
	var config := PackedInt32Array([
		maximum_physical_lod, 1, 0, 0,
	]).to_byte_array()
	var params := rd.storage_buffer_create(PARAM_BYTES, config)
	if not prepare_pipeline.is_valid() or not commit_pipeline.is_valid() \
			or not params.is_valid():
		_free_many(rd, [params, commit_pipeline, prepare_pipeline,
			commit_shader, prepare_shader])
		call_deferred(&"_finish_init", ERR_CANT_CREATE, {})
		return
	var prepare_set := rd.uniform_set_create([
		_storage_uniform(0, control_rid),
		_storage_uniform(1, params),
	], prepare_shader, 0)
	var commit_set := rd.uniform_set_create([
		_storage_uniform(0, control_rid),
	], commit_shader, 0)
	if not prepare_set.is_valid() or not commit_set.is_valid():
		_free_many(rd, [commit_set, prepare_set, params,
			commit_pipeline, prepare_pipeline, commit_shader, prepare_shader])
		call_deferred(&"_finish_init", ERR_CANT_CREATE, {})
		return
	call_deferred(&"_finish_init", OK, {
		"prepare_shader": prepare_shader,
		"prepare_pipeline": prepare_pipeline,
		"commit_shader": commit_shader,
		"commit_pipeline": commit_pipeline,
		"params": params,
		"prepare_set": prepare_set,
		"commit_set": commit_set,
	})


func _finish_init(error: Error, bundle: Dictionary) -> void:
	_init_pending = false
	if error != OK:
		initialization_failed.emit(error)
		return
	_prepare_shader = bundle["prepare_shader"]
	_prepare_pipeline = bundle["prepare_pipeline"]
	_commit_shader = bundle["commit_shader"]
	_commit_pipeline = bundle["commit_pipeline"]
	_params = bundle["params"]
	_prepare_set = bundle["prepare_set"]
	_commit_set = bundle["commit_set"]
	_initialized = true
	initialized.emit()


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
	if not _initialized and not _prepare_shader.is_valid():
		return
	var rids := [_commit_set, _prepare_set, _params,
		_commit_pipeline, _prepare_pipeline, _commit_shader, _prepare_shader]
	_initialized = false
	_init_pending = false
	_commit_set = RID(); _prepare_set = RID(); _params = RID()
	_commit_pipeline = RID(); _prepare_pipeline = RID()
	_commit_shader = RID(); _prepare_shader = RID()
	RenderingServer.call_on_render_thread(
		Callable(self, &"_release_render_thread").bind(rids))
	released.emit()


func _release_render_thread(rids: Array) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd != null:
		_free_many(rd, rids)


func _exit_tree() -> void:
	release()
