class_name SparseHydrologyRuntimeSubcycled
extends SparseHydrologyRuntimeLOD
## Phase-4 runtime with binary temporal HydroLOD subcycling and automatic physical LOD.
##
## Spatial ownership/interface behavior is inherited unchanged. This layer swaps in
## the enlarged-control subcycled solver, fine-clock schedule, flux-register
## interface implementation and one-transition-per-cycle automatic HydroLOD policy.

var lod_temporal: HydroLODTemporalScheduleGPU
var automatic_lod_policy: HydroAutomaticPhysicalLODPolicy
var _automatic_lod_focus_provider := Callable()


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
	if not p_atlas.hydrolod_enabled():
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

	automatic_lod_policy = HydroAutomaticPhysicalLODPolicy.new()
	var policy_error := automatic_lod_policy.initialize(
		scheduler, atlas, maximum_physical_lod)
	if policy_error != OK:
		automatic_lod_policy = null
		return policy_error

	_phase = Phase.INITIALIZING
	_component_ready = {
		"solver": false,
		"activity": false,
		"frontier": false,
		"activation": false,
		"terrain": not _using_internal_terrain_provider,
		"sources": not _using_internal_terrain_provider,
		"temporal": false,
		"interfaces": false,
		"hydrolod": not _using_internal_terrain_provider,
	}

	var subcycled_solver := SparseHydroStepGPUSubcycled.new()
	subcycled_solver.name = "SparseHydroStepGPU"
	subcycled_solver.gravity = gravity
	subcycled_solver.maximum_physical_lod = maximum_physical_lod
	solver = subcycled_solver
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

	activation = HydroFrontierActivationPipelineLOD.new()
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
		atlas.capacity, atlas.tile_resolution, atlas.cell_size_m, solver.dry_eps,
		gravity, atlas.tile_metadata_rid(), atlas.base_tile_level)
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


func hydrolod_available() -> bool:
	return super.hydrolod_available() and lod_temporal != null \
		and lod_temporal.initialized_ok() \
		and solver is SparseHydroStepGPUSubcycled


func set_automatic_hydrolod_enabled(value: bool) -> Error:
	if automatic_lod_policy == null:
		return ERR_UNCONFIGURED
	automatic_lod_policy.enabled = value
	return OK


func automatic_hydrolod_enabled() -> bool:
	return automatic_lod_policy != null and automatic_lod_policy.enabled


## Provider contract:
##   Dictionary { direction: Vector3 body-fixed unit direction,
##                planet_radius_m: float }
## A Vector3 result is also accepted and uses Frames.planet_radius as fallback.
func set_automatic_hydrolod_focus_provider(provider: Callable) -> Error:
	if _phase == Phase.OFFLINE:
		return ERR_UNCONFIGURED
	_automatic_lod_focus_provider = provider
	return OK


func request_automatic_hydrolod_detail(tile_id: int, target_lod: int = 0,
		hold_s: float = 5.0) -> Error:
	if automatic_lod_policy == null:
		return ERR_UNCONFIGURED
	return automatic_lod_policy.request_detail(tile_id, target_lod, hold_s)


func clear_automatic_hydrolod_detail(tile_id: int) -> bool:
	return automatic_lod_policy != null \
		and automatic_lod_policy.clear_detail_request(tile_id)


func stats() -> Dictionary:
	var out := super.stats()
	var physical: Dictionary = out.get("physical_hydrolod", {})
	physical["temporal_subcycling"] = lod_temporal != null \
		and lod_temporal.initialized_ok()
	physical["binary_level_ratios"] = true
	physical["fine_clock_cfl_normalization"] = true
	physical["coarse_fine_flux_registers"] = lod_interfaces \
		is HydroLODInterfaceFluxSubcycledGPU
	physical["temporal"] = {} if lod_temporal == null else lod_temporal.stats()
	physical["automatic_policy"] = {} if automatic_lod_policy == null \
		else automatic_lod_policy.stats()
	out["physical_hydrolod"] = physical
	return out


func _on_solver_initialized() -> void:
	_component_ready["solver"] = true
	_try_initialize_temporal_schedule()
	_try_initialize_lod_interfaces()
	_try_initialize_lod_manager()
	_try_finish_initialization()


func _try_initialize_temporal_schedule() -> void:
	if lod_temporal != null or solver == null or not solver.initialized_ok() \
			or not (solver is SparseHydroStepGPUSubcycled):
		return
	var temporal := HydroLODTemporalScheduleGPU.new()
	temporal.name = "HydroLODTemporalScheduleGPU"
	lod_temporal = temporal
	add_child(temporal)
	temporal.initialized.connect(_on_temporal_initialized)
	temporal.initialization_failed.connect(func(error: Error):
		if temporal == lod_temporal:
			_fail_initialization(error, "hydrolod_temporal"))
	var err := temporal.initialize(solver.control_buffer_rid(), maximum_physical_lod)
	if err != OK:
		_fail_initialization(err, "hydrolod_temporal")


func _on_temporal_initialized() -> void:
	if not (solver is SparseHydroStepGPUSubcycled):
		_fail_initialization(ERR_UNCONFIGURED, "hydrolod_temporal_solver")
		return
	var err := (solver as SparseHydroStepGPUSubcycled).set_temporal_schedule(lod_temporal)
	if err != OK:
		_fail_initialization(err, "hydrolod_temporal_bind")
		return
	_component_ready["temporal"] = true
	_try_finish_initialization()


func _try_initialize_lod_interfaces() -> void:
	if lod_interfaces != null or solver == null or not solver.initialized_ok():
		return
	var interfaces := HydroLODInterfaceFluxSubcycledGPU.new()
	interfaces.name = "HydroLODInterfaceFluxGPU"
	interfaces.maximum_physical_lod = maximum_physical_lod
	lod_interfaces = interfaces
	add_child(interfaces)
	interfaces.initialized.connect(_on_lod_interfaces_initialized)
	interfaces.initialization_failed.connect(func(error: Error):
		if interfaces == lod_interfaces:
			_fail_initialization(error, "hydrolod_interfaces"))
	var err := interfaces.initialize(atlas, solver.control_buffer_rid(),
		solver.gravity, solver.dry_eps)
	if err != OK:
		_fail_initialization(err, "hydrolod_interfaces")


## Run policy only after the ordinary activity/frontier cycle is fully committed.
## super._finish_cycle() returns the runtime to IDLE and queues the normal pump; if
## an automatic transaction starts here it disables the runtime before that deferred
## pump can enqueue another solver advance.
func _finish_cycle(activation_results: Array[Dictionary]) -> void:
	var policy_summaries := _cycle_summaries.duplicate(true)
	var policy_dt := _cycle_advanced_dt_s
	super._finish_cycle(activation_results)
	_run_automatic_lod_policy(policy_summaries, policy_dt)


func _run_automatic_lod_policy(summaries: Array[Dictionary], advanced_dt_s: float) -> void:
	if _phase != Phase.IDLE or automatic_lod_policy == null \
			or not automatic_lod_policy.enabled or not hydrolod_available() or busy():
		return
	var focus_context := _automatic_focus_context()
	var action := automatic_lod_policy.choose_action(summaries, advanced_dt_s,
		focus_context, Callable(self, &"hydrolod_refine_eligibility"),
		Callable(self, &"hydrolod_coarsen_eligibility"))
	if action.is_empty():
		return
	var parent := action.get("parent") as HydroTileKey
	if parent == null:
		return
	var request_id := -1
	match String(action.get("mode", "")):
		"refine": request_id = refine_hydrolod(parent)
		"coarsen": request_id = coarsen_hydrolod(parent)
	if request_id >= 0:
		automatic_lod_policy.note_action_started(action)


func _automatic_focus_context() -> Dictionary:
	var context: Dictionary = {}
	if _automatic_lod_focus_provider.is_valid():
		var value: Variant = _automatic_lod_focus_provider.call()
		if value is Dictionary:
			context = (value as Dictionary).duplicate(true)
		elif value is Vector3:
			context["direction"] = value
	if not context.has("planet_radius_m"):
		context["planet_radius_m"] = Frames.planet_radius
	return context


func _on_lod_transition_completed(request_id: int, report: Dictionary) -> void:
	if automatic_lod_policy != null:
		automatic_lod_policy.note_transition_completed(report)
	super._on_lod_transition_completed(request_id, report)


func _on_lod_transition_failed(request_id: int, error: Error, stage: String,
		recovery: String) -> void:
	if automatic_lod_policy != null:
		automatic_lod_policy.note_transition_failed(error, stage, recovery)
	super._on_lod_transition_failed(request_id, error, stage, recovery)


func release() -> void:
	if _phase == Phase.OFFLINE:
		return
	if lod_manager != null and lod_manager.busy():
		enabled = false
		_lod_release_requested = true
		lod_manager.release()
		return
	_release_temporal_now()
	_automatic_lod_focus_provider = Callable()
	automatic_lod_policy = null
	super.release()


func _on_lod_manager_released() -> void:
	if not _lod_release_requested:
		return
	_release_temporal_now()
	_automatic_lod_focus_provider = Callable()
	automatic_lod_policy = null
	super._on_lod_manager_released()


func _release_temporal_now() -> void:
	var temporal := lod_temporal
	lod_temporal = null
	if temporal != null and is_instance_valid(temporal):
		temporal.release()
		temporal.queue_free()
