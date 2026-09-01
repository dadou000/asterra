class_name FixedHydroGPU
extends Node
## Phase 2 fixed-domain GPU SWE dispatcher.
##
## This is deliberately not sparse yet. It proves the authoritative compute path
## on the same global RenderingDevice used by the water renderer. State is two
## ping-pong SSBOs of vec4(h, hu, hv, bed). All RD mutations/dispatches happen on
## the render thread; no per-step CPU readback is required.

signal initialized
signal initialization_failed(error: Error)
signal step_recorded(step_id: int)
signal released

const STATE_FLOATS := 4
const SOURCE_FLOATS := 4
const LOCAL_SIZE_X := 8
const LOCAL_SIZE_Y := 8

var width := 0
var height := 0
var cell_size_m := 1.0
var gravity := 9.81
var dry_eps := 1.0e-5
var manning_n := 0.025

var _rd: RenderingDevice
var _shader := RID()
var _pipeline := RID()
var _state_a := RID()
var _state_b := RID()
var _sources := RID()
var _params := RID()
var _set_a_to_b := RID()
var _set_b_to_a := RID()
var _front_is_a := true
var _initialized := false
var _initialization_pending := false
var _step_pending := false
var _next_step_id := 1


func initialize(p_width: int, p_height: int, p_cell_size_m: float,
		state: PackedFloat32Array, source_terms := PackedFloat32Array()) -> Error:
	if _initialization_pending or _step_pending:
		return ERR_BUSY
	var w := maxi(p_width, 1)
	var hgt := maxi(p_height, 1)
	var count := w * hgt
	if state.size() != count * STATE_FLOATS:
		return ERR_INVALID_PARAMETER

	var sources_data: PackedFloat32Array = source_terms
	if sources_data.is_empty():
		sources_data = PackedFloat32Array()
		sources_data.resize(count * SOURCE_FLOATS)
	elif sources_data.size() != count * SOURCE_FLOATS:
		return ERR_INVALID_PARAMETER

	var shader_file: RDShaderFile = load("res://shaders/water/hydro_step.glsl")
	if shader_file == null:
		return ERR_CANT_OPEN
	var spirv := shader_file.get_spirv()
	if spirv == null:
		return ERR_CANT_CREATE

	width = w
	height = hgt
	cell_size_m = maxf(p_cell_size_m, 1.0e-3)
	_rd = RenderingServer.get_rendering_device()
	if _rd == null:
		return ERR_UNAVAILABLE

	_initialization_pending = true
	var state_bytes := state.to_byte_array()
	var source_bytes := sources_data.to_byte_array()
	RenderingServer.call_on_render_thread(
		Callable(self, &"_initialize_render_thread").bind(
			spirv, state_bytes, source_bytes, count))
	return OK


func initialized_ok() -> bool:
	return _initialized


func initialization_pending() -> bool:
	return _initialization_pending


func step_pending() -> bool:
	return _step_pending


## Records one CFL-safe caller-selected substep. The scheduler will later choose
## dt from a GPU reduction; for Phase 2 tests the reference solver supplies/limits
## the requested dt. Returns step id, or -1 if unavailable/busy.
func step(dt_s: float) -> int:
	if not _initialized or _step_pending or dt_s <= 0.0:
		return -1
	_step_pending = true
	var step_id := _next_step_id
	_next_step_id += 1
	var params_data := PackedFloat32Array([
		float(width), float(height), cell_size_m, dt_s,
		gravity, dry_eps, manning_n, 0.0,
	])
	var front_a := _front_is_a
	RenderingServer.call_on_render_thread(
		Callable(self, &"_step_render_thread").bind(
			front_a, params_data.to_byte_array(), step_id))
	return step_id


func current_state_rid() -> RID:
	if not _initialized:
		return RID()
	return _state_a if _front_is_a else _state_b


func source_buffer_rid() -> RID:
	return _sources if _initialized else RID()


func cell_count() -> int:
	return width * height


func gpu_bytes_estimate() -> int:
	var count := cell_count()
	return count * (STATE_FLOATS * 4 * 2 + SOURCE_FLOATS * 4) + 8 * 4


func stats() -> Dictionary:
	return {
		"initialized": _initialized,
		"initialization_pending": _initialization_pending,
		"step_pending": _step_pending,
		"width": width,
		"height": height,
		"cell_size_m": cell_size_m,
		"cells": cell_count(),
		"gpu_bytes": gpu_bytes_estimate(),
		"front": "A" if _front_is_a else "B",
	}


## Render-thread only.
func _initialize_render_thread(spirv: RDShaderSPIRV, state_bytes: PackedByteArray,
		source_bytes: PackedByteArray, count: int) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null:
		call_deferred("_finish_initialization", ERR_UNAVAILABLE,
			RID(), RID(), RID(), RID(), RID(), RID(), RID())
		return

	var shader := rd.shader_create_from_spirv(spirv)
	if not shader.is_valid():
		call_deferred("_finish_initialization", ERR_CANT_CREATE,
			RID(), RID(), RID(), RID(), RID(), RID(), RID())
		return
	var pipeline := rd.compute_pipeline_create(shader)
	if not pipeline.is_valid():
		rd.free_rid(shader)
		call_deferred("_finish_initialization", ERR_CANT_CREATE,
			RID(), RID(), RID(), RID(), RID(), RID(), RID())
		return

	var zero_state := PackedByteArray()
	zero_state.resize(state_bytes.size())
	var params_bytes := PackedByteArray()
	params_bytes.resize(8 * 4)
	var state_a := rd.storage_buffer_create(state_bytes.size(), state_bytes)
	var state_b := rd.storage_buffer_create(zero_state.size(), zero_state)
	var sources := rd.storage_buffer_create(source_bytes.size(), source_bytes)
	var params := rd.storage_buffer_create(params_bytes.size(), params_bytes)
	if not state_a.is_valid() or not state_b.is_valid() \
			or not sources.is_valid() or not params.is_valid():
		_free_render_thread_many(rd, [state_a, state_b, sources, params, pipeline, shader])
		call_deferred("_finish_initialization", ERR_CANT_CREATE,
			RID(), RID(), RID(), RID(), RID(), RID(), RID())
		return

	var set_a := rd.uniform_set_create([
		_storage_uniform(0, state_a), _storage_uniform(1, state_b),
		_storage_uniform(2, sources), _storage_uniform(3, params),
	], shader, 0)
	var set_b := rd.uniform_set_create([
		_storage_uniform(0, state_b), _storage_uniform(1, state_a),
		_storage_uniform(2, sources), _storage_uniform(3, params),
	], shader, 0)
	if not set_a.is_valid() or not set_b.is_valid():
		_free_render_thread_many(rd,
			[set_a, set_b, state_a, state_b, sources, params, pipeline, shader])
		call_deferred("_finish_initialization", ERR_CANT_CREATE,
			RID(), RID(), RID(), RID(), RID(), RID(), RID())
		return

	call_deferred("_finish_initialization", OK, shader, pipeline,
		state_a, state_b, sources, params, set_a, set_b)


func _finish_initialization(error: Error, shader: RID, pipeline: RID,
		state_a: RID, state_b: RID, sources: RID, params: RID,
		set_a: RID, set_b: RID) -> void:
	_initialization_pending = false
	if error != OK:
		_initialization_failed(error)
		return
	_shader = shader
	_pipeline = pipeline
	_state_a = state_a
	_state_b = state_b
	_sources = sources
	_params = params
	_set_a_to_b = set_a
	_set_b_to_a = set_b
	_front_is_a = true
	_initialized = true
	initialized.emit()


func _initialization_failed(error: Error) -> void:
	_initialized = false
	initialization_failed.emit(error)


## Render-thread only. Global RenderingDevice command lists are consumed by the
## renderer; unlike a local RenderingDevice, do not call submit()/sync() here.
func _step_render_thread(front_a: bool, params_bytes: PackedByteArray,
		step_id: int) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null or not _pipeline.is_valid() or not _params.is_valid():
		call_deferred("_finish_step", step_id, false, front_a)
		return
	var set_rid := _set_a_to_b if front_a else _set_b_to_a
	if not set_rid.is_valid():
		call_deferred("_finish_step", step_id, false, front_a)
		return

	rd.buffer_update(_params, 0, params_bytes.size(), params_bytes)
	var list := rd.compute_list_begin()
	rd.compute_list_bind_compute_pipeline(list, _pipeline)
	rd.compute_list_bind_uniform_set(list, set_rid, 0)
	rd.compute_list_dispatch(list,
		int(ceil(float(width) / float(LOCAL_SIZE_X))),
		int(ceil(float(height) / float(LOCAL_SIZE_Y))), 1)
	rd.compute_list_end()
	call_deferred("_finish_step", step_id, true, front_a)


func _finish_step(step_id: int, success: bool, previous_front_a: bool) -> void:
	_step_pending = false
	if not success:
		push_error("FixedHydroGPU: failed to record compute step %d" % step_id)
		return
	# GPU command ordering guarantees this output becomes the next input before a
	# subsequent render-thread callback records another step.
	_front_is_a = not previous_front_a
	step_recorded.emit(step_id)


func _storage_uniform(binding: int, buffer: RID) -> RDUniform:
	var uniform := RDUniform.new()
	uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
	uniform.binding = binding
	uniform.add_id(buffer)
	return uniform


## Render-thread only.
func _free_render_thread_many(rd: RenderingDevice, rids: Array) -> void:
	for value in rids:
		if value is RID:
			var rid: RID = value
			if rid.is_valid():
				rd.free_rid(rid)


## Render-thread only.
func _release_render_thread(rids: Array) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd != null:
		_free_render_thread_many(rd, rids)


func release() -> void:
	if not _initialized and not _shader.is_valid():
		return
	var rids: Array = [
		_set_a_to_b, _set_b_to_a, _state_a, _state_b,
		_sources, _params, _pipeline, _shader,
	]
	_initialized = false
	_step_pending = false
	_shader = RID()
	_pipeline = RID()
	_state_a = RID()
	_state_b = RID()
	_sources = RID()
	_params = RID()
	_set_a_to_b = RID()
	_set_b_to_a = RID()
	RenderingServer.call_on_render_thread(
		Callable(self, &"_release_render_thread").bind(rids))
	released.emit()


func _exit_tree() -> void:
	release()
	_rd = null
