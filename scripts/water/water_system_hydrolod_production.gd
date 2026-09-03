extends "res://scripts/water/water_system_river_cluster_production.gd"
## Phase-4 production facade: conservative physical HydroLOD layered on top of the
## complete Phase-3 river/component stack.
##
## Mixed-resolution physical metrics, conservative parent/children transfers and
## live 2:1 interface reflux are active. The runtime enforces a strict 2:1 balance;
## unsupported >2:1 boundaries remain fail-closed until temporal subcycling exists.

signal physical_hydrolod_ready
signal physical_hydrolod_transition_completed(report: Dictionary)
signal physical_hydrolod_transition_failed(error: Error, stage: String,
	recovery: String)

var maximum_physical_hydrolod := 4


func physical_hydrolod_available() -> bool:
	return _sparse_runtime is SparseHydrologyRuntimeLOD \
		and (_sparse_runtime as SparseHydrologyRuntimeLOD).hydrolod_available()


func physical_hydrolod_coarsen_eligibility(parent_tile_id: int) -> Dictionary:
	if not physical_hydrolod_available():
		return {"error": ERR_UNCONFIGURED, "eligible": false,
			"reason": "hydrolod_unavailable"}
	var key := HydroTileKey.unpack(parent_tile_id)
	if key == null:
		return {"error": ERR_INVALID_PARAMETER, "eligible": false,
			"reason": "invalid_parent_tile_id"}
	return (_sparse_runtime as SparseHydrologyRuntimeLOD) \
		.hydrolod_coarsen_eligibility(key)


func physical_hydrolod_refine_eligibility(parent_tile_id: int) -> Dictionary:
	if not physical_hydrolod_available():
		return {"error": ERR_UNCONFIGURED, "eligible": false,
			"reason": "hydrolod_unavailable"}
	var key := HydroTileKey.unpack(parent_tile_id)
	if key == null:
		return {"error": ERR_INVALID_PARAMETER, "eligible": false,
			"reason": "invalid_parent_tile_id"}
	return (_sparse_runtime as SparseHydrologyRuntimeLOD) \
		.hydrolod_refine_eligibility(key)


func coarsen_sparse_hydrolod(parent_tile_id: int) -> int:
	if _representation_bridge_busy():
		return -1
	var key := HydroTileKey.unpack(parent_tile_id)
	if key == null or not physical_hydrolod_available():
		return -1
	return (_sparse_runtime as SparseHydrologyRuntimeLOD).coarsen_hydrolod(key)


func refine_sparse_hydrolod(parent_tile_id: int) -> int:
	if _representation_bridge_busy():
		return -1
	var key := HydroTileKey.unpack(parent_tile_id)
	if key == null or not physical_hydrolod_available():
		return -1
	return (_sparse_runtime as SparseHydrologyRuntimeLOD).refine_hydrolod(key)


func gpu_stats() -> Dictionary:
	var out := super.gpu_stats()
	out["phase"] = 4
	out["physical_hydrolod"] = {
		"available": physical_hydrolod_available(),
		"maximum_lod": maximum_physical_hydrolod,
		"mixed_level_metrics": true,
		"cfl_uses_local_dx": true,
		"source_volume_uses_local_area": true,
		"activity_flux_uses_local_dx": true,
		"point_sources_follow_active_lod_owner": true,
		"conservative_restriction": true,
		"terrain_aware_conservative_prolongation": true,
		"atomic_hierarchy_swap": true,
		"river_member_transition_guard": true,
		"cross_lod_interface_reflux": true,
		"cross_lod_frontier_claims": true,
		"two_to_one_balance_enforced": true,
		"temporal_subcycling": false,
		"runtime": {} if not (_sparse_runtime is SparseHydrologyRuntimeLOD) \
			else (_sparse_runtime as SparseHydrologyRuntimeLOD).stats().get(
				"physical_hydrolod", {}),
	}
	return out


## Atlas initialization finishes before identity/connectivity/runtime bootstrap. Set
## the metric level here so every later GPU component sees H0 consistently.
func _on_sparse_atlas_initialized(generation: int,
		atlas: SparseHydroAtlasGPU) -> void:
	if not _sparse_generation_matches(generation) or atlas != _sparse_atlas:
		return
	var base_level := int(_sparse_metric_contract.get("level", -1))
	var metric_error := atlas.set_base_tile_level(base_level)
	if metric_error != OK:
		_fail_sparse_bootstrap(metric_error, "hydrolod_metric_contract")
		return
	super._on_sparse_atlas_initialized(generation, atlas)


## Phase-3 base builds the stable reachability service here. This override keeps
## that exact bootstrap contract but selects SparseHydrologyRuntimeLOD.
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

	var runtime := SparseHydrologyRuntimeLOD.new()
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


func _on_sparse_runtime_initialized(generation: int,
		runtime: SparseHydrologyRuntime) -> void:
	if not _sparse_generation_matches(generation) or runtime != _sparse_runtime:
		return
	if runtime is SparseHydrologyRuntimeLOD:
		var guard_error := (runtime as SparseHydrologyRuntimeLOD) \
			.set_hydrolod_transition_guard(Callable(self, &"_hydrolod_transition_guard"))
		if guard_error != OK:
			_fail_sparse_bootstrap(guard_error, "hydrolod_transition_guard")
			return
	super._on_sparse_runtime_initialized(generation, runtime)


## River/component sparse members have independent coarse/fine ownership contracts;
## a generic HydroLOD swap must never move those tiles behind their bridges. This
## reads the already-existing refined record registry; no parallel ownership table
## is introduced for the guard.
func _hydrolod_transition_guard(_mode: String, parent: HydroTileKey,
		children: Array[HydroTileKey]) -> Dictionary:
	if parent == null:
		return {"error": ERR_INVALID_PARAMETER, "allowed": false}
	if not PersistentHydrologySystem.available() \
			or not (PersistentHydrologySystem.store() is PlanetHydrologyRiverClusterStore):
		return {"error": OK, "allowed": true}
	var involved: Dictionary = {parent.packed(): true}
	for child in children:
		if child != null:
			involved[child.packed()] = true
	var store := PersistentHydrologySystem.store() as PlanetHydrologyRiverClusterStore
	var records_value: Variant = store.get("_refined_records")
	if not (records_value is Dictionary):
		# An ownership registry we cannot inspect is not permission to move water.
		return {"error": ERR_INVALID_DATA, "allowed": false,
			"reason": "river_refinement_registry_unavailable"}
	for record_value: Variant in (records_value as Dictionary).values():
		if not (record_value is Dictionary):
			continue
		var record := record_value as Dictionary
		var members_value: Variant = record.get("members", null)
		if members_value is Array and not (members_value as Array).is_empty():
			for member_value: Variant in members_value:
				if not (member_value is Dictionary):
					continue
				var tile_id := int((member_value as Dictionary).get("tile_id", -1))
				if involved.has(tile_id):
					return {"error": ERR_BUSY, "allowed": false,
						"reason": "river_refinement_tile_pinned", "tile_id": tile_id}
		else:
			var tile_id := int(record.get("tile_id", -1))
			if involved.has(tile_id):
				return {"error": ERR_BUSY, "allowed": false,
					"reason": "river_refinement_tile_pinned", "tile_id": tile_id}
	return {"error": OK, "allowed": true}


func _representation_bridge_busy() -> bool:
	if _component_bridge_busy():
		return true
	if _river_cluster_collapse != null and _river_cluster_collapse.busy():
		return true
	if _river_cluster_promotion != null and _river_cluster_promotion.busy():
		return true
	if _river_reach_collapse_bridge != null and _river_reach_collapse_bridge.busy():
		return true
	if _river_reach_coupling != null and _river_reach_coupling.pending():
		return true
	return false
