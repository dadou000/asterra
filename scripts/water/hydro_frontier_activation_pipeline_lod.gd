class_name HydroFrontierActivationPipelineLOD
extends HydroFrontierActivationPipeline
## Production frontier pipeline that preserves the Phase-3 transactional lifecycle
## while binding the HydroLOD-aware same-level handoff operator.


func initialize(p_scheduler: SparseHydroScheduler, p_atlas: SparseHydroAtlasGPU,
		p_connectivity: SparseHydroConnectivityGPU,
		p_identity_bridge: SparseHydroIdentityBridge) -> Error:
	if _initialized or _busy:
		return ERR_BUSY
	if p_scheduler == null or p_atlas == null or p_connectivity == null \
			or p_identity_bridge == null:
		return ERR_INVALID_PARAMETER
	if not p_atlas.initialized_ok() or not p_connectivity.initialized_ok() \
			or not p_identity_bridge.is_bound():
		return ERR_UNCONFIGURED
	if p_scheduler.pool.capacity != p_atlas.capacity \
			or p_connectivity.capacity != p_atlas.capacity:
		return ERR_INVALID_PARAMETER

	scheduler = p_scheduler
	atlas = p_atlas
	connectivity = p_connectivity
	identity_bridge = p_identity_bridge
	resolver = HydroFrontierResolver.new(scheduler)
	handoff = HydroFrontierHandoffLOD.new()
	add_child(handoff)
	handoff.initialized.connect(_on_handoff_initialized)
	handoff.initialization_failed.connect(_on_handoff_initialization_failed)
	handoff.handoff_recorded.connect(_on_handoff_recorded)
	handoff.handoff_failed.connect(_on_handoff_failed)
	var err := (handoff as HydroFrontierHandoffLOD).initialize_lod(scheduler, atlas)
	if err != OK:
		handoff.queue_free()
		handoff = null
		return err
	return OK
