class_name SparseHydrologyRuntimeSubcycledCompacted
extends SparseHydrologyRuntimeSubcycled
## Production temporal HydroLOD runtime with compact activity/frontier queues.
##
## The parent still owns solver/cache/LOD/source orchestration. This layer replaces
## the parent's not-yet-initialized readback-only activity facade once the fused cache
## exists, then builds frontier directly over the dense active-record queue.


func hydrolod_available() -> bool:
	return super.hydrolod_available() \
		and activity is HydroTileActivityCompactedGPU \
		and frontier is HydroFrontierCandidatesCompactedGPU


func stats() -> Dictionary:
	var out := super.stats()
	var physical: Dictionary = out.get("physical_hydrolod", {})
	physical["activity_cpu_readback_compacted"] = activity \
		is HydroTileActivityCompactedGPU
	physical["full_capacity_activity_readback"] = false
	physical["frontier_active_record_indirect_dispatch"] = frontier \
		is HydroFrontierCandidatesCompactedGPU
	physical["frontier_full_capacity_summary_scan"] = false
	physical["frontier_exact_candidate_readback"] = frontier \
		is HydroFrontierCandidatesCompactedGPU
	physical["compacted_activity"] = {} if not (activity is HydroTileActivityCompactedGPU) \
		else (activity as HydroTileActivityCompactedGPU).compacted_stats()
	physical["compacted_frontier"] = {} \
		if not (frontier is HydroFrontierCandidatesCompactedGPU) \
		else (frontier as HydroFrontierCandidatesCompactedGPU).compacted_stats()
	out["physical_hydrolod"] = physical
	return out


## Parent solver initialization arrives here after the fused cache has created its
## canonical activity buffer. Replace the placeholder cached facade before binding it.
func _on_cfl_cache_initialized() -> void:
	if not (solver is SparseHydroStepGPUSubcycledCached) or cfl_cache == null:
		_fail_initialization(ERR_UNCONFIGURED, "hydrolod_cfl_cache_bind")
		return
	var cached_solver := solver as SparseHydroStepGPUSubcycledCached
	var err := cached_solver.set_cfl_cache(cfl_cache)
	if err != OK:
		_fail_initialization(err, "hydrolod_cfl_cache_bind")
		return
	cached_solver.set_cfl_cache_topology_revision(scheduler.pool.topology_revision)
	_component_ready["solver"] = true
	_component_ready["cfl_cache"] = true

	var old_activity := activity
	activity = null
	if old_activity != null and is_instance_valid(old_activity):
		old_activity.release()
		old_activity.queue_free()

	var compacted := HydroTileActivityCompactedGPU.new()
	compacted.name = "HydroTileActivityGPU"
	activity = compacted
	add_child(activity)
	activity.initialized.connect(_on_activity_initialized)
	activity.initialization_failed.connect(
		func(error: Error): _fail_initialization(error, "activity"))
	activity.summaries_ready.connect(_on_activity_summaries)
	activity.classification_failed.connect(_on_activity_failed)
	err = compacted.initialize_compacted(cfl_cache.activity_summary_rid(), atlas.capacity)
	if err != OK:
		_fail_initialization(err, "hydrolod_compacted_activity_bind")
		return

	_try_initialize_temporal_schedule()
	_try_initialize_lod_interfaces()
	_try_initialize_lod_manager()
	_try_finish_initialization()


## Use the dense activity queue and its GPU indirect count instead of dispatching
## frontier generation over atlas capacity.
func _on_activity_initialized() -> void:
	if not (activity is HydroTileActivityCompactedGPU):
		_fail_initialization(ERR_UNCONFIGURED, "hydrolod_compacted_activity_type")
		return
	_component_ready["activity"] = true
	var compacted := activity as HydroTileActivityCompactedGPU
	var compact_frontier := HydroFrontierCandidatesCompactedGPU.new()
	compact_frontier.name = "HydroFrontierCandidatesGPU"
	frontier = compact_frontier
	add_child(frontier)
	frontier.initialized.connect(_on_frontier_initialized)
	frontier.initialization_failed.connect(
		func(error: Error): _fail_initialization(error, "frontier"))
	frontier.candidates_ready.connect(_on_frontier_candidates)
	frontier.queue_failed.connect(_on_frontier_failed)
	var err := compact_frontier.initialize_compacted(
		compacted.compact_queue_rid(), compacted.compact_indirect_rid(),
		atlas.tile_metadata_rid(), atlas.capacity, scheduler.wake_flux_threshold_m3s)
	if err != OK:
		_fail_initialization(err, "frontier")
