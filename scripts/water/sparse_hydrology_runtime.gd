class_name SparseHydrologyRuntime
extends Node
## Persistent Phase 3 sparse-hydrology orchestration loop.
##
## The runtime owns no stable world identity and no atlas storage. Those remain in
## SparseHydroScheduler/SparseHydroAtlasGPU. This node sequences compact source
## synchronization and the validated GPU solve/frontier chain while carrying any
## unadvanced CFL time debt between cycles:
##
##   source sync -> advance -> diagnostics -> classify -> frontier -> policy
##               -> terrain stage/handoff -> connectivity -> repeat
##
## Full h/hu/hv/bed grids remain GPU-resident.

signal initialized
signal initialization_failed(error: Error, component: String)
signal cycle_started(cycle_id: int, requested_dt_s: float, time_debt_s: float)
signal cycle_completed(cycle_id: int, report: Dictionary)
signal frontier_overflow(cycle_id: int)
signal source_sync_completed(source_count: int, entry_count: int)
signal runtime_failed(error: Error, stage: String)
signal released

enum Phase {
	OFFLINE,
	INITIALIZING,
	IDLE,
	SOURCES,
	ADVANCING,
	CLASSIFYING,
	FRONTIER,
	ACTIVATING,
	FAILED,
}

var auto_run := true
var enabled := true
var time_scale := 1.0
var macro_dt_s := 0.05
var max_time_debt_s := 0.50
var min_cycle_dt_s := 1.0e-5
var max_gpu_substeps := 16
var seed_dt_s := 0.02
var max_seed_fraction := 0.12
var gravity := 9.81
## FROZEN_WATER is currently a policy label only; GPU occupancy remains live.
## Keep it disabled until analytical-domain collapse can really remove the tile.
var disable_placeholder_wet_freeze := true

var scheduler: SparseHydroScheduler
var atlas: SparseHydroAtlasGPU
var connectivity: SparseHydroConnectivityGPU
var identity_bridge: SparseHydroIdentityBridge

var solver: SparseHydroStepGPU
var activity: HydroTileActivityGPU
var frontier: HydroFrontierCandidatesGPU
var activation: HydroFrontierActivationPipeline
var terrain_bed: HydroTerrainBedGPU
var source_ingress: HydroSourceIngress

var _phase := Phase.OFFLINE
var _reachability := Callable()
var _destination_provider := Callable()
var _using_internal_terrain_provider := false
var _component_ready: Dictionary = {}

var _time_debt_s := 0.0
var _next_cycle_id := 1
var _cycle_id := -1
var _cycle_requested_dt_s := 0.0
var _cycle_advanced_dt_s := 0.0
var _cycle_solver_step_id := -1
var _cycle_activity_request_id := -1
var _cycle_frontier_request_id := -1
var _cycle_activation_batch_id := -1
var _cycle_summaries: Array[Dictionary] = []
var _last_solver_diagnostics: Dictionary = {}
var _cycles_completed := 0
var _frontier_overflows := 0


func initialize(p_scheduler: SparseHydroScheduler, p_atlas: SparseHydroAtlasGPU,
		p_connectivity: SparseHydroConnectivityGPU,
		p_identity_bridge: SparseHydroIdentityBridge,
		reachability: Callable,
		destination_state_provider: Callable = Callable()) -> Error:
	if _phase != Phase.OFFLINE:
		return ERR_BUSY
	if p_scheduler == null or p_atlas == null or p_connectivity == null \
			or p_identity_bridge == null or not reachability.is_valid():
		return ERR_INVALID_PARAMETER
	if p_scheduler.pool == null or not p_atlas.initialized_ok() \
			or not p_connectivity.initialized_ok() or not p_identity_bridge.is_bound():
		return ERR_UNCONFIGURED
	if p_scheduler.pool.capacity != p_atlas.capacity \
			or p_connectivity.capacity != p_atlas.capacity:
		return ERR_INVALID_PARAMETER
	if RenderingServer.get_rendering_device() == null:
		return ERR_UNAVAILABLE

	scheduler = p_scheduler
	atlas = p_atlas
	connectivity = p_connectivity
	identity_bridge = p_identity_bridge
	_reachability = reachability
	_destination_provider = destination_state_provider
	_using_internal_terrain_provider = not _destination_provider.is_valid()
	if disable_placeholder_wet_freeze:
		scheduler.freeze_wet_tiles = false

	_phase = Phase.INITIALIZING
	_component_ready = {
		"solver": false,
		"activity": false,
		"frontier": false,
		"activation": false,
		"terrain": not _using_internal_terrain_provider,
		# Generic point-source ingress currently depends on HydroTerrainBedGPU for
		# first-domain creation. Custom numerical providers can still run without it.
		"sources": not _using_internal_terrain_provider,
	}

	solver = SparseHydroStepGPU.new()
	solver.name = "SparseHydroStepGPU"
	solver.gravity = gravity
	add_child(solver)
	solver.initialized.connect(_on_solver_initialized)
	solver.initialization_failed.connect(
		func(error: Error): _fail_initialization(error, "solver"))
	solver.diagnostics_ready.connect(_on_solver_diagnostics)

	activity = HydroTileActivityGPU.new()
	activity.name = "HydroTileActivityGPU"
	add_child(activity)
	activity.initialized.connect(_on_activity_initialized)
	activity.initialization_failed.connect(
		func(error: Error): _fail_initialization(error, "activity"))
	activity.summaries_ready.connect(_on_activity_summaries)
	activity.classification_failed.connect(_on_activity_failed)

	activation = HydroFrontierActivationPipeline.new()
	activation.name = "HydroFrontierActivationPipeline"
	add_child(activation)
	activation.initialized.connect(_on_activation_initialized)
	activation.initialization_failed.connect(
		func(error: Error): _fail_initialization(error, "activation"))
	activation.batch_completed.connect(_on_activation_completed)
	activation.batch_failed.connect(_on_activation_failed)

	if _using_internal_terrain_provider:
		terrain_bed = HydroTerrainBedGPU.new()
		terrain_bed.name = "HydroTerrainBedGPU"
		add_child(terrain_bed)
		terrain_bed.initialized.connect(_on_terrain_initialized)
		terrain_bed.initialization_failed.connect(
			func(error: Error): _fail_initialization(error, "terrain"))
		terrain_bed.stage_failed.connect(_on_terrain_stage_failed)

	var err := solver.initialize(atlas, connectivity)
	if err != OK:
		_fail_initialization(err, "solver")
		return err
	err = activity.initialize(atlas.state_a_rid(), atlas.occupancy_rid(),
		atlas.capacity, atlas.tile_resolution, atlas.cell_size_m, solver.dry_eps, gravity)
	if err != OK:
		_fail_initialization(err, "activity")
		return err
	err = activation.initialize(scheduler, atlas, connectivity, identity_bridge)
	if err != OK:
		_fail_initialization(err, "activation")
		return err
	if terrain_bed != null:
		err = terrain_bed.initialize(atlas)
		if err != OK:
			_fail_initialization(err, "terrain")
			return err
	return OK


func initialized_ok() -> bool:
	return _phase >= Phase.IDLE and _phase != Phase.FAILED


func phase() -> int:
	return _phase


func phase_name() -> String:
	return Phase.keys()[_phase] if _phase >= 0 and _phase < Phase.size() else "UNKNOWN"


func busy() -> bool:
	return _phase in [Phase.SOURCES, Phase.ADVANCING, Phase.CLASSIFYING,
		Phase.FRONTIER, Phase.ACTIVATING]


func time_debt_s() -> float:
	return _time_debt_s


func sources_available() -> bool:
	return source_ingress != null and source_ingress.initialized_ok()


## Generic world-space point source. Positive Q adds water; negative Q removes it.
func upsert_point_source(source_id: String, direction: Vector3,
		rate_m3_s: float, injection_velocity_world: Vector3 = Vector3.ZERO,
		tile_level: int = -1, source_enabled: bool = true) -> Error:
	if not sources_available():
		return ERR_UNAVAILABLE
	var err := source_ingress.upsert_point_source(source_id, direction, rate_m3_s,
		injection_velocity_world, tile_level, source_enabled)
	if err == OK:
		_pump()
	return err


func remove_point_source(source_id: String) -> bool:
	if not sources_available():
		return false
	var removed := source_ingress.remove_source(source_id)
	if removed:
		_pump()
	return removed


func set_point_source_enabled(source_id: String, source_enabled: bool) -> bool:
	if not sources_available():
		return false
	var changed := source_ingress.set_source_enabled(source_id, source_enabled)
	if changed:
		_pump()
	return changed


## Add physical simulation time. The remainder is never discarded merely because
## one macro advance hit its GPU substep cap.
func advance_time(dt_s: float) -> Error:
	if _phase in [Phase.OFFLINE, Phase.INITIALIZING, Phase.FAILED]:
		return ERR_UNCONFIGURED
	if not is_finite(dt_s) or dt_s < 0.0:
		return ERR_INVALID_PARAMETER
	_time_debt_s = minf(_time_debt_s + dt_s, maxf(max_time_debt_s, min_cycle_dt_s))
	_pump()
	return OK


func clear_time_debt() -> void:
	_time_debt_s = 0.0


func stats() -> Dictionary:
	return {
		"initialized": initialized_ok(),
		"enabled": enabled,
		"auto_run": auto_run,
		"phase": phase_name(),
		"time_debt_s": _time_debt_s,
		"macro_dt_s": macro_dt_s,
		"max_time_debt_s": max_time_debt_s,
		"max_gpu_substeps": max_gpu_substeps,
		"cycles_completed": _cycles_completed,
		"frontier_overflows": _frontier_overflows,
		"last_solver_diagnostics": _last_solver_diagnostics.duplicate(true),
		"sources": {} if source_ingress == null else source_ingress.stats(),
		"scheduler": {} if scheduler == null else scheduler.stats(),
	}


func _process(delta: float) -> void:
	if not auto_run or not enabled or _phase in [Phase.OFFLINE, Phase.INITIALIZING, Phase.FAILED]:
		return
	if not is_finite(delta) or delta <= 0.0:
		return
	var source_work := source_ingress != null and source_ingress.dirty()
	# Do not accumulate a huge catch-up burst while neither physical water nor a
	# pending source exists. A new spring/glacier source is allowed to create the
	# first tile and then consume the time accumulated in this frame.
	if not _has_solver_visible_tiles() and not source_work:
		_time_debt_s = 0.0
		return
	advance_time(delta * maxf(time_scale, 0.0))


func _has_solver_visible_tiles() -> bool:
	if scheduler == null or scheduler.pool == null:
		return false
	for record in scheduler.pool.active_records():
		if int(record.get("state", HydroTilePool.TileState.ALLOCATING)) \
				!= HydroTilePool.TileState.ALLOCATING:
			return true
	return false


func _pump() -> void:
	if not enabled or _phase != Phase.IDLE:
		return
	# Source definitions are synchronized only at an idle boundary, before the next
	# SWE command chain can read the source buffer.
	if source_ingress != null and source_ingress.dirty():
		_phase = Phase.SOURCES
		var source_request := source_ingress.flush()
		if source_request < 0:
			_fail_runtime(ERR_BUSY, "source_sync_submit")
		return
	if _time_debt_s < min_cycle_dt_s or not _has_solver_visible_tiles():
		return

	_cycle_id = _next_cycle_id
	_next_cycle_id += 1
	_cycle_requested_dt_s = minf(_time_debt_s, maxf(macro_dt_s, min_cycle_dt_s))
	_cycle_advanced_dt_s = 0.0
	_cycle_summaries = []
	_cycle_activation_batch_id = -1
	_phase = Phase.ADVANCING
	cycle_started.emit(_cycle_id, _cycle_requested_dt_s, _time_debt_s)

	_cycle_solver_step_id = solver.advance(_cycle_requested_dt_s,
		clampi(max_gpu_substeps, 1, SparseHydroStepGPU.MAX_GPU_SUBSTEPS), true)
	if _cycle_solver_step_id < 0:
		_fail_runtime(ERR_BUSY, "advance_submit")


func _on_solver_initialized() -> void:
	_component_ready["solver"] = true
	_try_finish_initialization()


func _on_activity_initialized() -> void:
	_component_ready["activity"] = true
	frontier = HydroFrontierCandidatesGPU.new()
	frontier.name = "HydroFrontierCandidatesGPU"
	add_child(frontier)
	frontier.initialized.connect(_on_frontier_initialized)
	frontier.initialization_failed.connect(
		func(error: Error): _fail_initialization(error, "frontier"))
	frontier.candidates_ready.connect(_on_frontier_candidates)
	frontier.queue_failed.connect(_on_frontier_failed)
	var err := frontier.initialize(activity.summary_rid(), atlas.tile_metadata_rid(),
		atlas.capacity, scheduler.wake_flux_threshold_m3s)
	if err != OK:
		_fail_initialization(err, "frontier")


func _on_frontier_initialized() -> void:
	_component_ready["frontier"] = true
	_try_finish_initialization()


func _on_activation_initialized() -> void:
	_component_ready["activation"] = true
	_try_finish_initialization()


func _on_terrain_initialized() -> void:
	_component_ready["terrain"] = true
	_destination_provider = Callable(terrain_bed, &"stage_reserved_tile")

	source_ingress = HydroSourceIngress.new()
	source_ingress.name = "HydroSourceIngress"
	add_child(source_ingress)
	source_ingress.initialized.connect(_on_source_ingress_initialized)
	source_ingress.initialization_failed.connect(
		func(error: Error): _fail_initialization(error, "sources"))
	source_ingress.flush_completed.connect(_on_source_flush_completed)
	source_ingress.flush_failed.connect(_on_source_flush_failed)
	var err := source_ingress.initialize(scheduler, atlas, connectivity,
		identity_bridge, terrain_bed)
	if err != OK:
		_fail_initialization(err, "sources")
		return
	_try_finish_initialization()


func _on_source_ingress_initialized() -> void:
	_component_ready["sources"] = true
	_try_finish_initialization()


func _try_finish_initialization() -> void:
	if _phase != Phase.INITIALIZING:
		return
	for ready: Variant in _component_ready.values():
		if not bool(ready):
			return
	if not _destination_provider.is_valid():
		_fail_initialization(ERR_UNCONFIGURED, "destination_provider")
		return
	_phase = Phase.IDLE
	initialized.emit()
	_pump()


func _on_source_flush_completed(_request_id: int, source_count: int,
		entry_count: int) -> void:
	if _phase != Phase.SOURCES:
		return
	_phase = Phase.IDLE
	source_sync_completed.emit(source_count, entry_count)
	call_deferred(&"_pump")


func _on_source_flush_failed(_request_id: int, error: Error, stage: String) -> void:
	if _phase == Phase.SOURCES:
		_fail_runtime(error, "source_sync_" + stage)


func _on_solver_diagnostics(step_id: int, diagnostics: Dictionary) -> void:
	if _phase != Phase.ADVANCING or step_id != _cycle_solver_step_id:
		return
	_last_solver_diagnostics = diagnostics.duplicate(true)
	if int(diagnostics.get("post_invalid_cells", 0)) > 0:
		_fail_runtime(ERR_INVALID_DATA, "solver_health")
		return

	_cycle_advanced_dt_s = clampf(float(diagnostics.get("advanced_dt_s", 0.0)),
		0.0, _cycle_requested_dt_s)
	_time_debt_s = maxf(_time_debt_s - _cycle_advanced_dt_s, 0.0)
	if _cycle_advanced_dt_s <= 0.0:
		_fail_runtime(ERR_CANT_RESOLVE, "zero_advance")
		return

	_phase = Phase.CLASSIFYING
	_cycle_activity_request_id = activity.classify(true)
	if _cycle_activity_request_id < 0:
		_fail_runtime(ERR_BUSY, "activity_submit")


func _on_activity_summaries(request_id: int, summaries: Array[Dictionary]) -> void:
	if _phase != Phase.CLASSIFYING or request_id != _cycle_activity_request_id:
		return

	_cycle_summaries = []
	for summary in summaries:
		if not bool(summary.get("active", false)):
			continue
		var slot := int(summary.get("slot", -1))
		var tile_id := scheduler.pool.id_for_slot(slot)
		if tile_id < 0:
			continue
		var snapshot := summary.duplicate(true)
		snapshot["tile_id"] = tile_id
		_cycle_summaries.append(snapshot)

	_phase = Phase.FRONTIER
	_cycle_frontier_request_id = frontier.generate(true)
	if _cycle_frontier_request_id < 0:
		_fail_runtime(ERR_BUSY, "frontier_submit")
		return

	_apply_activity_policy(_cycle_summaries)
	var conn_error := connectivity.sync_pool(scheduler.pool)
	if conn_error != OK:
		_fail_runtime(conn_error, "connectivity_after_activity")


func _apply_activity_policy(summaries: Array[Dictionary]) -> void:
	for summary in summaries:
		var slot := int(summary.get("slot", -1))
		var tile_id := int(summary.get("tile_id", -1))
		if tile_id < 0 or scheduler.pool.id_for_slot(slot) != tile_id:
			continue
		var key := HydroTileKey.unpack(tile_id)
		if key == null:
			continue
		var max_flux := maxf(
			maxf(float(summary.get("flux_west_m3s", 0.0)),
				float(summary.get("flux_east_m3s", 0.0))),
			maxf(float(summary.get("flux_south_m3s", 0.0)),
				float(summary.get("flux_north_m3s", 0.0))))
		scheduler.report_activity(key,
			float(summary.get("max_depth_m", 0.0)),
			float(summary.get("max_velocity_mps", 0.0)),
			max_flux,
			float(summary.get("kinetic_energy_proxy", 0.0)),
			_cycle_advanced_dt_s)


func _on_frontier_candidates(request_id: int, candidates: Array[Dictionary],
		overflow: bool) -> void:
	if _phase != Phase.FRONTIER or request_id != _cycle_frontier_request_id:
		return
	if overflow:
		_frontier_overflows += 1
		frontier_overflow.emit(_cycle_id)

	if candidates.is_empty():
		_finish_cycle([])
		return

	_phase = Phase.ACTIVATING
	_cycle_activation_batch_id = activation.process_candidates(candidates,
		_reachability, _destination_provider, seed_dt_s, max_seed_fraction, gravity)
	if _cycle_activation_batch_id < 0:
		_fail_runtime(ERR_BUSY, "activation_submit")


func _on_activation_completed(batch_id: int, results: Array[Dictionary]) -> void:
	if _phase != Phase.ACTIVATING or batch_id != _cycle_activation_batch_id:
		return
	_finish_cycle(results)


func _finish_cycle(activation_results: Array[Dictionary]) -> void:
	var report := {
		"cycle_id": _cycle_id,
		"requested_dt_s": _cycle_requested_dt_s,
		"advanced_dt_s": _cycle_advanced_dt_s,
		"remaining_time_debt_s": _time_debt_s,
		"cfl_clamped": bool(_last_solver_diagnostics.get("cfl_clamped", false)),
		"steps_taken": int(_last_solver_diagnostics.get("steps_taken", 0)),
		"activity_tiles": _cycle_summaries.size(),
		"activation_results": activation_results.duplicate(true),
		"scheduler": scheduler.stats(),
	}
	var completed_id := _cycle_id
	_cycles_completed += 1
	_reset_cycle_state()
	_phase = Phase.IDLE
	cycle_completed.emit(completed_id, report)
	call_deferred(&"_pump")


func _on_activity_failed(request_id: int, error: Error) -> void:
	if _phase == Phase.CLASSIFYING and request_id == _cycle_activity_request_id:
		_fail_runtime(error, "activity")


func _on_frontier_failed(request_id: int, error: Error) -> void:
	if _phase == Phase.FRONTIER and request_id == _cycle_frontier_request_id:
		_fail_runtime(error, "frontier")


func _on_activation_failed(batch_id: int, error: Error) -> void:
	if _phase == Phase.ACTIVATING and batch_id == _cycle_activation_batch_id:
		_fail_runtime(error, "activation")


func _on_terrain_stage_failed(_request_id: int, error: Error) -> void:
	if _phase == Phase.ACTIVATING:
		_fail_runtime(error, "terrain_stage")


func _fail_initialization(error: Error, component: String) -> void:
	if _phase == Phase.FAILED:
		return
	_phase = Phase.FAILED
	enabled = false
	initialization_failed.emit(error, component)


func _fail_runtime(error: Error, stage: String) -> void:
	if _phase == Phase.FAILED:
		return
	_phase = Phase.FAILED
	enabled = false
	runtime_failed.emit(error, stage)


func _reset_cycle_state() -> void:
	_cycle_id = -1
	_cycle_requested_dt_s = 0.0
	_cycle_advanced_dt_s = 0.0
	_cycle_solver_step_id = -1
	_cycle_activity_request_id = -1
	_cycle_frontier_request_id = -1
	_cycle_activation_batch_id = -1
	_cycle_summaries = []


func release() -> void:
	if _phase == Phase.OFFLINE:
		return
	enabled = false
	_phase = Phase.OFFLINE
	_reset_cycle_state()
	_time_debt_s = 0.0
	_component_ready.clear()
	_reachability = Callable()
	_destination_provider = Callable()

	if source_ingress != null and is_instance_valid(source_ingress):
		source_ingress.release()
		source_ingress.queue_free()
	if terrain_bed != null and is_instance_valid(terrain_bed):
		terrain_bed.release()
		terrain_bed.queue_free()
	if frontier != null and is_instance_valid(frontier):
		frontier.release()
		frontier.queue_free()
	if activation != null and is_instance_valid(activation):
		activation.release()
		activation.queue_free()
	if activity != null and is_instance_valid(activity):
		activity.release()
		activity.queue_free()
	if solver != null and is_instance_valid(solver):
		solver.release()
		solver.queue_free()

	source_ingress = null
	terrain_bed = null
	frontier = null
	activation = null
	activity = null
	solver = null
	scheduler = null
	atlas = null
	connectivity = null
	identity_bridge = null
	released.emit()


func _exit_tree() -> void:
	release()
