class_name SparseHydrologyRuntimeSubcycled
extends SparseHydrologyRuntimeLOD
## Phase-4 runtime with binary temporal HydroLOD subcycling.
##
## Spatial ownership/interface behavior is inherited unchanged. This layer swaps in
## the enlarged-control subcycled solver, fine-clock schedule, and flux-register
## interface implementation.

var lod_temporal: HydroLODTemporalScheduleGPU


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


func release() -> void:
	if _phase == Phase.OFFLINE:
		return
	if lod_manager != null and lod_manager.busy():
		enabled = false
		_lod_release_requested = true
		lod_manager.release()
		return
	_release_temporal_now()
	super.release()


func _on_lod_manager_released() -> void:
	if not _lod_release_requested:
		return
	_release_temporal_now()
	super._on_lod_manager_released()


func _release_temporal_now() -> void:
	var temporal := lod_temporal
	lod_temporal = null
	if temporal != null and is_instance_valid(temporal):
		temporal.release()
		temporal.queue_free()
