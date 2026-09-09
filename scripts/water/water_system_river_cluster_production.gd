extends "res://scripts/water/water_system_river_cluster.gd"
## Final production facade for single-reach clusters and connected river components.
##
## A component promotion owns its complete coarse graph, hidden sparse construction,
## branched junction seeding, coupling registration and later component-wide collapse.

var _river_cluster_collapse: PlanetRiverReachClusterCollapseBridge
var _deferred_sparse_release_for_cluster_collapse := false

var _river_component_promotion: PlanetRiverComponentPromotionBridgeProduction
var _river_component_collapse: PlanetRiverComponentCollapseBridge
var _deferred_sparse_release_for_component := false


func _ready() -> void:
	super._ready()
	if not river_reach_coupling_ready.is_connected(_try_bind_river_cluster_collapse):
		river_reach_coupling_ready.connect(_try_bind_river_cluster_collapse)
	if not sparse_runtime_ready.is_connected(_try_bind_river_cluster_collapse):
		sparse_runtime_ready.connect(_try_bind_river_cluster_collapse)
	if not river_reach_coupling_ready.is_connected(_try_bind_river_component_bridges):
		river_reach_coupling_ready.connect(_try_bind_river_component_bridges)
	if not sparse_runtime_ready.is_connected(_try_bind_river_component_bridges):
		sparse_runtime_ready.connect(_try_bind_river_component_bridges)
	call_deferred(&"_try_bind_river_cluster_collapse")
	call_deferred(&"_try_bind_river_component_bridges")


func river_component_promotion_available() -> bool:
	return _river_component_promotion != null \
		and _river_component_promotion.initialized_ok()


func river_component_collapse_available() -> bool:
	return _river_component_collapse != null \
		and _river_component_collapse.initialized_ok()


func river_component_requested_member_count(cells: PackedInt32Array) -> int:
	if not river_component_promotion_available():
		return 0
	return _river_component_promotion.planned_member_count(cells)


func river_component_member_capacity_available(cells: PackedInt32Array) -> bool:
	var required := river_component_requested_member_count(cells)
	return required > 0 and river_cluster_free_member_slots() >= required


## Promote a complete connected coarse river graph. All requested cells must still
## be coarse-owned. The graph must be acyclic with exactly one downstream outlet.
func promote_coarse_river_component(cells: PackedInt32Array) -> int:
	if not river_component_promotion_available() \
			or _river_component_promotion.busy() \
			or cells.size() < 2:
		return -1
	if _river_component_collapse != null and _river_component_collapse.busy():
		return -1
	if _river_cluster_collapse != null and _river_cluster_collapse.busy():
		return -1
	if _river_cluster_promotion != null and _river_cluster_promotion.busy():
		return -1
	if _river_reach_collapse_bridge != null and _river_reach_collapse_bridge.busy():
		return -1
	if _river_reach_coupling != null \
			and (_river_reach_coupling.pending() or _river_reach_coupling.failed()):
		return -1
	if not river_component_member_capacity_available(cells):
		return -1
	return _river_component_promotion.promote_component(cells)


func collapse_fine_river_component(component_id: int,
		ignore_quiet_policy: bool = false) -> int:
	if not river_component_collapse_available() or component_id < 0 \
			or _river_component_collapse.busy():
		return -1
	if _river_component_promotion != null and _river_component_promotion.busy():
		return -1
	if _river_cluster_promotion != null and _river_cluster_promotion.busy():
		return -1
	if _river_cluster_collapse != null and _river_cluster_collapse.busy():
		return -1
	if _river_reach_coupling != null and _river_reach_coupling.pending():
		return -1
	return _river_component_collapse.collapse_component(component_id,
		ignore_quiet_policy)


func river_reach_collapse_bridge_available() -> bool:
	return river_component_collapse_available() \
		or (_river_cluster_collapse != null and _river_cluster_collapse.initialized_ok()) \
		or super.river_reach_collapse_bridge_available()


func river_reach_collapse_eligible(cell: int) -> bool:
	var component_id := _refined_component_id(cell)
	if component_id >= 0:
		return _river_component_collapse != null \
			and _river_component_collapse.eligible(component_id)
	if _is_multi_tile_refined_reach(cell):
		return _river_cluster_collapse != null and _river_cluster_collapse.eligible(cell)
	return super.river_reach_collapse_eligible(cell)


func collapse_fine_river_reach(cell: int, ignore_quiet_policy: bool = false) -> int:
	var component_id := _refined_component_id(cell)
	if component_id >= 0:
		return collapse_fine_river_component(component_id, ignore_quiet_policy)
	if _is_multi_tile_refined_reach(cell):
		if _river_cluster_collapse == null or not _river_cluster_collapse.initialized_ok() \
				or _river_cluster_collapse.busy():
			return -1
		if _river_component_promotion != null and _river_component_promotion.busy():
			return -1
		if _river_cluster_promotion != null and _river_cluster_promotion.busy():
			return -1
		if _river_reach_coupling != null and _river_reach_coupling.pending():
			return -1
		return _river_cluster_collapse.collapse_cluster(cell, ignore_quiet_policy)
	return super.collapse_fine_river_reach(cell, ignore_quiet_policy)


func promote_coarse_river_reach(cell: int, requested_volume_m3: float = -1.0) -> int:
	if _river_component_promotion != null and _river_component_promotion.busy():
		return -1
	if _river_component_collapse != null and _river_component_collapse.busy():
		return -1
	if _river_cluster_collapse != null and _river_cluster_collapse.busy():
		return -1
	return super.promote_coarse_river_reach(cell, requested_volume_m3)


func promote_coarse_surface_cell(cell: int, requested_volume_m3: float = -1.0,
		local_velocity: Vector2 = Vector2.ZERO) -> int:
	if _component_bridge_busy() or (_river_cluster_collapse != null \
			and _river_cluster_collapse.busy()):
		return -1
	return super.promote_coarse_surface_cell(cell, requested_volume_m3, local_velocity)


func demote_fine_surface_cell(cell: int) -> int:
	if _refined_component_id(cell) >= 0 or _is_multi_tile_refined_reach(cell):
		return -1
	if _component_bridge_busy() or (_river_cluster_collapse != null \
			and _river_cluster_collapse.busy()):
		return -1
	return super.demote_fine_surface_cell(cell)


func gpu_stats() -> Dictionary:
	var out := super.gpu_stats()
	out["river_cluster_collapse"] = {
		"available": _river_cluster_collapse != null \
			and _river_cluster_collapse.initialized_ok(),
		"busy": _river_cluster_collapse != null and _river_cluster_collapse.busy(),
		"atomic_members": true,
		"state_readback_bytes_per_member": 16,
	}
	out["river_component"] = {
		"promotion_available": river_component_promotion_available(),
		"promotion_busy": _river_component_promotion != null \
			and _river_component_promotion.busy(),
		"collapse_available": river_component_collapse_available(),
		"collapse_busy": _river_component_collapse != null \
			and _river_component_collapse.busy(),
		"branched_junction_seeding": true,
		"atomic_multi_reach_ownership": true,
		"atomic_batch_publish": true,
		"atomic_component_collapse": true,
		"automatic_policy_compatible": true,
	}
	return out


func _refined_component_id(cell: int) -> int:
	if not PersistentHydrologySystem.available() \
			or not (PersistentHydrologySystem.store() is PlanetHydrologyRiverClusterStore):
		return -1
	return (PersistentHydrologySystem.store() as PlanetHydrologyRiverClusterStore) \
		.refined_component_id_for_cell(cell)


func _is_multi_tile_refined_reach(cell: int) -> bool:
	if not PersistentHydrologySystem.available() \
			or not (PersistentHydrologySystem.store() is PlanetHydrologyRiverClusterStore):
		return false
	return (PersistentHydrologySystem.store() as PlanetHydrologyRiverClusterStore) \
		.refined_cluster_size(cell) > 1


func _component_bridge_busy() -> bool:
	return (_river_component_promotion != null and _river_component_promotion.busy()) \
		or (_river_component_collapse != null and _river_component_collapse.busy())


func _register_component_before_runtime_resume(report: Dictionary) -> int:
	if not (_river_reach_coupling is HydroRiverComponentCoupling):
		return ERR_UNCONFIGURED
	return (_river_reach_coupling as HydroRiverComponentCoupling) \
		.register_promoted_component(report)


func _try_bind_river_component_bridges() -> void:
	if not sparse_runtime_available() or not river_reach_coupling_available() \
			or not PersistentHydrologySystem.available() \
			or not (PersistentHydrologySystem.store() is PlanetHydrologyRiverClusterStore) \
			or not (_river_reach_coupling is HydroRiverComponentCoupling):
		return
	var runtime := sparse_runtime()
	if runtime == null or not runtime.initialized_ok() or runtime.terrain_bed == null \
			or not runtime.terrain_bed.initialized_ok():
		return

	if _river_component_promotion == null:
		var promotion := PlanetRiverComponentPromotionBridgeProduction.new()
		promotion.name = "PlanetRiverComponentPromotionBridge"
		promotion.set_registration_callback(
			Callable(self, &"_register_component_before_runtime_resume"))
		_river_component_promotion = promotion
		add_child(promotion)
		promotion.promotion_completed.connect(func(_request_id: int, report: Dictionary):
			if promotion == _river_component_promotion:
				river_reach_promotion_completed.emit(report.duplicate(true)))
		promotion.promotion_failed.connect(func(_request_id: int, error: Error, stage: String):
			if promotion == _river_component_promotion:
				river_reach_promotion_failed.emit(error, "component_" + stage))
		promotion.initialization_failed.connect(func(error: Error):
			if promotion == _river_component_promotion:
				river_reach_promotion_failed.emit(error, "component_initialize")
				_release_river_component_promotion())
		var promotion_error := promotion.initialize(
			PersistentHydrologySystem.store() as PlanetHydrologyRiverClusterStore,
			runtime.scheduler, runtime.atlas, runtime.connectivity, runtime.identity_bridge,
			runtime.terrain_bed, runtime)
		if promotion_error != OK:
			river_reach_promotion_failed.emit(promotion_error, "component_initialize")
			_release_river_component_promotion()

	if _river_component_collapse == null:
		var collapse := PlanetRiverComponentCollapseBridge.new()
		collapse.name = "PlanetRiverComponentCollapseBridge"
		_river_component_collapse = collapse
		add_child(collapse)
		collapse.collapse_completed.connect(func(_request_id: int, report: Dictionary):
			if collapse == _river_component_collapse:
				river_reach_collapse_completed.emit(report.duplicate(true)))
		collapse.collapse_failed.connect(func(_request_id: int, error: Error, stage: String):
			if collapse == _river_component_collapse:
				river_reach_collapse_failed.emit(error, "component_" + stage))
		collapse.initialization_failed.connect(func(error: Error):
			if collapse == _river_component_collapse:
				river_reach_collapse_failed.emit(error, "component_initialize")
				_release_river_component_collapse())
		collapse.released.connect(func():
			if collapse == _river_component_collapse:
				_river_component_collapse = null
			if _deferred_sparse_release_for_component:
				_deferred_sparse_release_for_component = false
				call_deferred(&"_continue_sparse_release_after_component"))
		var collapse_error := collapse.initialize(
			PersistentHydrologySystem.store() as PlanetHydrologyRiverClusterStore,
			runtime.scheduler, runtime.atlas, runtime.connectivity, runtime,
			_river_reach_coupling as HydroRiverComponentCoupling)
		if collapse_error != OK:
			river_reach_collapse_failed.emit(collapse_error, "component_initialize")
			_release_river_component_collapse()


func _try_bind_river_cluster_collapse() -> void:
	if _river_cluster_collapse != null or not sparse_runtime_available() \
			or not PersistentHydrologySystem.available() \
			or not (PersistentHydrologySystem.store() is PlanetHydrologyRiverClusterStore) \
			or not (_river_reach_coupling is HydroRiverReachClusterCoupling):
		return
	var runtime := sparse_runtime()
	if runtime == null or not runtime.initialized_ok():
		return
	var bridge := PlanetRiverReachClusterCollapseBridge.new()
	bridge.name = "PlanetRiverReachClusterCollapseBridge"
	_river_cluster_collapse = bridge
	add_child(bridge)
	bridge.collapse_completed.connect(func(_request_id: int, report: Dictionary):
		if bridge == _river_cluster_collapse:
			river_reach_collapse_completed.emit(report.duplicate(true)))
	bridge.collapse_failed.connect(func(_request_id: int, error: Error, stage: String):
		if bridge == _river_cluster_collapse:
			river_reach_collapse_failed.emit(error, "cluster_" + stage))
	bridge.initialization_failed.connect(func(error: Error):
		if bridge == _river_cluster_collapse:
			river_reach_collapse_failed.emit(error, "cluster_initialize")
			_release_river_cluster_collapse())
	bridge.released.connect(func():
		if bridge == _river_cluster_collapse:
			_river_cluster_collapse = null
		if _deferred_sparse_release_for_cluster_collapse:
			_deferred_sparse_release_for_cluster_collapse = false
			call_deferred(&"_continue_sparse_release_after_cluster_collapse"))
	var err := bridge.initialize(PersistentHydrologySystem.store() as PlanetHydrologyRiverClusterStore,
		runtime.scheduler, runtime.atlas, runtime.connectivity, runtime,
		_river_reach_coupling as HydroRiverReachClusterCoupling)
	if err != OK:
		river_reach_collapse_failed.emit(err, "cluster_initialize")
		_release_river_cluster_collapse()


func _release_river_component_promotion() -> void:
	var bridge := _river_component_promotion
	_river_component_promotion = null
	if bridge != null and is_instance_valid(bridge):
		bridge.release()
		bridge.queue_free()


func _release_river_component_collapse() -> void:
	var bridge := _river_component_collapse
	if bridge == null:
		return
	bridge.release()
	if not bridge.busy():
		_river_component_collapse = null
		if is_instance_valid(bridge):
			bridge.queue_free()


func _release_river_cluster_collapse() -> void:
	var bridge := _river_cluster_collapse
	if bridge == null:
		return
	bridge.release()
	if not bridge.busy():
		_river_cluster_collapse = null
		if is_instance_valid(bridge):
			bridge.queue_free()


func _release_sparse_runtime() -> void:
	if _river_component_collapse != null and _river_component_collapse.busy():
		_deferred_sparse_release_for_component = true
		_river_component_collapse.release()
		return
	if _river_cluster_collapse != null and _river_cluster_collapse.busy():
		_deferred_sparse_release_for_cluster_collapse = true
		_river_cluster_collapse.release()
		return
	_release_river_component_promotion()
	_release_river_component_collapse()
	_release_river_cluster_collapse()
	super._release_sparse_runtime()


func _continue_sparse_release_after_component() -> void:
	_release_river_component_promotion()
	_release_river_component_collapse()
	if _river_cluster_collapse != null and _river_cluster_collapse.busy():
		_deferred_sparse_release_for_cluster_collapse = true
		_river_cluster_collapse.release()
		return
	_release_river_cluster_collapse()
	super._release_sparse_runtime()


func _continue_sparse_release_after_cluster_collapse() -> void:
	_release_river_component_promotion()
	_release_river_component_collapse()
	_release_river_cluster_collapse()
	super._release_sparse_runtime()
