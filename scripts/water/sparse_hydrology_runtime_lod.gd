class_name SparseHydrologyRuntimeLOD
extends SparseHydrologyRuntime
## Phase-4 production runtime foundation.
##
## Reuses the proven Phase-3 cycle orchestration while binding per-level metrics,
## HydroLOD-aware ingress/frontier policy and an atomic conservative parent/children
## transition manager. Cross-LOD live interfaces remain disabled until the reflux
## layer is initialized, so no unsupported mixed boundary can enter the solver.

signal hydrolod_ready
signal hydrolod_transition_completed(report: Dictionary)
signal hydrolod_transition_failed(error: Error, stage: String, recovery: String)

var maximum_physical_lod := 4
var lod_manager: HydroPhysicalLODManager

var _lod_previous_enabled := true
var _lod_release_requested := false


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
		"hydrolod": not _using_internal_terrain_provider,
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
	return lod_manager != null and lod_manager.initialized_ok()


func hydrolod_coarsen_eligibility(parent: HydroTileKey) -> Dictionary:
	if not hydrolod_available():
		return {"error": ERR_UNCONFIGURED, "eligible": false,
			"reason": "hydrolod_unavailable"}
	return lod_manager.coarsen_eligibility(parent)


func hydrolod_refine_eligibility(parent: HydroTileKey) -> Dictionary:
	if not hydrolod_available():
		return {"error": ERR_UNCONFIGURED, "eligible": false,
			"reason": "hydrolod_unavailable"}
	return lod_manager.refine_eligibility(parent)


func coarsen_hydrolod(parent: HydroTileKey) -> int:
	if not hydrolod_available() or _phase != Phase.IDLE or busy():
		return -1
	_lod_previous_enabled = enabled
	enabled = false
	var request_id := lod_manager.coarsen(parent)
	if request_id < 0:
		enabled = _lod_previous_enabled
	return request_id


func refine_hydrolod(parent: HydroTileKey) -> int:
	if not hydrolod_available() or _phase != Phase.IDLE or busy():
		return -1
	_lod_previous_enabled = enabled
	enabled = false
	var request_id := lod_manager.refine(parent)
	if request_id < 0:
		enabled = _lod_previous_enabled
	return request_id


func set_hydrolod_transition_guard(guard: Callable) -> Error:
	if not hydrolod_available():
		return ERR_UNCONFIGURED
	return lod_manager.set_transition_guard(guard)


func busy() -> bool:
	return super.busy() or (lod_manager != null and lod_manager.busy())


func stats() -> Dictionary:
	var out := super.stats()
	out["physical_hydrolod"] = {
		"available": hydrolod_available(),
		"maximum_lod": maximum_physical_lod,
		"base_tile_level": -1 if atlas == null else atlas.base_tile_level,
		"mixed_metric_solver": true,
		"conservative_transfer": true,
		"cross_lod_interfaces_enabled": false if lod_manager == null \
			else lod_manager.cross_lod_interfaces_enabled,
		"manager": {} if lod_manager == null else lod_manager.stats(),
	}
	return out


func _on_solver_initialized() -> void:
	_component_ready["solver"] = true
	_try_initialize_lod_manager()
	_try_finish_initialization()


func _on_terrain_initialized() -> void:
	_component_ready["terrain"] = true
	_destination_provider = Callable(terrain_bed, &"stage_reserved_tile")

	source_ingress = HydroSourceIngressLOD.new()
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
	_try_initialize_lod_manager()
	_try_finish_initialization()


func _try_initialize_lod_manager() -> void:
	if not _using_internal_terrain_provider or lod_manager != null \
			or terrain_bed == null or not terrain_bed.initialized_ok() \
			or solver == null or not solver.initialized_ok():
		return
	var manager := HydroPhysicalLODManager.new()
	manager.name = "HydroPhysicalLODManager"
	manager.maximum_physical_lod = maximum_physical_lod
	lod_manager = manager
	add_child(manager)
	manager.initialized.connect(_on_lod_manager_initialized)
	manager.initialization_failed.connect(func(error: Error):
		if manager == lod_manager:
			_fail_initialization(error, "hydrolod"))
	manager.transition_completed.connect(_on_lod_transition_completed)
	manager.transition_failed.connect(_on_lod_transition_failed)
	manager.released.connect(_on_lod_manager_released)
	var err := manager.initialize(scheduler, atlas, connectivity, terrain_bed,
		solver.atmospheric_source_rid())
	if err != OK:
		_fail_initialization(err, "hydrolod")


func _on_lod_manager_initialized() -> void:
	_component_ready["hydrolod"] = true
	hydrolod_ready.emit()
	_try_finish_initialization()


func _on_lod_transition_completed(_request_id: int, report: Dictionary) -> void:
	if source_ingress is HydroSourceIngressLOD:
		(source_ingress as HydroSourceIngressLOD).request_rebuild()
	enabled = _lod_previous_enabled
	hydrolod_transition_completed.emit(report.duplicate(true))
	if enabled:
		call_deferred(&"_pump")


func _on_lod_transition_failed(_request_id: int, error: Error, stage: String,
		recovery: String) -> void:
	if source_ingress is HydroSourceIngressLOD:
		(source_ingress as HydroSourceIngressLOD).request_rebuild()
	enabled = _lod_previous_enabled
	hydrolod_transition_failed.emit(error, stage, recovery)
	# Manager failure paths preserve either the original or the conservative target
	# representation. Continue only after a coherent recovery result was reported.
	if recovery.is_empty():
		_fail_runtime(error, "hydrolod_" + stage)
	elif enabled:
		call_deferred(&"_pump")


func release() -> void:
	if _phase == Phase.OFFLINE:
		return
	if lod_manager != null and lod_manager.busy():
		enabled = false
		_lod_release_requested = true
		lod_manager.release()
		return
	_release_lod_manager_now()
	super.release()


func _on_lod_manager_released() -> void:
	if not _lod_release_requested:
		return
	var manager := lod_manager
	lod_manager = null
	_lod_release_requested = false
	if manager != null and is_instance_valid(manager):
		manager.queue_free()
	super.release()


func _release_lod_manager_now() -> void:
	var manager := lod_manager
	lod_manager = null
	if manager != null and is_instance_valid(manager):
		manager.release()
		manager.queue_free()
