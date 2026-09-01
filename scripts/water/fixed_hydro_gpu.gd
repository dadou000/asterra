class_name FixedHydroGPU
extends Node
## Fixed-domain GPU SWE dispatcher used before sparse-tile scheduling exists.
## State ping-pongs as vec4(h, hu, hv, bed) on the global RenderingDevice.

signal initialized
signal initialization_failed(error: Error)
signal step_recorded(step_id: int)
signal released

const STATE_FLOATS := 4
const SOURCE_FLOATS := 4
const LOCAL_X := 8
const LOCAL_Y := 8

var width := 0
var height := 0
var cell_size_m := 1.0
var gravity := 9.81
var dry_eps := 1.0e-5
var manning_n := 0.025

var _shader := RID()
var _pipeline := RID()
var _state_a := RID()
var _state_b := RID()
var _sources := RID()
var _params := RID()
var _set_a_b := RID()
var _set_b_a := RID()
var _front_a := true
var _initialized := false
var _init_pending := false
var _step_pending := false
var _next_step_id := 1


func initialize(p_width: int, p_height: int, p_dx: float,
		state: PackedFloat32Array, source_terms := PackedFloat32Array()) -> Error:
	if _init_pending or _step_pending:
		return ERR_BUSY
	var w := maxi(p_width, 1)
	var hgt := maxi(p_height, 1)
	var cell_count := w * hgt
	if state.size() != cell_count * STATE_FLOATS:
		return ERR_INVALID_PARAMETER
	var source_data: PackedFloat32Array = source_terms
	if source_data.is_empty():
		source_data = PackedFloat32Array()
		source_data.resize(cell_count * SOURCE_FLOATS)
	elif source_data.size() != cell_count * SOURCE_FLOATS:
		return ERR_INVALID_PARAMETER

	var shader_file: RDShaderFile = load("res://shaders/water/hydro_step.glsl")
	if shader_file == null:
		return ERR_CANT_OPEN
	var spirv: RDShaderSPIRV = shader_file.get_spirv()
	if spirv == null or RenderingServer.get_rendering_device() == null:
		return ERR_UNAVAILABLE

	width = w
	height = hgt
	cell_size_m = maxf(p_dx, 1.0e-3)
	_init_pending = true
	RenderingServer.call_on_render_thread(
		Callable(self, &"_init_render_thread").bind(
			spirv, state.to_byte_array(), source_data.to_byte_array()))
	return OK


func initialized_ok() -> bool:
	return _initialized


func initialization_pending() -> bool:
	return _init_pending


func step_pending() -> bool:
	return _step_pending


func step(dt_s: float) -> int:
	if not _initialized or _step_pending or dt_s <= 0.0:
		return -1
	var step_id := _next_step_id
	_next_step_id += 1
	_step_pending = true
	var p := PackedFloat32Array([
		float(width), float(height), cell_size_m, dt_s,
		gravity, dry_eps, manning_n, 0.0,
	])
	RenderingServer.call_on_render_thread(
		Callable(self, &"_step_render_thread").bind(
			_front_a, p.to_byte_array(), step_id))
	return step_id


func current_state_rid() -> RID:
	if not _initialized:
		return RID()
	return _state_a if _front_a else _state_b


func source_buffer_rid() -> RID:
	return _sources if _initialized else RID()


func cell_count() -> int:
	return width * height


func gpu_bytes_estimate() -> int:
	return cell_count() * (STATE_FLOATS * 8 + SOURCE_FLOATS * 4) + 32


func stats() -> Dictionary:
	return {
		"initialized": _initialized,
		"initialization_pending": _init_pending,
		"step_pending": _step_pending,
		"width": width,
		"height": height,
		"cell_size_m": cell_size_m,
		"cells": cell_count(),
		"gpu_bytes": gpu_bytes_estimate(),
		"front": "A" if _front_a else "B",
	}


## Render-thread only.
func _init_render_thread(spirv: RDShaderSPIRV, state_bytes: PackedByteArray,
		source_bytes: PackedByteArray) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null:
		_fail_init_deferred(ERR_UNAVAILABLE)
		return
	var shader := rd.shader_create_from_spirv(spirv)
	if not shader.is_valid():
		_fail_init_deferred(ERR_CANT_CREATE)
		return
	var pipeline := rd.compute_pipeline_create(shader)
	if not pipeline.is_valid():
		rd.free_rid(shader)
		_fail_init_deferred(ERR_CANT_CREATE)
		return

	var zero_state := PackedByteArray()
	zero_state.resize(state_bytes.size())
	var zero_params := PackedByteArray()
	zero_params.resize(32)
	var a := rd.storage_buffer_create(state_bytes.size(), state_bytes)
	var b := rd.storage_buffer_create(zero_state.size(), zero_state)
	var src := rd.storage_buffer_create(source_bytes.size(), source_bytes)
	var prm := rd.storage_buffer_create(zero_params.size(), zero_params)
	if not a.is_valid() or not b.is_valid() or not src.is_valid() or not prm.is_valid():
		var failed_rids: Array[RID] = [a, b, src, prm, pipeline, shader]
		_free_many(rd, failed_rids)
		_fail_init_deferred(ERR_CANT_CREATE)
		return

	var set_ab := _make_set(rd, shader, a, b, src, prm)
	var set_ba := _make_set(rd, shader, b, a, src, prm)
	if not set_ab.is_valid() or not set_ba.is_valid():
		var failed_rids: Array[RID] = [set_ab, set_ba, a, b, src, prm, pipeline, shader]
		_free_many(rd, failed_rids)
		_fail_init_deferred(ERR_CANT_CREATE)
		return
	call_deferred("_finish_init", OK, shader, pipeline, a, b, src, prm, set_ab, set_ba)


## Render-thread only.
func _fail_init_deferred(error: Error) -> void:
	call_deferred("_finish_init", error,
		RID(), RID(), RID(), RID(), RID(), RID(), RID(), RID())


func _finish_init(error: Error, shader: RID, pipeline: RID,
		a: RID, b: RID, src: RID, prm: RID, set_ab: RID, set_ba: RID) -> void:
	_init_pending = false
	if error != OK:
		_initialized = false
		initialization_failed.emit(error)
		return
	_shader = shader
	_pipeline = pipeline
	_state_a = a
	_state_b = b
	_sources = src
	_params = prm
	_set_a_b = set_ab
	_set_b_a = set_ba
	_front_a = true
	_initialized = true
	initialized.emit()


## Render-thread only. The global RD is submitted by Godot's renderer; no
## submit()/sync() call belongs here.
func _step_render_thread(front_a: bool, param_bytes: PackedByteArray,
		step_id: int) -> void:
	var rd := RenderingServer.get_rendering_device()
	var set_rid := _set_a_b if front_a else _set_b_a
	if rd == null or not _pipeline.is_valid() or not _params.is_valid() \
			or not set_rid.is_valid():
		call_deferred("_finish_step", step_id, false, front_a)
		return
	rd.buffer_update(_params, 0, param_bytes.size(), param_bytes)
	var list := rd.compute_list_begin()
	rd.compute_list_bind_compute_pipeline(list, _pipeline)
	rd.compute_list_bind_uniform_set(list, set_rid, 0)
	rd.compute_list_dispatch(list,
		int(ceil(float(width) / float(LOCAL_X))),
		int(ceil(float(height) / float(LOCAL_Y))), 1)
	rd.compute_list_end()
	call_deferred("_finish_step", step_id, true, front_a)


func _finish_step(step_id: int, success: bool, old_front_a: bool) -> void:
	_step_pending = false
	if not success:
		push_error("FixedHydroGPU: failed to record step %d" % step_id)
		return
	_front_a = not old_front_a
	step_recorded.emit(step_id)


## Render-thread only.
func _make_set(rd: RenderingDevice, shader: RID, input: RID, output: RID,
		src: RID, prm: RID) -> RID:
	var uniforms: Array[RDUniform] = []
	uniforms.append(_storage_uniform(0, input))
	uniforms.append(_storage_uniform(1, output))
	uniforms.append(_storage_uniform(2, src))
	uniforms.append(_storage_uniform(3, prm))
	return rd.uniform_set_create(uniforms, shader, 0)


func _storage_uniform(binding: int, buffer: RID) -> RDUniform:
	var u := RDUniform.new()
	u.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
	u.binding = binding
	u.add_id(buffer)
	return u


## Render-thread only.
func _free_many(rd: RenderingDevice, rids: Array[RID]) -> void:
	for rid in rids:
		if rid.is_valid():
			rd.free_rid(rid)


## Render-thread only.
func _release_render_thread(rids: Array[RID]) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd != null:
		_free_many(rd, rids)


func release() -> void:
	if not _initialized and not _shader.is_valid():
		return
	var rids: Array[RID] = [
		_set_a_b, _set_b_a, _state_a, _state_b,
		_sources, _params, _pipeline, _shader,
	]
	_initialized = false
	_step_pending = false
	_shader = RID(); _pipeline = RID()
	_state_a = RID(); _state_b = RID(); _sources = RID(); _params = RID()
	_set_a_b = RID(); _set_b_a = RID()
	RenderingServer.call_on_render_thread(
		Callable(self, &"_release_render_thread").bind(rids))
	released.emit()


func _exit_tree() -> void:
	release()
