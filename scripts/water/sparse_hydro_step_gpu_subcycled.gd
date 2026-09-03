class_name SparseHydroStepGPUSubcycled
extends SparseHydroStepGPULOD
## Phase-4 temporal HydroLOD solver.
##
## The first 104 control bytes retain the existing diagnostics/CFL ABI. This solver
## upgrades only its own control buffer to 184 bytes, appending fine-clock schedule
## state. Existing Phase-3 and spatial-only Phase-4 solvers remain unchanged.

const SUBCYCLED_CONTROL_BYTES := 184
const BASE_CONTROL_BYTES := 104

var maximum_physical_lod := 4
var temporal_schedule: HydroLODTemporalScheduleGPU


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
		"step": _load_spirv("res://shaders/water/sparse_hydro_step_subcycled.glsl"),
		"commit": _load_spirv("res://shaders/water/sparse_hydro_commit.glsl"),
		"reduce": _load_spirv("res://shaders/water/sparse_hydro_reduce_subcycled.glsl"),
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


func set_temporal_schedule(provider: HydroLODTemporalScheduleGPU) -> Error:
	if step_pending() or diagnostics_pending():
		return ERR_BUSY
	if provider != null and not provider.initialized_ok():
		return ERR_UNCONFIGURED
	temporal_schedule = provider
	return OK


func advance(dt_s: float, max_substeps: int = 16,
		request_diagnostics: bool = true) -> int:
	if not _initialized or _advance_pending or _diagnostics_pending \
			or not is_finite(dt_s) or dt_s <= 0.0:
		return -1
	if _atlas == null or _connectivity == null \
			or not _atlas.initialized_ok() or not _connectivity.initialized_ok() \
			or temporal_schedule == null or not temporal_schedule.initialized_ok():
		return -1
	var cap := clampi(max_substeps, 1, MAX_GPU_SUBSTEPS)
	var step_id := _next_step_id
	_next_step_id += 1
	_advance_pending = true
	_diagnostics_pending = request_diagnostics
	var lod_enabled := 1.0 if _atlas.hydrolod_enabled() else 0.0
	var base_level := float(maxi(_atlas.base_tile_level, 0))
	var temporal_encoded := float(clampi(maximum_physical_lod, 0, 7) + 1)
	var params := PackedFloat32Array([
		float(_atlas.tile_resolution), float(_atlas.capacity),
		_atlas.cell_size_m, dt_s,
		gravity, dry_eps, manning_n, clampf(cfl, 0.01, 0.95),
		float(cap), base_level, lod_enabled, temporal_encoded,
	])
	RenderingServer.call_on_render_thread(Callable(self, &"_advance_render_thread").bind(
		step_id, cap, request_diagnostics, params.to_byte_array()))
	return step_id


func gpu_bytes_estimate() -> int:
	return super.gpu_bytes_estimate() + (SUBCYCLED_CONTROL_BYTES - BASE_CONTROL_BYTES)


func stats() -> Dictionary:
	var out := super.stats()
	out["temporal_subcycling"] = temporal_schedule != null \
		and temporal_schedule.initialized_ok()
	out["maximum_physical_lod"] = maximum_physical_lod
	out["fine_clock_cfl_normalization"] = true
	out["synchronizes_on_advance_end"] = true
	out["control_bytes"] = SUBCYCLED_CONTROL_BYTES
	out["temporal_schedule"] = {} if temporal_schedule == null \
		else temporal_schedule.stats()
	return out


## Base initialization intentionally creates its normal 104-byte control block.
## Before publishing initialized(), replace only that block and the uniform sets
## which reference it. All expensive pipelines/atlas resources are retained.
func _finish_init(error: Error, bundle: Dictionary) -> void:
	if error != OK:
		_init_pending = false
		_initialized = false
		initialization_failed.emit(error)
		return
	RenderingServer.call_on_render_thread(
		Callable(self, &"_upgrade_control_render_thread").bind(bundle))


func _upgrade_control_render_thread(bundle: Dictionary) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null:
		call_deferred(&"_finish_subcycled_init", ERR_UNAVAILABLE, {})
		return
	var zero := PackedByteArray()
	zero.resize(SUBCYCLED_CONTROL_BYTES)
	var control := rd.storage_buffer_create(zero.size(), zero)
	if not control.is_valid():
		_free_init_bundle(rd, bundle)
		call_deferred(&"_finish_subcycled_init", ERR_CANT_CREATE, {})
		return

	var step_set := rd.uniform_set_create([
		_storage_uniform(0, _atlas.state_a_rid()),
		_storage_uniform(1, _atlas.state_b_rid()),
		_storage_uniform(2, _atlas.source_rid()),
		_storage_uniform(3, _atlas.occupancy_rid()),
		_storage_uniform(4, _connectivity.neighbor_slots_rid()),
		_storage_uniform(5, _connectivity.neighbor_links_rid()),
		_storage_uniform(6, bundle["params"]),
		_storage_uniform(7, control),
		_storage_uniform(8, bundle["atmospheric_sources"]),
		_storage_uniform(9, bundle["external_flux_ledger"]),
		_storage_uniform(10, _atlas.tile_metadata_rid()),
	], bundle["step_shader"], 0)
	var reduce_set := rd.uniform_set_create([
		_storage_uniform(0, _atlas.state_a_rid()),
		_storage_uniform(1, _atlas.occupancy_rid()),
		_storage_uniform(2, control),
		_storage_uniform(3, bundle["params"]),
		_storage_uniform(4, _atlas.tile_metadata_rid()),
	], bundle["reduce_shader"], 0)
	var reset_set := rd.uniform_set_create([
		_storage_uniform(0, control),
	], bundle["reset_shader"], 0)
	var prepare_set := rd.uniform_set_create([
		_storage_uniform(0, control),
		_storage_uniform(1, bundle["params"]),
	], bundle["prepare_shader"], 0)
	var external_finalize_set := rd.uniform_set_create([
		_storage_uniform(0, bundle["external_flux_partials"]),
		_storage_uniform(1, control),
		_storage_uniform(2, bundle["commit_params"]),
	], bundle["external_finalize_shader"], 0)
	var replacements := [step_set, reduce_set, reset_set, prepare_set, external_finalize_set]
	for rid in replacements:
		if not (rid is RID) or not (rid as RID).is_valid():
			_free_many(rd, replacements + [control])
			_free_init_bundle(rd, bundle)
			call_deferred(&"_finish_subcycled_init", ERR_CANT_CREATE, {})
			return

	_free_many(rd, [bundle["step_set"], bundle["reduce_set"], bundle["reset_set"],
		bundle["prepare_set"], bundle["external_finalize_set"], bundle["control"]])
	bundle["control"] = control
	bundle["step_set"] = step_set
	bundle["reduce_set"] = reduce_set
	bundle["reset_set"] = reset_set
	bundle["prepare_set"] = prepare_set
	bundle["external_finalize_set"] = external_finalize_set
	call_deferred(&"_finish_subcycled_init", OK, bundle)


func _finish_subcycled_init(error: Error, bundle: Dictionary) -> void:
	_init_pending = false
	if error != OK:
		_initialized = false
		initialization_failed.emit(error)
		return
	_step_shader = bundle["step_shader"]; _step_pipeline = bundle["step_pipeline"]
	_commit_shader = bundle["commit_shader"]; _commit_pipeline = bundle["commit_pipeline"]
	_reduce_shader = bundle["reduce_shader"]; _reduce_pipeline = bundle["reduce_pipeline"]
	_reset_shader = bundle["reset_shader"]; _reset_pipeline = bundle["reset_pipeline"]
	_prepare_shader = bundle["prepare_shader"]; _prepare_pipeline = bundle["prepare_pipeline"]
	_external_reduce_shader = bundle["external_reduce_shader"]
	_external_reduce_pipeline = bundle["external_reduce_pipeline"]
	_external_finalize_shader = bundle["external_finalize_shader"]
	_external_finalize_pipeline = bundle["external_finalize_pipeline"]
	_params = bundle["params"]; _control = bundle["control"]
	_atmospheric_sources = bundle["atmospheric_sources"]
	_external_flux_ledger = bundle["external_flux_ledger"]
	_external_flux_partials = bundle["external_flux_partials"]
	_commit_params = bundle["commit_params"]
	_step_set = bundle["step_set"]; _commit_set = bundle["commit_set"]
	_reduce_set = bundle["reduce_set"]; _reset_set = bundle["reset_set"]
	_prepare_set = bundle["prepare_set"]
	_external_reduce_set = bundle["external_reduce_set"]
	_external_finalize_set = bundle["external_finalize_set"]
	_initialized = true
	initialized.emit()


func _free_init_bundle(rd: RenderingDevice, bundle: Dictionary) -> void:
	var keys := [
		"step_set", "commit_set", "reduce_set", "reset_set", "prepare_set",
		"external_reduce_set", "external_finalize_set",
		"params", "control", "atmospheric_sources", "external_flux_ledger",
		"external_flux_partials", "commit_params",
		"step_pipeline", "commit_pipeline", "reduce_pipeline", "reset_pipeline",
		"prepare_pipeline", "external_reduce_pipeline", "external_finalize_pipeline",
		"step_shader", "commit_shader", "reduce_shader", "reset_shader",
		"prepare_shader", "external_reduce_shader", "external_finalize_shader",
	]
	var values: Array = []
	for key in keys:
		if bundle.has(key):
			values.append(bundle[key])
	_free_many(rd, values)


func _advance_render_thread(step_id: int, cap: int, request_diagnostics: bool,
		param_bytes: PackedByteArray) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null or not _all_runtime_rids_valid() \
			or temporal_schedule == null or not temporal_schedule.initialized_ok():
		call_deferred("_finish_advance", step_id, false, request_diagnostics)
		return
	if lod_interface_flux != null and not lod_interface_flux.initialized_ok():
		call_deferred("_finish_advance", step_id, false, request_diagnostics)
		return
	var err := rd.buffer_update(_params, 0, param_bytes.size(), param_bytes)
	var external_ledger_bytes := _atlas.total_cell_count() * EXTERNAL_LEDGER_FLOATS * 4
	if err != OK or rd.buffer_clear(_control, 0, SUBCYCLED_CONTROL_BYTES) != OK \
			or rd.buffer_clear(_external_flux_ledger, 0, external_ledger_bytes) != OK:
		call_deferred("_finish_advance", step_id, false, request_diagnostics)
		return
	if lod_interface_flux is HydroLODInterfaceFluxSubcycledGPU:
		err = (lod_interface_flux as HydroLODInterfaceFluxSubcycledGPU) \
			.clear_flux_register(rd)
		if err != OK:
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

		temporal_schedule.record_prepare(rd, compute, iteration)

		rd.compute_list_bind_compute_pipeline(compute, _step_pipeline)
		rd.compute_list_bind_uniform_set(compute, _step_set, 0)
		_set_u32_push(rd, compute, iteration)
		rd.compute_list_dispatch(compute, groups_x, groups_y, _atlas.capacity)
		rd.compute_list_add_barrier(compute)

		if lod_interface_flux != null:
			lod_interface_flux.record_corrections(rd, compute)

		rd.compute_list_bind_compute_pipeline(compute, _commit_pipeline)
		rd.compute_list_bind_uniform_set(compute, _commit_set, 0)
		rd.compute_list_dispatch(compute, commit_groups, 1, 1)
		rd.compute_list_add_barrier(compute)

		temporal_schedule.record_commit(rd, compute)

	rd.compute_list_bind_compute_pipeline(compute, _reduce_pipeline)
	rd.compute_list_bind_uniform_set(compute, _reduce_set, 0)
	_set_u32_push(rd, compute, 1)
	rd.compute_list_dispatch(compute, groups_x, groups_y, _atlas.capacity)
	rd.compute_list_add_barrier(compute)

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
		err = rd.buffer_get_data_async(_control, callback, 0, SUBCYCLED_CONTROL_BYTES)
		if err != OK:
			call_deferred("_diagnostics_failed", step_id, err)
	call_deferred("_finish_advance", step_id, true, request_diagnostics)


func release() -> void:
	temporal_schedule = null
	super.release()
