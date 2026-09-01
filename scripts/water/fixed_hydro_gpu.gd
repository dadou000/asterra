class_name FixedHydroGPU
extends Node
## Fixed-domain GPU SWE dispatcher used before sparse-tile scheduling exists.
##
## A macro advance is recorded entirely on the global RenderingDevice:
##   pre-step reduction -> GPU CFL schedule -> conditional substeps -> canonicalize
##   -> post-step reduction -> compact asynchronous diagnostic readback.
##
## The authoritative state RID remains stable across advance(): if the GPU selects
## an odd number of ping-pong substeps, hydro_finalize copies the result back into
## the buffer that was authoritative at the start of the macro step.

signal initialized
signal initialization_failed(error: Error)
signal step_recorded(step_id: int)
signal advance_recorded(step_id: int)
signal diagnostics_ready(step_id: int, diagnostics: Dictionary)
signal released

const STATE_FLOATS := 4
const SOURCE_FLOATS := 4
const PARAM_FLOATS := 12
const CONTROL_BYTES := 64
const LOCAL_X := 8
const LOCAL_Y := 8
const MAX_GPU_SUBSTEPS := 32

var width := 0
var height := 0
var cell_size_m := 1.0
var gravity := 9.81
var dry_eps := 1.0e-5
var manning_n := 0.025
var cfl := 0.42

var _step_shader := RID()
var _step_pipeline := RID()
var _reduce_shader := RID()
var _reduce_pipeline := RID()
var _prepare_shader := RID()
var _prepare_pipeline := RID()
var _finalize_shader := RID()
var _finalize_pipeline := RID()

var _state_a := RID()
var _state_b := RID()
var _sources := RID()
var _params := RID()
var _control := RID()

var _step_ab := RID()
var _step_ba := RID()
var _reduce_a := RID()
var _reduce_b := RID()
var _prepare_set := RID()
var _finalize_to_a := RID()
var _finalize_to_b := RID()

var _front_a := true
var _initialized := false
var _init_pending := false
var _step_pending := false
var _next_step_id := 1
var _latest_diagnostics: Dictionary = {}


func initialize(p_width: int, p_height: int, p_dx: float,
		state: PackedFloat32Array, source_terms := PackedFloat32Array()) -> Error:
	if _init_pending or _step_pending:
		return ERR_BUSY
	var w := maxi(p_width, 1)
	var hgt := maxi(p_height, 1)
	var count := w * hgt
	if state.size() != count * STATE_FLOATS:
		return ERR_INVALID_PARAMETER
	var source_data: PackedFloat32Array = source_terms
	if source_data.is_empty():
		source_data = PackedFloat32Array()
		source_data.resize(count * SOURCE_FLOATS)
	elif source_data.size() != count * SOURCE_FLOATS:
		return ERR_INVALID_PARAMETER

	var step_spirv := _load_spirv("res://shaders/water/hydro_step.glsl")
	var reduce_spirv := _load_spirv("res://shaders/water/hydro_reduce.glsl")
	var prepare_spirv := _load_spirv("res://shaders/water/hydro_prepare_step.glsl")
	var finalize_spirv := _load_spirv("res://shaders/water/hydro_finalize.glsl")
	if step_spirv == null or reduce_spirv == null or prepare_spirv == null \
			or finalize_spirv == null:
		return ERR_CANT_OPEN
	if RenderingServer.get_rendering_device() == null:
		return ERR_UNAVAILABLE

	width = w
	height = hgt
	cell_size_m = maxf(p_dx, 1.0e-3)
	_init_pending = true
	RenderingServer.call_on_render_thread(
		Callable(self, &"_init_render_thread").bind(
			step_spirv, reduce_spirv, prepare_spirv, finalize_spirv,
			state.to_byte_array(), source_data.to_byte_array()))
	return OK


func _load_spirv(path: String) -> RDShaderSPIRV:
	var shader_file: RDShaderFile = load(path)
	return null if shader_file == null else shader_file.get_spirv()


func initialized_ok() -> bool:
	return _initialized


func initialization_pending() -> bool:
	return _init_pending


func step_pending() -> bool:
	return _step_pending


## Compatibility entry point. It now uses the same GPU CFL scheduler with a cap
## of one substep; if dt is unsafe, it advances only the safe amount rather than
## deliberately violating CFL stability.
func step(dt_s: float) -> int:
	return advance(dt_s, 1, true)


## Advance a requested macro timestep. The CPU records max_substeps candidate
## dispatches, but hydro_prepare_step computes the active prefix and sub_dt on GPU.
func advance(dt_s: float, max_substeps: int = 16,
		request_diagnostics: bool = true) -> int:
	if not _initialized or _step_pending or dt_s <= 0.0:
		return -1
	var cap := clampi(max_substeps, 1, MAX_GPU_SUBSTEPS)
	var step_id := _next_step_id
	_next_step_id += 1
	_step_pending = true
	var p := PackedFloat32Array([
		float(width), float(height), cell_size_m, dt_s,
		gravity, dry_eps, manning_n, clampf(cfl, 0.01, 0.95),
		float(cap), 0.0, 0.0, 0.0,
	])
	RenderingServer.call_on_render_thread(
		Callable(self, &"_advance_render_thread").bind(
			_front_a, p.to_byte_array(), step_id, cap, request_diagnostics))
	return step_id


func current_state_rid() -> RID:
	if not _initialized:
		return RID()
	return _state_a if _front_a else _state_b


func source_buffer_rid() -> RID:
	return _sources if _initialized else RID()


func control_buffer_rid() -> RID:
	return _control if _initialized else RID()


func cell_count() -> int:
	return width * height


func latest_diagnostics() -> Dictionary:
	return _latest_diagnostics.duplicate(true)


func gpu_bytes_estimate() -> int:
	return cell_count() * (STATE_FLOATS * 8 + SOURCE_FLOATS * 4) \
		+ PARAM_FLOATS * 4 + CONTROL_BYTES


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
		"max_gpu_substeps": MAX_GPU_SUBSTEPS,
		"latest_diagnostics": latest_diagnostics(),
	}


## Render-thread only.
func _init_render_thread(step_spirv: RDShaderSPIRV, reduce_spirv: RDShaderSPIRV,
		prepare_spirv: RDShaderSPIRV, finalize_spirv: RDShaderSPIRV,
		state_bytes: PackedByteArray, source_bytes: PackedByteArray) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null:
		_fail_init_deferred(ERR_UNAVAILABLE)
		return

	var step_shader := rd.shader_create_from_spirv(step_spirv)
	var reduce_shader := rd.shader_create_from_spirv(reduce_spirv)
	var prepare_shader := rd.shader_create_from_spirv(prepare_spirv)
	var finalize_shader := rd.shader_create_from_spirv(finalize_spirv)
	var shaders: Array[RID] = [step_shader, reduce_shader, prepare_shader, finalize_shader]
	for shader in shaders:
		if not shader.is_valid():
			_free_many(rd, shaders)
			_fail_init_deferred(ERR_CANT_CREATE)
			return

	var step_pipeline := rd.compute_pipeline_create(step_shader)
	var reduce_pipeline := rd.compute_pipeline_create(reduce_shader)
	var prepare_pipeline := rd.compute_pipeline_create(prepare_shader)
	var finalize_pipeline := rd.compute_pipeline_create(finalize_shader)
	var pipelines: Array[RID] = [step_pipeline, reduce_pipeline, prepare_pipeline, finalize_pipeline]
	for pipeline in pipelines:
		if not pipeline.is_valid():
			_free_many(rd, pipelines + shaders)
			_fail_init_deferred(ERR_CANT_CREATE)
			return

	var zero_state := PackedByteArray()
	zero_state.resize(state_bytes.size())
	var zero_params := PackedByteArray()
	zero_params.resize(PARAM_FLOATS * 4)
	var zero_control := PackedByteArray()
	zero_control.resize(CONTROL_BYTES)
	var a := rd.storage_buffer_create(state_bytes.size(), state_bytes)
	var b := rd.storage_buffer_create(zero_state.size(), zero_state)
	var src := rd.storage_buffer_create(source_bytes.size(), source_bytes)
	var prm := rd.storage_buffer_create(zero_params.size(), zero_params)
	var ctl := rd.storage_buffer_create(zero_control.size(), zero_control)
	var buffers: Array[RID] = [a, b, src, prm, ctl]
	for buffer in buffers:
		if not buffer.is_valid():
			_free_many(rd, buffers + pipelines + shaders)
			_fail_init_deferred(ERR_CANT_CREATE)
			return

	var step_ab := _make_step_set(rd, step_shader, a, b, src, prm, ctl)
	var step_ba := _make_step_set(rd, step_shader, b, a, src, prm, ctl)
	var reduce_a := _make_reduce_set(rd, reduce_shader, a, ctl, prm)
	var reduce_b := _make_reduce_set(rd, reduce_shader, b, ctl, prm)
	var prepare_set := _make_prepare_set(rd, prepare_shader, ctl, prm)
	var finalize_to_a := _make_finalize_set(rd, finalize_shader, b, a, ctl, prm)
	var finalize_to_b := _make_finalize_set(rd, finalize_shader, a, b, ctl, prm)
	var sets: Array[RID] = [
		step_ab, step_ba, reduce_a, reduce_b, prepare_set,
		finalize_to_a, finalize_to_b,
	]
	for set_rid in sets:
		if not set_rid.is_valid():
			_free_many(rd, sets + buffers + pipelines + shaders)
			_fail_init_deferred(ERR_CANT_CREATE)
			return

	call_deferred("_finish_init", OK,
		step_shader, step_pipeline, reduce_shader, reduce_pipeline,
		prepare_shader, prepare_pipeline, finalize_shader, finalize_pipeline,
		a, b, src, prm, ctl, step_ab, step_ba, reduce_a, reduce_b,
		prepare_set, finalize_to_a, finalize_to_b)


## Render-thread only.
func _fail_init_deferred(error: Error) -> void:
	call_deferred("_finish_init", error,
		RID(), RID(), RID(), RID(), RID(), RID(), RID(), RID(),
		RID(), RID(), RID(), RID(), RID(), RID(), RID(), RID(), RID(),
		RID(), RID(), RID())


func _finish_init(error: Error,
		step_shader: RID, step_pipeline: RID, reduce_shader: RID, reduce_pipeline: RID,
		prepare_shader: RID, prepare_pipeline: RID,
		finalize_shader: RID, finalize_pipeline: RID,
		a: RID, b: RID, src: RID, prm: RID, ctl: RID,
		step_ab: RID, step_ba: RID, reduce_a: RID, reduce_b: RID,
		prepare_set: RID, finalize_to_a: RID, finalize_to_b: RID) -> void:
	_init_pending = false
	if error != OK:
		_initialized = false
		initialization_failed.emit(error)
		return
	_step_shader = step_shader; _step_pipeline = step_pipeline
	_reduce_shader = reduce_shader; _reduce_pipeline = reduce_pipeline
	_prepare_shader = prepare_shader; _prepare_pipeline = prepare_pipeline
	_finalize_shader = finalize_shader; _finalize_pipeline = finalize_pipeline
	_state_a = a; _state_b = b; _sources = src; _params = prm; _control = ctl
	_step_ab = step_ab; _step_ba = step_ba
	_reduce_a = reduce_a; _reduce_b = reduce_b
	_prepare_set = prepare_set
	_finalize_to_a = finalize_to_a; _finalize_to_b = finalize_to_b
	_front_a = true
	_initialized = true
	initialized.emit()


## Render-thread only. The global RD is submitted by Godot's renderer; no
## submit()/sync() call belongs here.
func _advance_render_thread(front_a: bool, param_bytes: PackedByteArray,
		step_id: int, cap: int, request_diagnostics: bool) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null or not _all_runtime_rids_valid():
		call_deferred("_finish_advance", step_id, false)
		return

	rd.buffer_update(_params, 0, param_bytes.size(), param_bytes)
	var clear_error := rd.buffer_clear(_control, 0, CONTROL_BYTES)
	if clear_error != OK:
		call_deferred("_finish_advance", step_id, false)
		return

	var groups_x := int(ceil(float(width) / float(LOCAL_X)))
	var groups_y := int(ceil(float(height) / float(LOCAL_Y)))
	var compute := rd.compute_list_begin()

	# Pre-step characteristic-speed + health reduction.
	rd.compute_list_bind_compute_pipeline(compute, _reduce_pipeline)
	rd.compute_list_bind_uniform_set(compute, _reduce_a if front_a else _reduce_b, 0)
	_set_u32_push(rd, compute, 0)
	rd.compute_list_dispatch(compute, groups_x, groups_y, 1)
	rd.compute_list_add_barrier(compute)

	# Convert reduction into a CFL-safe substep schedule entirely on GPU.
	rd.compute_list_bind_compute_pipeline(compute, _prepare_pipeline)
	rd.compute_list_bind_uniform_set(compute, _prepare_set, 0)
	rd.compute_list_dispatch(compute, 1, 1, 1)
	rd.compute_list_add_barrier(compute)

	# Record the maximum candidate sequence. Each shader invocation checks the
	# GPU-computed control.substeps and only the active prefix writes state.
	for substep in cap:
		var input_is_a := front_a if (substep & 1) == 0 else not front_a
		rd.compute_list_bind_compute_pipeline(compute, _step_pipeline)
		rd.compute_list_bind_uniform_set(compute, _step_ab if input_is_a else _step_ba, 0)
		_set_u32_push(rd, compute, substep)
		rd.compute_list_dispatch(compute, groups_x, groups_y, 1)
		rd.compute_list_add_barrier(compute)

	# Make the original front buffer authoritative again regardless of odd/even
	# GPU-selected substep count, so consumers never need an immediate readback.
	rd.compute_list_bind_compute_pipeline(compute, _finalize_pipeline)
	rd.compute_list_bind_uniform_set(compute,
		_finalize_to_a if front_a else _finalize_to_b, 0)
	rd.compute_list_dispatch(compute, groups_x, groups_y, 1)
	rd.compute_list_add_barrier(compute)

	# Post-step health metrics are separate from the pre-step CFL reduction.
	rd.compute_list_bind_compute_pipeline(compute, _reduce_pipeline)
	rd.compute_list_bind_uniform_set(compute, _reduce_a if front_a else _reduce_b, 0)
	_set_u32_push(rd, compute, 1)
	rd.compute_list_dispatch(compute, groups_x, groups_y, 1)
	rd.compute_list_end()

	if request_diagnostics:
		var callback := Callable(self, &"_on_diagnostics_bytes").bind(step_id)
		var err := rd.buffer_get_data_async(_control, callback, 0, CONTROL_BYTES)
		if err != OK:
			push_warning("FixedHydroGPU: async diagnostic readback request failed (%d)." % int(err))

	call_deferred("_finish_advance", step_id, true)


func _finish_advance(step_id: int, success: bool) -> void:
	_step_pending = false
	if not success:
		push_error("FixedHydroGPU: failed to record advance %d" % step_id)
		return
	# Canonicalization intentionally keeps _front_a unchanged.
	step_recorded.emit(step_id)
	advance_recorded.emit(step_id)


func _on_diagnostics_bytes(bytes: PackedByteArray, step_id: int) -> void:
	if bytes.size() < CONTROL_BYTES:
		return
	var diagnostics := {
		"pre_max_speed_mps": bytes.decode_float(0),
		"pre_max_depth_m": bytes.decode_float(4),
		"pre_wet_cells": int(bytes.decode_u32(8)),
		"pre_invalid_cells": int(bytes.decode_u32(12)),
		"post_max_speed_mps": bytes.decode_float(16),
		"post_max_depth_m": bytes.decode_float(20),
		"post_wet_cells": int(bytes.decode_u32(24)),
		"post_invalid_cells": int(bytes.decode_u32(28)),
		"requested_dt_s": bytes.decode_float(32),
		"sub_dt_s": bytes.decode_float(36),
		"advanced_dt_s": bytes.decode_float(40),
		"cfl_dt_s": bytes.decode_float(44),
		"substeps": int(bytes.decode_u32(48)),
		"max_substeps": int(bytes.decode_u32(52)),
		"cfl_clamped": bytes.decode_u32(56) != 0,
	}
	call_deferred("_publish_diagnostics", step_id, diagnostics)


func _publish_diagnostics(step_id: int, diagnostics: Dictionary) -> void:
	_latest_diagnostics = diagnostics
	diagnostics_ready.emit(step_id, diagnostics)


## Render-thread only.
func _set_u32_push(rd: RenderingDevice, compute: int, value: int) -> void:
	var push := PackedInt32Array([value, 0, 0, 0]).to_byte_array()
	rd.compute_list_set_push_constant(compute, push, push.size())


func _all_runtime_rids_valid() -> bool:
	return _step_pipeline.is_valid() and _reduce_pipeline.is_valid() \
		and _prepare_pipeline.is_valid() and _finalize_pipeline.is_valid() \
		and _state_a.is_valid() and _state_b.is_valid() \
		and _sources.is_valid() and _params.is_valid() and _control.is_valid()


## Render-thread only.
func _make_step_set(rd: RenderingDevice, shader: RID, input: RID, output: RID,
		src: RID, prm: RID, ctl: RID) -> RID:
	return rd.uniform_set_create([
		_storage_uniform(0, input), _storage_uniform(1, output),
		_storage_uniform(2, src), _storage_uniform(3, prm),
		_storage_uniform(4, ctl),
	], shader, 0)


## Render-thread only.
func _make_reduce_set(rd: RenderingDevice, shader: RID, state: RID,
		ctl: RID, prm: RID) -> RID:
	return rd.uniform_set_create([
		_storage_uniform(0, state), _storage_uniform(1, ctl),
		_storage_uniform(2, prm),
	], shader, 0)


## Render-thread only.
func _make_prepare_set(rd: RenderingDevice, shader: RID, ctl: RID, prm: RID) -> RID:
	return rd.uniform_set_create([
		_storage_uniform(0, ctl), _storage_uniform(1, prm),
	], shader, 0)


## Render-thread only.
func _make_finalize_set(rd: RenderingDevice, shader: RID, other: RID,
		canonical: RID, ctl: RID, prm: RID) -> RID:
	return rd.uniform_set_create([
		_storage_uniform(0, other), _storage_uniform(1, canonical),
		_storage_uniform(2, ctl), _storage_uniform(3, prm),
	], shader, 0)


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
	if not _initialized and not _step_shader.is_valid():
		return
	var rids: Array[RID] = [
		_step_ab, _step_ba, _reduce_a, _reduce_b, _prepare_set,
		_finalize_to_a, _finalize_to_b,
		_state_a, _state_b, _sources, _params, _control,
		_step_pipeline, _reduce_pipeline, _prepare_pipeline, _finalize_pipeline,
		_step_shader, _reduce_shader, _prepare_shader, _finalize_shader,
	]
	_initialized = false
	_step_pending = false
	_step_shader = RID(); _step_pipeline = RID()
	_reduce_shader = RID(); _reduce_pipeline = RID()
	_prepare_shader = RID(); _prepare_pipeline = RID()
	_finalize_shader = RID(); _finalize_pipeline = RID()
	_state_a = RID(); _state_b = RID(); _sources = RID(); _params = RID(); _control = RID()
	_step_ab = RID(); _step_ba = RID(); _reduce_a = RID(); _reduce_b = RID()
	_prepare_set = RID(); _finalize_to_a = RID(); _finalize_to_b = RID()
	_latest_diagnostics.clear()
	RenderingServer.call_on_render_thread(
		Callable(self, &"_release_render_thread").bind(rids))
	released.emit()


func _exit_tree() -> void:
	release()
