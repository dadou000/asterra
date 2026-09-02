extends "res://scripts/water/water_system_river_coupled_production.gd"
## Final production WaterSystem facade adding symmetric sparse-2D -> persistent-1D
## river collapse. Automatic channel promotion/demotion remain OFF by default.

signal river_reach_collapse_bridge_ready
signal river_reach_collapse_completed(report: Dictionary)
signal river_reach_collapse_failed(error: Error, stage: String)

var automatic_channel_demotion_enabled := false
var _river_reach_collapse_bridge: PlanetRiverReachCollapseBridgeProduction
var _deferred_sparse_release_for_river_collapse := false
var _deferred_store_rebuild_for_river_collapse := false


func _ready() -> void:
	super._ready()
	# Replace the inherited direct store-rebuild handler so collapse can finish its
	# compact readback/ownership transaction before coupling/store generation teardown.
	if PersistentHydrologySystem.store_rebuilt.is_connected(_on_coupled_store_rebuilt):
		PersistentHydrologySystem.store_rebuilt.disconnect(_on_coupled_store_rebuilt)
	if not PersistentHydrologySystem.store_rebuilt.is_connected(
			_on_store_rebuilt_for_river_collapse):
		PersistentHydrologySystem.store_rebuilt.connect(_on_store_rebuilt_for_river_collapse)
	if not river_reach_coupling_ready.is_connected(_try_bind_river_reach_collapse_bridge):
		river_reach_coupling_ready.connect(_try_bind_river_reach_collapse_bridge)
	if not sparse_runtime_ready.is_connected(_try_bind_river_reach_collapse_bridge):
		sparse_runtime_ready.connect(_try_bind_river_reach_collapse_bridge)
	call_deferred(&"_try_bind_river_reach_collapse_bridge")


func river_reach_collapse_bridge_available() -> bool:
	return _river_reach_collapse_bridge != null \
		and _river_reach_collapse_bridge.initialized_ok()


func river_reach_collapse_bridge() -> PlanetRiverReachCollapseBridgeProduction:
	return _river_reach_collapse_bridge


func river_reach_collapse_eligible(cell: int) -> bool:
	return river_reach_collapse_bridge_available() \
		and _river_reach_collapse_bridge.eligible(cell)


func collapse_fine_river_reach(cell: int, ignore_quiet_policy: bool = false) -> int:
	if not river_reach_collapse_bridge_available() \
			or _river_reach_collapse_bridge.busy():
		return -1
	if _river_reach_promotion_bridge != null and _river_reach_promotion_bridge.busy():
		return -1
	if _planet_promotion_bridge != null and _planet_promotion_bridge.busy():
		return -1
	if _planet_demotion_bridge != null and _planet_demotion_bridge.busy():
		return -1
	if _river_reach_coupling != null and _river_reach_coupling.pending():
		return -1
	return _river_reach_collapse_bridge.collapse_reach(cell, ignore_quiet_policy)


func promote_coarse_river_reach(cell: int, requested_volume_m3: float = -1.0) -> int:
	if _river_reach_collapse_bridge != null and _river_reach_collapse_bridge.busy():
		return -1
	return super.promote_coarse_river_reach(cell, requested_volume_m3)


func promote_coarse_surface_cell(cell: int, requested_volume_m3: float = -1.0,
		local_velocity: Vector2 = Vector2.ZERO) -> int:
	if _river_reach_collapse_bridge != null and _river_reach_collapse_bridge.busy():
		return -1
	return super.promote_coarse_surface_cell(cell, requested_volume_m3, local_velocity)


func demote_fine_surface_cell(cell: int) -> int:
	if _river_reach_collapse_bridge != null and _river_reach_collapse_bridge.busy():
		return -1
	return super.demote_fine_surface_cell(cell)


func gpu_stats() -> Dictionary:
	var out := super.gpu_stats()
	out["river_reach_collapse"] = {
		"available": river_reach_collapse_bridge_available(),
		"busy": _river_reach_collapse_bridge != null \
			and _river_reach_collapse_bridge.busy(),
		"automatic_enabled": automatic_channel_demotion_enabled,
		"requires_quiet_state": true,
		"state_readback_bytes": 16,
	}
	return out


func _try_bind_river_reach_collapse_bridge() -> void:
	if _river_reach_collapse_bridge != null or not sparse_runtime_available() \
			or not PersistentHydrologySystem.available() \
			or not (PersistentHydrologySystem.store() is PlanetHydrologyRiverCoupledStore) \
			or not (_river_reach_coupling is HydroRiverReachCouplingCollapse):
		return
	var runtime := sparse_runtime()
	var coupling := _river_reach_coupling as HydroRiverReachCouplingCollapse
	if runtime == null or not runtime.initialized_ok() or not coupling.initialized_ok():
		return
	var bridge := PlanetRiverReachCollapseBridgeProduction.new()
	bridge.name = "PlanetRiverReachCollapseBridge"
	_river_reach_collapse_bridge = bridge
	add_child(bridge)
	bridge.initialized.connect(func():
		if bridge == _river_reach_collapse_bridge:
			river_reach_collapse_bridge_ready.emit())
	bridge.initialization_failed.connect(func(error: Error):
		if bridge == _river_reach_collapse_bridge:
			river_reach_collapse_failed.emit(error, "bridge_initialize")
			_release_river_reach_collapse_bridge())
	bridge.collapse_completed.connect(func(_request_id: int, report: Dictionary):
		if bridge == _river_reach_collapse_bridge:
			river_reach_collapse_completed.emit(report.duplicate(true)))
	bridge.collapse_failed.connect(func(_request_id: int, error: Error, stage: String):
		if bridge == _river_reach_collapse_bridge:
			river_reach_collapse_failed.emit(error, stage))
	bridge.released.connect(func():
		if bridge == _river_reach_collapse_bridge:
			_river_reach_collapse_bridge = null
		if _deferred_store_rebuild_for_river_collapse:
			_deferred_store_rebuild_for_river_collapse = false
			call_deferred(&"_continue_store_rebuild_after_river_collapse")
		elif _deferred_sparse_release_for_river_collapse:
			_deferred_sparse_release_for_river_collapse = false
			call_deferred(&"_continue_sparse_release_after_river_collapse"))
	var err := bridge.initialize(
		PersistentHydrologySystem.store() as PlanetHydrologyRiverCoupledStore,
		runtime.scheduler, runtime.atlas, runtime.connectivity, runtime, coupling)
	if err != OK:
		river_reach_collapse_failed.emit(err, "bridge_initialize")
		_release_river_reach_collapse_bridge()


func _release_river_reach_collapse_bridge() -> void:
	var bridge := _river_reach_collapse_bridge
	if bridge == null:
		return
	bridge.release()
	if not bridge.busy():
		_river_reach_collapse_bridge = null
		if is_instance_valid(bridge):
			bridge.queue_free()


func _on_store_rebuilt_for_river_collapse() -> void:
	if _river_reach_collapse_bridge != null and _river_reach_collapse_bridge.busy():
		_deferred_store_rebuild_for_river_collapse = true
		_river_reach_collapse_bridge.release()
		return
	_release_river_reach_collapse_bridge()
	_on_coupled_store_rebuilt()
	call_deferred(&"_try_bind_river_reach_collapse_bridge")


func _continue_store_rebuild_after_river_collapse() -> void:
	_release_river_reach_collapse_bridge()
	_on_coupled_store_rebuilt()
	call_deferred(&"_try_bind_river_reach_collapse_bridge")


func _release_sparse_runtime() -> void:
	if _river_reach_collapse_bridge != null and _river_reach_collapse_bridge.busy():
		_deferred_sparse_release_for_river_collapse = true
		_river_reach_collapse_bridge.release()
		return
	_release_river_reach_collapse_bridge()
	super._release_sparse_runtime()


func _continue_sparse_release_after_river_collapse() -> void:
	_release_river_reach_collapse_bridge()
	super._release_sparse_runtime()
