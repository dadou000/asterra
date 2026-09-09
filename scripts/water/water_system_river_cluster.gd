extends "res://scripts/water/water_system_river_collapse_production.gd"
## Final production facade preferring contiguous multi-tile river refinement.
##
## Public river APIs stay unchanged. Cluster coupling registration is performed by
## the promotion bridge before sparse runtime resume, closing the brief state where
## fine tiles were visible while the coarse reach still lacked its refinement hole.

var default_river_cluster_tiles := 3
var _river_cluster_promotion: PlanetRiverReachClusterPromotionBridge


func _ready() -> void:
	super._ready()
	if not sparse_runtime_ready.is_connected(_try_bind_river_cluster_promotion):
		sparse_runtime_ready.connect(_try_bind_river_cluster_promotion)
	if not river_reach_coupling_ready.is_connected(_try_bind_river_cluster_promotion):
		river_reach_coupling_ready.connect(_try_bind_river_cluster_promotion)
	call_deferred(&"_try_bind_river_cluster_promotion")


func river_cluster_promotion_available() -> bool:
	return _river_cluster_promotion != null and _river_cluster_promotion.initialized_ok()


func river_cluster_default_member_count() -> int:
	return maxi(default_river_cluster_tiles, 1)


## Exact member count the current planner would request for this reach. This may be
## smaller than the configured cluster size for a short macro reach. The query is
## CPU-only and does not reserve sparse slots or begin an ownership transaction.
func river_cluster_requested_member_count(cell: int) -> int:
	if not river_cluster_promotion_available() or not PersistentHydrologySystem.available() \
			or not (PersistentHydrologySystem.store() is PlanetHydrologyRiverClusterStore):
		return 1
	var runtime := sparse_runtime()
	if runtime == null or runtime.atlas == null or not runtime.atlas.initialized_ok():
		return 0
	var planned := HydroRiverClusterPlanner.plan(
		PersistentHydrologySystem.store() as PlanetHydrologyRiverClusterStore,
		runtime.atlas, cell, default_river_cluster_tiles)
	if int(planned.get("error", FAILED)) != OK:
		return 0
	return maxi(int(planned.get("member_count", 0)), 0)


func river_cluster_free_member_slots() -> int:
	var runtime := sparse_runtime()
	if runtime == null or runtime.scheduler == null or runtime.scheduler.pool == null:
		return 0
	return runtime.scheduler.pool.free_count()


func river_cluster_member_capacity_available(required_members: int = -1) -> bool:
	var required := river_cluster_default_member_count() if required_members < 0 \
		else maxi(required_members, 1)
	return river_cluster_free_member_slots() >= required


func suggested_river_reach_promotion_volume_m3(cell: int) -> float:
	if river_cluster_promotion_available():
		return _river_cluster_promotion.suggested_cluster_volume_m3(
			cell, default_river_cluster_tiles)
	return super.suggested_river_reach_promotion_volume_m3(cell)


func promote_coarse_river_reach(cell: int, requested_volume_m3: float = -1.0) -> int:
	if _river_reach_collapse_bridge != null and _river_reach_collapse_bridge.busy():
		return -1
	if _river_reach_coupling != null \
			and (_river_reach_coupling.pending() or _river_reach_coupling.failed()):
		return -1
	if river_cluster_promotion_available() and not _river_cluster_promotion.busy():
		var required_members := river_cluster_requested_member_count(cell)
		if required_members <= 0 or not river_cluster_member_capacity_available(required_members):
			return -1
		return _river_cluster_promotion.promote_cluster(
			cell, default_river_cluster_tiles, requested_volume_m3)
	return super.promote_coarse_river_reach(cell, requested_volume_m3)


func gpu_stats() -> Dictionary:
	var out := super.gpu_stats()
	out["river_cluster_promotion"] = {
		"available": river_cluster_promotion_available(),
		"busy": _river_cluster_promotion != null and _river_cluster_promotion.busy(),
		"default_tiles": default_river_cluster_tiles,
		"free_member_slots": river_cluster_free_member_slots(),
		"capacity_available": river_cluster_member_capacity_available(),
		"registration_before_runtime_resume": true,
		"fallback": "single_tile_river_promotion",
	}
	return out


## The parent connected this virtual method to river_reach_promotion_completed.
## Cluster reports are already registered synchronously inside their bridge; legacy
## single-tile reports still use the inherited post-promotion registration path.
func _on_river_promotion_for_coupling(report: Dictionary) -> void:
	var cell := int(report.get("cell", -1))
	if _river_reach_coupling != null and cell >= 0 \
			and not _river_reach_coupling.registered_reach(cell).is_empty():
		return
	super._on_river_promotion_for_coupling(report)


func _register_cluster_before_runtime_resume(report: Dictionary) -> int:
	if not (_river_reach_coupling is HydroRiverReachClusterCoupling):
		return ERR_UNCONFIGURED
	return (_river_reach_coupling as HydroRiverReachClusterCoupling) \
		.register_promoted_cluster(report)


func _try_bind_river_cluster_promotion() -> void:
	if _river_cluster_promotion != null or not sparse_runtime_available() \
			or not river_reach_coupling_available() or not PersistentHydrologySystem.available() \
			or not (PersistentHydrologySystem.store() is PlanetHydrologyRiverClusterStore) \
			or not (_river_reach_coupling is HydroRiverReachClusterCoupling):
		return
	var runtime := sparse_runtime()
	if runtime == null or runtime.terrain_bed == null or not runtime.terrain_bed.initialized_ok():
		return
	var bridge := PlanetRiverReachClusterPromotionBridge.new()
	bridge.name = "PlanetRiverReachClusterPromotionBridge"
	bridge.default_cluster_tiles = default_river_cluster_tiles
	bridge.set_registration_callback(Callable(self, &"_register_cluster_before_runtime_resume"))
	_river_cluster_promotion = bridge
	add_child(bridge)
	bridge.promotion_completed.connect(func(_request_id: int, report: Dictionary):
		if bridge == _river_cluster_promotion:
			river_reach_promotion_completed.emit(report.duplicate(true)))
	bridge.promotion_failed.connect(func(_request_id: int, error: Error, stage: String):
		if bridge == _river_cluster_promotion:
			river_reach_promotion_failed.emit(error, "cluster_" + stage))
	bridge.initialization_failed.connect(func(error: Error):
		if bridge == _river_cluster_promotion:
			river_reach_promotion_failed.emit(error, "cluster_initialize")
			_release_river_cluster_promotion())
	var err := bridge.initialize(PersistentHydrologySystem.store() as PlanetHydrologyRiverClusterStore,
		runtime.scheduler, runtime.atlas, runtime.connectivity, runtime.identity_bridge,
		runtime.terrain_bed, runtime)
	if err != OK:
		river_reach_promotion_failed.emit(err, "cluster_initialize")
		_release_river_cluster_promotion()


func _release_river_cluster_promotion() -> void:
	var bridge := _river_cluster_promotion
	_river_cluster_promotion = null
	if bridge != null and is_instance_valid(bridge):
		bridge.release()
		bridge.queue_free()


func _release_sparse_runtime() -> void:
	_release_river_cluster_promotion()
	super._release_sparse_runtime()
