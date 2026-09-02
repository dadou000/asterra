class_name SparseHydrologyRuntime
extends Node
## Persistent Phase 3 sparse-hydrology orchestration loop.
##
## The runtime owns no stable world identity and no atlas storage. Those remain in
## SparseHydroScheduler/SparseHydroAtlasGPU. This node only sequences the already
## validated GPU passes and carries unadvanced CFL time debt between cycles:
##
##   advance -> diagnostics -> classify -> frontier -> policy activity
##           -> connectivity sync -> terrain stage/handoff -> repeat
##
## Frontier and activity compact buffers are read back today because allocation,
## exact topology, terrain/structure reachability and settle/sleep policy are still
## CPU decisions. Full water grids remain GPU-resident.

signal initialized
signal initialization_failed(error: Error, component: String)
signal cycle_started(cycle_id: int, requested_dt_s: float, time_debt_s: float)
signal cycle_completed(cycle_id: int, report: Dictionary)
signal frontier_overflow(cycle_id: int)
signal runtime_failed(error: Error, stage: String)
signal released

enum Phase {
	OFFLINE,
	INITIALIZING,
	IDLE,
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
## Keep it disabled until the later analytical-domain collapse pass actually
## removes frozen tiles from the SWE domain.
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
	return _phase in [Phase.ADVANCING, Phase.CLASSIFYING, Phase.FRONTIER, Phase.ACTIVATING]


func time_debt_s() -> float:
	return _time_debt_s


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
		"scheduler": {} if scheduler == null else scheduler.stats(),
	}


func _process(delta: float) -> void:
	if not auto_run or not enabled or _phase in [Phase.OFFLINE, Phase.INITIALIZING, Phase.FAILED]:
		return
	if not is_finite(delta) or delta <= 0.0:
		return
	# Do not accumulate a huge catch-up burst while no physical water domain exists.
	if not _has_solver_visible_tiles():
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

	# Snapshot stable identity before policy can release a dry tile. Frontier work is
	# queued first, so its GPU metadata snapshot precedes any release/unbind calls.
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
	# Carry solver remainder forward without recursive callback chains.
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
