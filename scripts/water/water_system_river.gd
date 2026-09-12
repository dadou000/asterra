extends "res://scripts/water/water_system.gd"
## Final production WaterSystem facade adding explicit 1D river-reach -> sparse 2D
## promotion without changing the existing surface promotion/demotion contracts.
## Automatic channel promotion remains OFF until corridor reconstruction and
## conservation gates are runtime-passed on the project Godot build.

signal river_reach_promotion_bridge_state_changed(state: String)
signal river_reach_promotion_bridge_ready
signal river_reach_promotion_completed(report: Dictionary)
signal river_reach_promotion_failed(error: Error, stage: String)

var automatic_channel_promotion_enabled := false
var _river_reach_promotion_bridge: PlanetRiverReachPromotionBridge
var _river_reach_promotion_state := "offline"


func _ready() -> void:
	super._ready()
	if not sparse_runtime_ready.is_connected(_on_sparse_runtime_ready_for_river):
		sparse_runtime_ready.connect(_on_sparse_runtime_ready_for_river)
	if not sparse_runtime_state_changed.is_connected(_on_sparse_runtime_state_for_river):
		sparse_runtime_state_changed.connect(_on_sparse_runtime_state_for_river)
	if not PersistentHydrologySystem.store_rebuilt.is_connected(_on_river_store_rebuilt):
		PersistentHydrologySystem.store_rebuilt.connect(_on_river_store_rebuilt)
	call_deferred(&"_try_bind_river_reach_promotion_bridge")


func river_reach_promotion_bridge_available() -> bool:
	return _river_reach_promotion_state == "ready" \
		and _river_reach_promotion_bridge != null \
		and _river_reach_promotion_bridge.initialized_ok()


func river_reach_promotion_bridge_state() -> String:
	return _river_reach_promotion_state


func river_reach_promotion_bridge() -> PlanetRiverReachPromotionBridge:
	return _river_reach_promotion_bridge


func channel_reach_candidates(max_count: int = 64,
		discharge_ratio_threshold: float = 2.0,
		bankfull_ratio_threshold: float = 0.85) -> Array[Dictionary]:
	if not PersistentHydrologySystem.available():
		return []
	return PersistentHydrologySystem.channel_reach_candidates(
		max_count, discharge_ratio_threshold, bankfull_ratio_threshold)


func suggested_river_reach_promotion_volume_m3(cell: int) -> float:
	if not river_reach_promotion_bridge_available():
		return 0.0
	return _river_reach_promotion_bridge.suggested_channel_volume_m3(cell)


## Explicit 1D channel -> sparse 2D river transfer. Negative volume selects the
## fine-tile-length share of the current 1D cross-section.
func promote_coarse_river_reach(cell: int, requested_volume_m3: float = -1.0) -> int:
	if not river_reach_promotion_bridge_available() \
			or _river_reach_promotion_bridge.busy():
		return -1
	if _planet_promotion_bridge != null and _planet_promotion_bridge.busy():
		return -1
	if _planet_demotion_bridge != null and _planet_demotion_bridge.busy():
		return -1
	return _river_reach_promotion_bridge.promote_reach(cell, requested_volume_m3)


func gpu_stats() -> Dictionary:
	var out := super.gpu_stats()
	out["river_reach_promotion"] = {
		"state": _river_reach_promotion_state,
		"available": river_reach_promotion_bridge_available(),
		"busy": _river_reach_promotion_bridge != null \
			and _river_reach_promotion_bridge.busy(),
		"automatic_enabled": automatic_channel_promotion_enabled,
		"channel_only": true,
		"seed_strategy": "terrain_aligned_river_corridor",
	}
	return out


func _on_sparse_runtime_ready_for_river() -> void:
	_try_bind_river_reach_promotion_bridge()


func _on_sparse_runtime_state_for_river(state: String) -> void:
	if state == "ready":
		_try_bind_river_reach_promotion_bridge()
		return
	if _river_reach_promotion_bridge != null and not _river_reach_promotion_bridge.busy():
		_release_river_reach_promotion_bridge()
	if state == "disabled":
		_set_river_reach_promotion_state("disabled")
	elif state == "unavailable_no_rendering_device":
		_set_river_reach_promotion_state("unavailable")
	elif state == "failed":
		_set_river_reach_promotion_state("failed")
	else:
		_set_river_reach_promotion_state("waiting_for_sparse_runtime")


func _on_river_store_rebuilt() -> void:
	_release_river_reach_promotion_bridge()
	call_deferred(&"_try_bind_river_reach_promotion_bridge")


func _try_bind_river_reach_promotion_bridge() -> void:
	if _river_reach_promotion_bridge != null:
		return
	if not sparse_runtime_available():
		_set_river_reach_promotion_state("waiting_for_sparse_runtime")
		return
	if not PersistentHydrologySystem.available() \
			or not (PersistentHydrologySystem.store() is PlanetHydrologyRiverPromotionStore):
		_set_river_reach_promotion_state("waiting_for_reach_store")
		return
	var runtime := sparse_runtime()
	if runtime == null or runtime.terrain_bed == null \
			or not runtime.terrain_bed.initialized_ok():
		_set_river_reach_promotion_state("waiting_for_sparse_terrain")
		return
	var bridge := PlanetRiverReachPromotionBridge.new()
	bridge.name = "PlanetRiverReachPromotionBridge"
	_river_reach_promotion_bridge = bridge
	add_child(bridge)
	bridge.initialized.connect(func():
		if bridge == _river_reach_promotion_bridge:
			_set_river_reach_promotion_state("ready")
			river_reach_promotion_bridge_ready.emit())
	bridge.initialization_failed.connect(func(error: Error):
		if bridge == _river_reach_promotion_bridge:
			_set_river_reach_promotion_state("failed")
			river_reach_promotion_failed.emit(error, "bridge_initialize")
			_release_river_reach_promotion_bridge())
	bridge.promotion_completed.connect(func(_request_id: int, report: Dictionary):
		if bridge == _river_reach_promotion_bridge:
			river_reach_promotion_completed.emit(report.duplicate(true)))
	bridge.promotion_failed.connect(func(_request_id: int, error: Error, stage: String):
		if bridge == _river_reach_promotion_bridge:
			river_reach_promotion_failed.emit(error, stage))
	_set_river_reach_promotion_state("initializing")
	var err := bridge.initialize(
		PersistentHydrologySystem.store() as PlanetHydrologyRiverPromotionStore,
		runtime.scheduler, runtime.atlas, runtime.connectivity,
		runtime.identity_bridge, runtime.terrain_bed, runtime)
	if err != OK:
		_set_river_reach_promotion_state("failed")
		river_reach_promotion_failed.emit(err, "bridge_initialize")
		_release_river_reach_promotion_bridge()


func _set_river_reach_promotion_state(state: String) -> void:
	if _river_reach_promotion_state == state:
		return
	_river_reach_promotion_state = state
	river_reach_promotion_bridge_state_changed.emit(state)


func _release_river_reach_promotion_bridge() -> void:
	var bridge := _river_reach_promotion_bridge
	_river_reach_promotion_bridge = null
	if bridge != null and is_instance_valid(bridge):
		bridge.release()
		bridge.queue_free()
	_set_river_reach_promotion_state("offline")


func _release_sparse_runtime() -> void:
	_release_river_reach_promotion_bridge()
	super._release_sparse_runtime()
