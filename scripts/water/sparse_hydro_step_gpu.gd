class_name SparseHydroStepGPU
extends Node
## Connected sparse SWE dispatcher with GPU-driven adaptive CFL.
##
## Every macro advance records this loop entirely on the global RenderingDevice:
##   reduce occupied atlas A -> prepare safe dt -> A->B SWE -> B->A canonicalize
## and repeats until the requested macro time is consumed or the substep cap is
## reached. Phase 4 derives dx from each occupied tile's quadtree level; the CFL
## reduction therefore uses max(characteristic_speed / local_dx).
##
## Atlas A remains authoritative after every candidate iteration. Source composition
## keeps independent gameplay/world and atmospheric layers, and the external ledger
## records physical m3 using each tile's own cell area.

signal initialized
signal initialization_failed(error: Error)
signal substep_recorded(step_id: int)
signal advance_recorded(step_id: int)
signal diagnostics_ready(step_id: int, diagnostics: Dictionary)
signal released

const LOCAL_X := 8
const LOCAL_Y := 8
const COMMIT_LOCAL_X := 256
const EXTERNAL_REDUCE_LOCAL_X := 256
const PARAM_FLOATS := 12
const COMMIT_PARAM_INTS := 4
# First 96 bytes retain the established diagnostics ABI. The final 8 bytes are
# sparse-only per-iteration HydroLOD CFL scratch.
const CONTROL_BYTES := 104
const SOURCE_FLOATS := 4
const EXTERNAL_LEDGER_FLOATS := 2
const MAX_GPU_SUBSTEPS := 32

var gravity := 9.81
var dry_eps := 1.0e-5
var manning_n := 0.025
var cfl := 0.42

var _atlas: SparseHydroAtlasGPU
var _connectivity: SparseHydroConnectivityGPU

var _step_shader := RID()
var _step_pipeline := RID()
var _commit_shader := RID()
var _commit_pipeline := RID()
var _reduce_shader := RID()
var _reduce_pipeline := RID()
var _reset_shader := RID()
var _reset_pipeline := RID()
var _prepare_shader := RID()
var _prepare_pipeline := RID()
var _external_reduce_shader := RID()
var _external_reduce_pipeline := RID()
var _external_finalize_shader := RID()
var _external_finalize_pipeline := RID()

var _params := RID()
var _commit_params := RID()
var _control := RID()
var _atmospheric_sources := RID()
var _external_flux_ledger := RID()
var _external_flux_partials := RID()
var _step_set := RID()
var _commit_set := RID()
var _reduce_set := RID()
var _reset_set := RID()
var _prepare_set := RID()
var _external_reduce_set := RID()
var _external_finalize_set := RID()

var _initialized := false
var _init_pending := false
var _advance_pending := false
var _diagnostics_pending := false
var _next_step_id := 1
var _latest_diagnostics: Dictionary = {}


func initialize(atlas: SparseHydroAtlasGPU,
		connectivity: SparseHydroConnectivityGPU) -> Error:
	if _initialized or _init_pending or _advance_pending or _diagnostics_pending:
		return ERR_BUSY
	if atlas == null or connectivity == null \
			or not atlas.initialized_ok() or not connectivity.initialized_ok():
		return ERR_INVALID_PARAMETER
	if atlas.capacity != connectivity.capacity:
		return ERR_INVALID_PARAMETER
	if RenderingServer.get_rendering_device() == null:
		return ERR_UNAVAILABLE

	var spirv := {
		"step": _load_spirv("res://shaders/water/sparse_hydro_step.glsl"),
		"commit": _load_spirv("res://shaders/water/sparse_hydro_commit.glsl"),
		"reduce": _load_spirv("res://shaders/water/sparse_hydro_reduce.glsl"),
		"reset": _load_spirv("res://shaders/water/sparse_hydro_reset_reduction.glsl"),
		"prepare": _load_spirv("res://shaders/water/sparse_hydro_prepare_step.glsl"),
		"external_reduce": _load_spirv(
			"res://shaders/water/sparse_hydro_external_flux_reduce.glsl"),
		"external_finalize": _load_spirv(
			"res://shaders/water/sparse_hydro_external_flux_finalize.glsl"),
	}
	for value in spirv.values():
		if value == null:
			return ERR_CANT_OPEN

	_atlas = atlas
	_connectivity = connectivity
	_init_pending = true
	RenderingServer.call_on_render_thread(
		Callable(self, &"_init_render_thread").bind(spirv))
	return OK


func _load_spirv(path: String) -> RDShaderSPIRV:
	var shader_file: RDShaderFile = load(path)
	return null if shader_file == null else shader_file.get_spirv()


func initialized_ok() -> bool:
	return _initialized


func step_pending() -> bool:
	return _advance_pending


func diagnostics_pending() -> bool:
	return _diagnostics_pending


func canonical_state_rid() -> RID:
	return _atlas.state_a_rid() if _initialized and _atlas != null else RID()


func control_buffer_rid() -> RID:
	return _control if _initialized else RID()


## Distributed forcing layer. External writers own no lifetime for this RID and
## must stop using it before this solver is released/reinitialized.
func atmospheric_source_rid() -> RID:
	return _atmospheric_sources if _initialized else RID()


func clear_atmospheric_sources() -> Error:
	if not _initialized or not _atmospheric_sources.is_valid() or _atlas == null:
		return ERR_UNAVAILABLE
	RenderingServer.call_on_render_thread(
		Callable(self, &"_clear_atmospheric_sources_render_thread"))
	return OK


## Compatibility entry point used by earlier connected-boundary tests.
func substep(dt_s: float) -> int:
	return advance(dt_s, 1, false)


func advance(dt_s: float, max_substeps: int = 16,
		request_diagnostics: bool = true) -> int:
	if not _initialized or _advance_pending or _diagnostics_pending \
			or not is_finite(dt_s) or dt_s <= 0.0:
		return -1
	if _atlas == null or _connectivity == null \
			or not _atlas.initialized_ok() or not _connectivity.initialized_ok():
		return -1

	var cap := clampi(max_substeps, 1, MAX_GPU_SUBSTEPS)
	var step_id := _next_step_id
	_next_step_id += 1
	_advance_pending = true
	_diagnostics_pending = request_diagnostics
	var lod_enabled := 1.0 if _atlas.hydrolod_enabled() else 0.0
	var base_level := float(maxi(_atlas.base_tile_level, 0))
	var params := PackedFloat32Array([
		float(_atlas.tile_resolution), float(_atlas.capacity),
		_atlas.cell_size_m, dt_s,
		gravity, dry_eps, manning_n, clampf(cfl, 0.01, 0.95),
		float(cap), base_level, lod_enabled, 0.0,
	])
	RenderingServer.call_on_render_thread(Callable(self, &"_advance_render_thread").bind(
		step_id, cap, request_diagnostics, params.to_byte_array()))
	return step_id


func latest_diagnostics() -> Dictionary:
	return _latest_diagnostics.duplicate(true)


func gpu_bytes_estimate() -> int:
	var forcing_bytes := 0
	var ledger_bytes := 0
	if _atlas != null:
		forcing_bytes = _atlas.total_cell_count() * SOURCE_FLOATS * 4
		ledger_bytes = _atlas.total_cell_count() * EXTERNAL_LEDGER_FLOATS * 4
		var partial_count := int(ceil(float(_atlas.total_cell_count()) \
			/ float(EXTERNAL_REDUCE_LOCAL_X)))
		ledger_bytes += maxi(partial_count, 1) * EXTERNAL_LEDGER_FLOATS * 4
	return PARAM_FLOATS * 4 + COMMIT_PARAM_INTS * 4 + CONTROL_BYTES \
		+ forcing_bytes + ledger_bytes


func stats() -> Dictionary:
	return {
		"initialized": _initialized,
		"initialization_pending": _init_pending,
		"advance_pending": _advance_pending,
		"diagnostics_pending": _diagnostics_pending,
		"gravity": gravity,
		"dry_eps": dry_eps,
		"manning_n": manning_n,
		"cfl": cfl,
		"max_gpu_substeps": MAX_GPU_SUBSTEPS,
		"canonical_state": "atlas_A",
		"hydrolod_metrics": _atlas != null and _atlas.hydrolod_enabled(),
		"hydrolod_cfl_rate_reduction": true,
		"atmospheric_source_layer": _atmospheric_sources.is_valid(),
		"exact_external_flux_ledger": _external_flux_ledger.is_valid(),
		"gpu_bytes_owned": gpu_bytes_estimate(),
		"latest_diagnostics": latest_diagnostics(),
	}


func _init_render_thread(spirv: Dictionary) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null:
		call_deferred("_finish_init", ERR_UNAVAILABLE, {})
		return

	var resources: Array = []
	var bundle := {}
	for key in ["step", "commit", "reduce", "reset", "prepare",
			"external_reduce", "external_finalize"]:
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

	var total_cells := _atlas.total_cell_count()
	var partial_count := maxi(int(ceil(float(total_cells) \
		/ float(EXTERNAL_REDUCE_LOCAL_X))), 1)
	var zero_params := PackedByteArray()
	zero_params.resize(PARAM_FLOATS * 4)
	var zero_control := PackedByteArray()
	zero_control.resize(CONTROL_BYTES)
	var zero_atmospheric := PackedByteArray()
	zero_atmospheric.resize(total_cells * SOURCE_FLOATS * 4)
	var zero_external_ledger := PackedByteArray()
	zero_external_ledger.resize(total_cells * EXTERNAL_LEDGER_FLOATS * 4)
	var zero_external_partials := PackedByteArray()
	zero_external_partials.resize(partial_count * EXTERNAL_LEDGER_FLOATS * 4)
	var commit_values := PackedInt32Array([
		total_cells, _atlas.cells_per_tile(), _atlas.capacity, partial_count,
	])
	var params := rd.storage_buffer_create(zero_params.size(), zero_params)
	var control := rd.storage_buffer_create(zero_control.size(), zero_control)
	var atmospheric_sources := rd.storage_buffer_create(
		zero_atmospheric.size(), zero_atmospheric)
	var external_flux_ledger := rd.storage_buffer_create(
		zero_external_ledger.size(), zero_external_ledger)
	var external_flux_partials := rd.storage_buffer_create(
		zero_external_partials.size(), zero_external_partials)
	var commit_params := rd.storage_buffer_create(
		commit_values.to_byte_array().size(), commit_values.to_byte_array())
	if not params.is_valid() or not control.is_valid() \
			or not atmospheric_sources.is_valid() or not commit_params.is_valid() \
			or not external_flux_ledger.is_valid() or not external_flux_partials.is_valid():
		_free_many(rd, resources + [params, control, atmospheric_sources,
			external_flux_ledger, external_flux_partials, commit_params])
		call_deferred("_finish_init", ERR_CANT_CREATE, {})
		return
	resources.append_array([params, control, atmospheric_sources,
		external_flux_ledger, external_flux_partials, commit_params])

	var step_set := rd.uniform_set_create([
		_storage_uniform(0, _atlas.state_a_rid()),
		_storage_uniform(1, _atlas.state_b_rid()),
		_storage_uniform(2, _atlas.source_rid()),
		_storage_uniform(3, _atlas.occupancy_rid()),
		_storage_uniform(4, _connectivity.neighbor_slots_rid()),
		_storage_uniform(5, _connectivity.neighbor_links_rid()),
		_storage_uniform(6, params),
		_storage_uniform(7, control),
		_storage_uniform(8, atmospheric_sources),
		_storage_uniform(9, external_flux_ledger),
		_storage_uniform(10, _atlas.tile_metadata_rid()),
	], bundle.step_shader, 0)
	var commit_set := rd.uniform_set_create([
		_storage_uniform(0, _atlas.state_b_rid()),
		_storage_uniform(1, _atlas.state_a_rid()),
		_storage_uniform(2, _atlas.occupancy_rid()),
		_storage_uniform(3, commit_params),
	], bundle.commit_shader, 0)
	var reduce_set := rd.uniform_set_create([
		_storage_uniform(0, _atlas.state_a_rid()),
		_storage_uniform(1, _atlas.occupancy_rid()),
		_storage_uniform(2, control),
		_storage_uniform(3, params),
		_storage_uniform(4, _atlas.tile_metadata_rid()),
	], bundle.reduce_shader, 0)
	var reset_set := rd.uniform_set_create([
		_storage_uniform(0, control),
	], bundle.reset_shader, 0)
	var prepare_set := rd.uniform_set_create([
		_storage_uniform(0, control),
		_storage_uniform(1, params),
	], bundle.prepare_shader, 0)
	var external_reduce_set := rd.uniform_set_create([
		_storage_uniform(0, external_flux_ledger),
		_storage_uniform(1, external_flux_partials),
		_storage_uniform(2, commit_params),
	], bundle.external_reduce_shader, 0)
	var external_finalize_set := rd.uniform_set_create([
		_storage_uniform(0, external_flux_partials),
		_storage_uniform(1, control),
		_storage_uniform(2, commit_params),
	], bundle.external_finalize_shader, 0)
	var sets := [step_set, commit_set, reduce_set, reset_set, prepare_set,
		external_reduce_set, external_finalize_set]
	for set_rid in sets:
		if not set_rid.is_valid():
			_free_many(rd, resources + sets)
			call_deferred("_finish_init", ERR_CANT_CREATE, {})
			return
	resources.append_array(sets)

	bundle["params"] = params
	bundle["control"] = control
	bundle["atmospheric_sources"] = atmospheric_sources
	bundle["external_flux_ledger"] = external_flux_ledger
	bundle["external_flux_partials"] = external_flux_partials
	bundle["commit_params"] = commit_params
	bundle["step_set"] = step_set
	bundle["commit_set"] = commit_set
	bundle["reduce_set"] = reduce_set
	bundle["reset_set"] = reset_set
	bundle["prepare_set"] = prepare_set
	bundle["external_reduce_set"] = external_reduce_set
	bundle["external_finalize_set"] = external_finalize_set
	call_deferred("_finish_init", OK, bundle)


func _finish_init(error: Error, bundle: Dictionary) -> void:
	_init_pending = false
	if error != OK:
		_initialized = false
		initialization_failed.emit(error)
		return
	_step_shader = bundle.step_shader; _step_pipeline = bundle.step_pipeline
	_commit_shader = bundle.commit_shader; _commit_pipeline = bundle.commit_pipeline
	_reduce_shader = bundle.reduce_shader; _reduce_pipeline = bundle.reduce_pipeline
	_reset_shader = bundle.reset_shader; _reset_pipeline = bundle.reset_pipeline
	_prepare_shader = bundle.prepare_shader; _prepare_pipeline = bundle.prepare_pipeline
	_external_reduce_shader = bundle.external_reduce_shader
	_external_reduce_pipeline = bundle.external_reduce_pipeline
	_external_finalize_shader = bundle.external_finalize_shader
	_external_finalize_pipeline = bundle.external_finalize_pipeline
	_params = bundle.params; _control = bundle.control
	_atmospheric_sources = bundle.atmospheric_sources
	_external_flux_ledger = bundle.external_flux_ledger
	_external_flux_partials = bundle.external_flux_partials
	_commit_params = bundle.commit_params
	_step_set = bundle.step_set; _commit_set = bundle.commit_set
	_reduce_set = bundle.reduce_set; _reset_set = bundle.reset_set
	_prepare_set = bundle.prepare_set
	_external_reduce_set = bundle.external_reduce_set
	_external_finalize_set = bundle.external_finalize_set
	_initialized = true
	initialized.emit()


func _advance_render_thread(step_id: int, cap: int, request_diagnostics: bool,
		param_bytes: PackedByteArray) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null or not _all_runtime_rids_valid():
		call_deferred("_finish_advance", step_id, false, request_diagnostics)
		return
	var err := rd.buffer_update(_params, 0, param_bytes.size(), param_bytes)
	var external_ledger_bytes := _atlas.total_cell_count() * EXTERNAL_LEDGER_FLOATS * 4
	if err != OK or rd.buffer_clear(_control, 0, CONTROL_BYTES) != OK \
			or rd.buffer_clear(_external_flux_ledger, 0, external_ledger_bytes) != OK:
		call_deferred("_finish_advance", step_id, false, request_diagnostics)
		return

	var groups_x := int(ceil(float(_atlas.tile_resolution) / float(LOCAL_X)))
	var groups_y := int(ceil(float(_atlas.tile_resolution) / float(LOCAL_Y)))
	var commit_groups := int(ceil(float(_atlas.total_cell_count()) / float(COMMIT_LOCAL_X)))
	var external_groups := int(ceil(float(_atlas.total_cell_count()) \
		/ float(EXTERNAL_REDUCE_LOCAL_X)))
	var compute := rd.compute_list_begin()

	for iteration in cap:
		rd.compute_list_bind_compute_pipeline(compute, _reset_pipeline)
		rd.compute_list_bind_uniform_set(compute, _reset_set, 0)
		rd.compute_list_dispatch(compute, 1, 1, 1)
		rd.compute_list_add_barrier(compute)

		rd.compute_list_bind_compute_pipeline(compute, _reduce_pipeline)
		rd.compute_list_bind_uniform_set(compute, _reduce_set, 0)
		_set_u32_push(rd, compute, 0)
		rd.compute_list_dispatch(compute, groups_x, groups_y, _atlas.capacity)
		rd.compute_list_add_barrier(compute)

		rd.compute_list_bind_compute_pipeline(compute, _prepare_pipeline)
		rd.compute_list_bind_uniform_set(compute, _prepare_set, 0)
		_set_u32_push(rd, compute, iteration)
		rd.compute_list_dispatch(compute, 1, 1, 1)
		rd.compute_list_add_barrier(compute)

		rd.compute_list_bind_compute_pipeline(compute, _step_pipeline)
		rd.compute_list_bind_uniform_set(compute, _step_set, 0)
		_set_u32_push(rd, compute, iteration)
		rd.compute_list_dispatch(compute, groups_x, groups_y, _atlas.capacity)
		rd.compute_list_add_barrier(compute)

		rd.compute_list_bind_compute_pipeline(compute, _commit_pipeline)
		rd.compute_list_bind_uniform_set(compute, _commit_set, 0)
		rd.compute_list_dispatch(compute, commit_groups, 1, 1)
		rd.compute_list_add_barrier(compute)

	# Final physical-state health reduction.
	rd.compute_list_bind_compute_pipeline(compute, _reduce_pipeline)
	rd.compute_list_bind_uniform_set(compute, _reduce_set, 0)
	_set_u32_push(rd, compute, 1)
	rd.compute_list_dispatch(compute, groups_x, groups_y, _atlas.capacity)
	rd.compute_list_add_barrier(compute)

	# Exact external source ledger -> the established words at bytes 88/92.
	rd.compute_list_bind_compute_pipeline(compute, _external_reduce_pipeline)
	rd.compute_list_bind_uniform_set(compute, _external_reduce_set, 0)
	rd.compute_list_dispatch(compute, external_groups, 1, 1)
	rd.compute_list_add_barrier(compute)
	rd.compute_list_bind_compute_pipeline(compute, _external_finalize_pipeline)
	rd.compute_list_bind_uniform_set(compute, _external_finalize_set, 0)
	rd.compute_list_dispatch(compute, 1, 1, 1)
	rd.compute_list_end()

	if request_diagnostics:
		var callback := Callable(self, &"_on_diagnostics_bytes").bind(step_id)
		err = rd.buffer_get_data_async(_control, callback, 0, CONTROL_BYTES)
		if err != OK:
			call_deferred("_diagnostics_failed", step_id, err)
	call_deferred("_finish_advance", step_id, true, request_diagnostics)


func _clear_atmospheric_sources_render_thread() -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null or not _atmospheric_sources.is_valid() or _atlas == null:
		return
	var bytes := _atlas.total_cell_count() * SOURCE_FLOATS * 4
	var err := rd.buffer_clear(_atmospheric_sources, 0, bytes)
	if err != OK:
		push_warning("SparseHydroStepGPU: atmospheric source clear failed (%d)." % int(err))


func _finish_advance(step_id: int, success: bool, request_diagnostics: bool) -> void:
	_advance_pending = false
	if not success:
		if request_diagnostics:
			_diagnostics_pending = false
		push_error("SparseHydroStepGPU: failed to record sparse advance %d" % step_id)
		return
	substep_recorded.emit(step_id)
	advance_recorded.emit(step_id)


func _on_diagnostics_bytes(bytes: PackedByteArray, step_id: int) -> void:
	if bytes.size() < CONTROL_BYTES:
		call_deferred("_diagnostics_failed", step_id, ERR_INVALID_DATA)
		return
	var steps := int(bytes.decode_u32(56))
	var added_m3 := maxf(bytes.decode_float(88), 0.0)
	var removed_m3 := maxf(bytes.decode_float(92), 0.0)
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
		"external_added_m3": added_m3,
		"external_removed_m3": removed_m3,
		"external_net_m3": added_m3 - removed_m3,
		"external_sink_clipping_exact": true,
		"hydrolod_cfl": _atlas != null and _atlas.hydrolod_enabled(),
	}
	call_deferred("_publish_diagnostics", step_id, diagnostics)


func _publish_diagnostics(step_id: int, diagnostics: Dictionary) -> void:
	_diagnostics_pending = false
	_latest_diagnostics = diagnostics
	diagnostics_ready.emit(step_id, diagnostics)


func _diagnostics_failed(step_id: int, error: Error) -> void:
	_diagnostics_pending = false
	push_warning("SparseHydroStepGPU: diagnostics for advance %d failed (%d)." % [
		step_id, int(error)])


func _set_u32_push(rd: RenderingDevice, compute: int, value: int) -> void:
	var push := PackedInt32Array([value, 0, 0, 0]).to_byte_array()
	rd.compute_list_set_push_constant(compute, push, push.size())


func _all_runtime_rids_valid() -> bool:
	return _step_pipeline.is_valid() and _commit_pipeline.is_valid() \
		and _reduce_pipeline.is_valid() and _reset_pipeline.is_valid() \
		and _prepare_pipeline.is_valid() and _external_reduce_pipeline.is_valid() \
		and _external_finalize_pipeline.is_valid() and _params.is_valid() \
		and _commit_params.is_valid() and _control.is_valid() \
		and _atmospheric_sources.is_valid() and _external_flux_ledger.is_valid() \
		and _external_flux_partials.is_valid() \
		and _step_set.is_valid() and _commit_set.is_valid() \
		and _reduce_set.is_valid() and _reset_set.is_valid() \
		and _prepare_set.is_valid() and _external_reduce_set.is_valid() \
		and _external_finalize_set.is_valid()


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
	if not _initialized and not _step_shader.is_valid():
		return
	var rids := [
		_step_set, _commit_set, _reduce_set, _reset_set, _prepare_set,
		_external_reduce_set, _external_finalize_set,
		_params, _commit_params, _control, _atmospheric_sources,
		_external_flux_ledger, _external_flux_partials,
		_step_pipeline, _commit_pipeline, _reduce_pipeline, _reset_pipeline,
		_prepare_pipeline, _external_reduce_pipeline, _external_finalize_pipeline,
		_step_shader, _commit_shader, _reduce_shader, _reset_shader, _prepare_shader,
		_external_reduce_shader, _external_finalize_shader,
	]
	_initialized = false
	_init_pending = false
	_advance_pending = false
	_diagnostics_pending = false
	_step_set = RID(); _commit_set = RID(); _reduce_set = RID()
	_reset_set = RID(); _prepare_set = RID()
	_external_reduce_set = RID(); _external_finalize_set = RID()
	_params = RID(); _commit_params = RID(); _control = RID()
	_atmospheric_sources = RID(); _external_flux_ledger = RID()
	_external_flux_partials = RID()
	_step_pipeline = RID(); _commit_pipeline = RID(); _reduce_pipeline = RID()
	_reset_pipeline = RID(); _prepare_pipeline = RID()
	_external_reduce_pipeline = RID(); _external_finalize_pipeline = RID()
	_step_shader = RID(); _commit_shader = RID(); _reduce_shader = RID()
	_reset_shader = RID(); _prepare_shader = RID()
	_external_reduce_shader = RID(); _external_finalize_shader = RID()
	_atlas = null
	_connectivity = null
	RenderingServer.call_on_render_thread(
		Callable(self, &"_release_render_thread").bind(rids))
	released.emit()


func _release_render_thread(rids: Array) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd != null:
		_free_many(rd, rids)


func _exit_tree() -> void:
	release()
