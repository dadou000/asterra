class_name FixedHydroGPU
extends Node
## Fixed-domain GPU SWE dispatcher used before sparse-tile scheduling exists.
##
## A macro advance is entirely GPU-scheduled on the global RenderingDevice. CFL
## is recomputed before every candidate substep, so accelerating dam-break flows do
## not reuse a stale timestep. The CPU never waits for the reductions.

signal initialized
signal initialization_failed(error: Error)
signal step_recorded(step_id: int)
signal advance_recorded(step_id: int)
signal diagnostics_ready(step_id: int, diagnostics: Dictionary)
signal released

const STATE_FLOATS := 4
const SOURCE_FLOATS := 4
const PARAM_FLOATS := 12
const CONTROL_BYTES := 96
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
var _reset_shader := RID()
var _reset_pipeline := RID()
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
var _reset_set := RID()
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

	var spirv := {
		"step": _load_spirv("res://shaders/water/hydro_step.glsl"),
		"reduce": _load_spirv("res://shaders/water/hydro_reduce.glsl"),
		"reset": _load_spirv("res://shaders/water/hydro_reset_reduction.glsl"),
		"prepare": _load_spirv("res://shaders/water/hydro_prepare_step.glsl"),
		"finalize": _load_spirv("res://shaders/water/hydro_finalize.glsl"),
	}
	for value in spirv.values():
		if value == null:
			return ERR_CANT_OPEN
	if RenderingServer.get_rendering_device() == null:
		return ERR_UNAVAILABLE

	width = w
	height = hgt
	cell_size_m = maxf(p_dx, 1.0e-3)
	_init_pending = true
	RenderingServer.call_on_render_thread(
		Callable(self, &"_init_render_thread").bind(
			spirv, state.to_byte_array(), source_data.to_byte_array()))
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


## Compatibility entry point. A one-substep cap may under-advance if dt is unsafe,
## but it never intentionally records a super-CFL update.
func step(dt_s: float) -> int:
	return advance(dt_s, 1, true)


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
func _init_render_thread(spirv: Dictionary, state_bytes: PackedByteArray,
		source_bytes: PackedByteArray) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null:
		call_deferred("_finish_init", ERR_UNAVAILABLE, {})
		return

	var resources: Array = []
	var bundle := {}
	for key in ["step", "reduce", "reset", "prepare", "finalize"]:
		var shader := rd.shader_create_from_spirv(spirv[key])
		if not shader.is_valid():
			_free_many(rd, resources)
			call_deferred("_finish_init", ERR_CANT_CREATE, {})
			return
		resources.append(shader)
		var pipeline := rd.compute_pipeline_create(shader)
		if not pipeline.is_valid():
			_free_many(rd, resources)
			call_deferred("_finish_init", ERR_CANT_CREATE, {})
			return
		resources.append(pipeline)
		bundle[key + "_shader"] = shader
		bundle[key + "_pipeline"] = pipeline

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
	for buffer in [a, b, src, prm, ctl]:
		if not buffer.is_valid():
			_free_many(rd, resources + [a, b, src, prm, ctl])
			call_deferred("_finish_init", ERR_CANT_CREATE, {})
			return
		resources.append(buffer)
	bundle["state_a"] = a; bundle["state_b"] = b
	bundle["sources"] = src; bundle["params"] = prm; bundle["control"] = ctl

	var step_ab := _make_step_set(rd, bundle.step_shader, a, b, src, prm, ctl)
	var step_ba := _make_step_set(rd, bundle.step_shader, b, a, src, prm, ctl)
	var reduce_a := _make_reduce_set(rd, bundle.reduce_shader, a, ctl, prm)
	var reduce_b := _make_reduce_set(rd, bundle.reduce_shader, b, ctl, prm)
	var reset_set := _make_reset_set(rd, bundle.reset_shader, ctl)
	var prepare_set := _make_prepare_set(rd, bundle.prepare_shader, ctl, prm)
	var finalize_to_a := _make_finalize_set(rd, bundle.finalize_shader, b, a, ctl, prm)
	var finalize_to_b := _make_finalize_set(rd, bundle.finalize_shader, a, b, ctl, prm)
	var sets := [step_ab, step_ba, reduce_a, reduce_b, reset_set, prepare_set,
		finalize_to_a, finalize_to_b]
	for set_rid in sets:
		if not set_rid.is_valid():
			_free_many(rd, resources + sets)
			call_deferred("_finish_init", ERR_CANT_CREATE, {})
			return
		resources.append(set_rid)
	bundle["step_ab"] = step_ab; bundle["step_ba"] = step_ba
	bundle["reduce_a"] = reduce_a; bundle["reduce_b"] = reduce_b
	bundle["reset_set"] = reset_set; bundle["prepare_set"] = prepare_set
	bundle["finalize_to_a"] = finalize_to_a; bundle["finalize_to_b"] = finalize_to_b

	call_deferred("_finish_init", OK, bundle)


func _finish_init(error: Error, bundle: Dictionary) -> void:
	_init_pending = false
	if error != OK:
		_initialized = false
		initialization_failed.emit(error)
		return
	_step_shader = bundle.step_shader; _step_pipeline = bundle.step_pipeline
	_reduce_shader = bundle.reduce_shader; _reduce_pipeline = bundle.reduce_pipeline
	_reset_shader = bundle.reset_shader; _reset_pipeline = bundle.reset_pipeline
	_prepare_shader = bundle.prepare_shader; _prepare_pipeline = bundle.prepare_pipeline
	_finalize_shader = bundle.finalize_shader; _finalize_pipeline = bundle.finalize_pipeline
	_state_a = bundle.state_a; _state_b = bundle.state_b
	_sources = bundle.sources; _params = bundle.params; _control = bundle.control
	_step_ab = bundle.step_ab; _step_ba = bundle.step_ba
	_reduce_a = bundle.reduce_a; _reduce_b = bundle.reduce_b
	_reset_set = bundle.reset_set; _prepare_set = bundle.prepare_set
	_finalize_to_a = bundle.finalize_to_a; _finalize_to_b = bundle.finalize_to_b
	_front_a = true
	_initialized = true
	initialized.emit()


## Render-thread only. Each candidate iteration recomputes characteristic speed
## from the state produced by the preceding active iteration.
func _advance_render_thread(front_a: bool, param_bytes: PackedByteArray,
		step_id: int, cap: int, request_diagnostics: bool) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null or not _all_runtime_rids_valid():
		call_deferred("_finish_advance", step_id, false)
		return

	rd.buffer_update(_params, 0, param_bytes.size(), param_bytes)
	if rd.buffer_clear(_control, 0, CONTROL_BYTES) != OK:
		call_deferred("_finish_advance", step_id, false)
		return

	var groups_x := int(ceil(float(width) / float(LOCAL_X)))
	var groups_y := int(ceil(float(height) / float(LOCAL_Y)))
	var compute := rd.compute_list_begin()

	for iteration in cap:
		# Reset only current-iteration reduction scratch.
		rd.compute_list_bind_compute_pipeline(compute, _reset_pipeline)
		rd.compute_list_bind_uniform_set(compute, _reset_set, 0)
		rd.compute_list_dispatch(compute, 1, 1, 1)
		rd.compute_list_add_barrier(compute)

		# Active steps are always a prefix, therefore iteration parity is identical
		# to current-state parity until the scheduler finishes the requested time.
		var input_is_a := front_a if (iteration & 1) == 0 else not front_a
		rd.compute_list_bind_compute_pipeline(compute, _reduce_pipeline)
		rd.compute_list_bind_uniform_set(compute, _reduce_a if input_is_a else _reduce_b, 0)
		_set_u32_push(rd, compute, 0)
		rd.compute_list_dispatch(compute, groups_x, groups_y, 1)
		rd.compute_list_add_barrier(compute)

		# Recompute a safe dt from this exact state and update remaining macro time.
		rd.compute_list_bind_compute_pipeline(compute, _prepare_pipeline)
		rd.compute_list_bind_uniform_set(compute, _prepare_set, 0)
		_set_u32_push(rd, compute, iteration)
		rd.compute_list_dispatch(compute, 1, 1, 1)
		rd.compute_list_add_barrier(compute)

		# No-op automatically once the requested macro time has been consumed.
		rd.compute_list_bind_compute_pipeline(compute, _step_pipeline)
		rd.compute_list_bind_uniform_set(compute, _step_ab if input_is_a else _step_ba, 0)
		_set_u32_push(rd, compute, iteration)
		rd.compute_list_dispatch(compute, groups_x, groups_y, 1)
		rd.compute_list_add_barrier(compute)

	# Restore the original CPU-known authoritative RID when an odd number of
	# adaptive substeps executed.
	rd.compute_list_bind_compute_pipeline(compute, _finalize_pipeline)
	rd.compute_list_bind_uniform_set(compute,
		_finalize_to_a if front_a else _finalize_to_b, 0)
	rd.compute_list_dispatch(compute, groups_x, groups_y, 1)
	rd.compute_list_add_barrier(compute)

	# Final-state diagnostics use the canonical state.
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
	step_recorded.emit(step_id)
	advance_recorded.emit(step_id)


func _on_diagnostics_bytes(bytes: PackedByteArray, step_id: int) -> void:
	if bytes.size() < CONTROL_BYTES:
		return
	var steps := int(bytes.decode_u32(56))
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
		"remaining_dt_s": bytes.decode_float(36),
		"last_sub_dt_s": bytes.decode_float(40),
		"advanced_dt_s": bytes.decode_float(44),
		"min_cfl_dt_s": bytes.decode_float(48),
		"last_cfl_dt_s": bytes.decode_float(52),
		"steps_taken": steps,
		"substeps": steps,
		"max_substeps": int(bytes.decode_u32(60)),
		"cfl_clamped": bytes.decode_u32(64) != 0,
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
		and _reset_pipeline.is_valid() and _prepare_pipeline.is_valid() \
		and _finalize_pipeline.is_valid() and _state_a.is_valid() \
		and _state_b.is_valid() and _sources.is_valid() \
		and _params.is_valid() and _control.is_valid()


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
func _make_reset_set(rd: RenderingDevice, shader: RID, ctl: RID) -> RID:
	return rd.uniform_set_create([_storage_uniform(0, ctl)], shader, 0)


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
func _free_many(rd: RenderingDevice, rids: Array) -> void:
	for value in rids:
		if value is RID:
			var rid: RID = value
			if rid.is_valid():
				rd.free_rid(rid)


## Render-thread only.
func _release_render_thread(rids: Array) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd != null:
		_free_many(rd, rids)


func release() -> void:
	if not _initialized and not _step_shader.is_valid():
		return
	var rids := [
		_step_ab, _step_ba, _reduce_a, _reduce_b, _reset_set, _prepare_set,
		_finalize_to_a, _finalize_to_b,
		_state_a, _state_b, _sources, _params, _control,
		_step_pipeline, _reduce_pipeline, _reset_pipeline, _prepare_pipeline,
		_finalize_pipeline,
		_step_shader, _reduce_shader, _reset_shader, _prepare_shader, _finalize_shader,
	]
	_initialized = false
	_step_pending = false
	_step_shader = RID(); _step_pipeline = RID()
	_reduce_shader = RID(); _reduce_pipeline = RID()
	_reset_shader = RID(); _reset_pipeline = RID()
	_prepare_shader = RID(); _prepare_pipeline = RID()
	_finalize_shader = RID(); _finalize_pipeline = RID()
	_state_a = RID(); _state_b = RID(); _sources = RID(); _params = RID(); _control = RID()
	_step_ab = RID(); _step_ba = RID(); _reduce_a = RID(); _reduce_b = RID()
	_reset_set = RID(); _prepare_set = RID()
	_finalize_to_a = RID(); _finalize_to_b = RID()
	_latest_diagnostics.clear()
	RenderingServer.call_on_render_thread(
		Callable(self, &"_release_render_thread").bind(rids))
	released.emit()


func _exit_tree() -> void:
	release()
