extends "res://scripts/water/water_system_river_cluster.gd"
## Final production facade for contiguous river refinement clusters.
## Single-member reaches retain the legacy collapse bridge; multi-member clusters
## use one aggregate GPU reduction + one batch ownership transition.

var _river_cluster_collapse: PlanetRiverReachClusterCollapseBridge
var _deferred_sparse_release_for_cluster_collapse := false


func _ready() -> void:
	super._ready()
	if not river_reach_coupling_ready.is_connected(_try_bind_river_cluster_collapse):
		river_reach_coupling_ready.connect(_try_bind_river_cluster_collapse)
	if not sparse_runtime_ready.is_connected(_try_bind_river_cluster_collapse):
		sparse_runtime_ready.connect(_try_bind_river_cluster_collapse)
	call_deferred(&"_try_bind_river_cluster_collapse")


func river_reach_collapse_bridge_available() -> bool:
	return (_river_cluster_collapse != null and _river_cluster_collapse.initialized_ok()) \
		or super.river_reach_collapse_bridge_available()


func river_reach_collapse_eligible(cell: int) -> bool:
	if _is_multi_tile_refined_reach(cell):
		return _river_cluster_collapse != null and _river_cluster_collapse.eligible(cell)
	return super.river_reach_collapse_eligible(cell)


func collapse_fine_river_reach(cell: int, ignore_quiet_policy: bool = false) -> int:
	if _is_multi_tile_refined_reach(cell):
		if _river_cluster_collapse == null or not _river_cluster_collapse.initialized_ok() \
				or _river_cluster_collapse.busy():
			return -1
		if _river_cluster_promotion != null and _river_cluster_promotion.busy():
			return -1
		if _river_reach_coupling != null and _river_reach_coupling.pending():
			return -1
		return _river_cluster_collapse.collapse_cluster(cell, ignore_quiet_policy)
	return super.collapse_fine_river_reach(cell, ignore_quiet_policy)


func promote_coarse_river_reach(cell: int, requested_volume_m3: float = -1.0) -> int:
	if _river_cluster_collapse != null and _river_cluster_collapse.busy():
		return -1
	return super.promote_coarse_river_reach(cell, requested_volume_m3)


func promote_coarse_surface_cell(cell: int, requested_volume_m3: float = -1.0,
		local_velocity: Vector2 = Vector2.ZERO) -> int:
	if _river_cluster_collapse != null and _river_cluster_collapse.busy():
		return -1
	return super.promote_coarse_surface_cell(cell, requested_volume_m3, local_velocity)


func demote_fine_surface_cell(cell: int) -> int:
	if _is_multi_tile_refined_reach(cell):
		return -1
	if _river_cluster_collapse != null and _river_cluster_collapse.busy():
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
	return out


func _is_multi_tile_refined_reach(cell: int) -> bool:
	if not PersistentHydrologySystem.available() \
			or not (PersistentHydrologySystem.store() is PlanetHydrologyRiverClusterStore):
		return false
	return (PersistentHydrologySystem.store() as PlanetHydrologyRiverClusterStore) \
		.refined_cluster_size(cell) > 1


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
	if _river_cluster_collapse != null and _river_cluster_collapse.busy():
		_deferred_sparse_release_for_cluster_collapse = true
		_river_cluster_collapse.release()
		return
	_release_river_cluster_collapse()
	super._release_sparse_runtime()


func _continue_sparse_release_after_cluster_collapse() -> void:
	_release_river_cluster_collapse()
	super._release_sparse_runtime()
