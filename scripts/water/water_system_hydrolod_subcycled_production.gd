extends "res://scripts/water/water_system_hydrolod_production.gd"
## Final Phase-4 production facade with temporal HydroLOD subcycling.
##
## All river/component ownership, spatial HydroLOD transfer and 2:1 topology logic
## remain inherited. This layer selects SparseHydrologyRuntimeSubcycled.


func gpu_stats() -> Dictionary:
	var out := super.gpu_stats()
	var physical: Dictionary = out.get("physical_hydrolod", {})
	physical["temporal_subcycling"] = _sparse_runtime is SparseHydrologyRuntimeSubcycled
	physical["binary_ratios"] = "H0:1,H1:2,H2:4,H3:8,H4:16"
	physical["fine_clock_cfl_normalization"] = true
	physical["coarse_fine_flux_registers"] = true
	physical["synchronizes_at_advance_boundary"] = true
	out["physical_hydrolod"] = physical
	return out


func _on_sparse_connectivity_initialized(generation: int,
		connectivity: SparseHydroConnectivityGPU) -> void:
	if not _sparse_generation_matches(generation) or connectivity != _sparse_connectivity:
		return
	var sync_error := connectivity.sync_pool(_sparse_scheduler.pool)
	if sync_error != OK:
		_fail_sparse_bootstrap(sync_error, "connectivity_bootstrap_sync")
		return

	_sparse_reachability = HydroReachabilityService.new()
	var reachability_error := _sparse_reachability.initialize(_sparse_atlas)
	if reachability_error != OK:
		_fail_sparse_bootstrap(reachability_error, "reachability")
		return
	if _structure_crest_provider.is_valid():
		_sparse_reachability.set_structure_crest_provider(_structure_crest_provider)

	var runtime := SparseHydrologyRuntimeSubcycled.new()
	runtime.name = "SparseHydrologyRuntime"
	runtime.process_priority = 12
	runtime.auto_run = true
	runtime.macro_dt_s = maxf(sparse_macro_dt_s, 1.0e-5)
	runtime.max_time_debt_s = maxf(sparse_max_time_debt_s, runtime.macro_dt_s)
	runtime.max_gpu_substeps = clampi(sparse_max_gpu_substeps, 1,
		SparseHydroStepGPU.MAX_GPU_SUBSTEPS)
	runtime.maximum_physical_lod = maximum_physical_hydrolod
	_sparse_runtime = runtime
	add_child(runtime)
	runtime.initialized.connect(func(): _on_sparse_runtime_initialized(generation, runtime))
	runtime.initialization_failed.connect(func(error: Error, component: String):
		_on_sparse_runtime_init_failed(generation, runtime, error, component))
	runtime.runtime_failed.connect(func(error: Error, stage: String):
		_on_sparse_runtime_failed(generation, runtime, error, stage))
	runtime.hydrolod_ready.connect(func():
		if _sparse_generation_matches(generation) and runtime == _sparse_runtime:
			physical_hydrolod_ready.emit())
	runtime.hydrolod_transition_completed.connect(func(report: Dictionary):
		if runtime == _sparse_runtime:
			physical_hydrolod_transition_completed.emit(report.duplicate(true)))
	runtime.hydrolod_transition_failed.connect(
		func(error: Error, stage: String, recovery: String):
			if runtime == _sparse_runtime:
				physical_hydrolod_transition_failed.emit(error, stage, recovery))
	_set_sparse_state("initializing_runtime")
	var runtime_error := runtime.initialize(_sparse_scheduler, _sparse_atlas,
		_sparse_connectivity, _sparse_identity,
		Callable(_sparse_reachability, &"can_enter"))
	if runtime_error != OK:
		_fail_sparse_bootstrap(runtime_error, "runtime_submit")
