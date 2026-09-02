extends "res://scripts/water/water_system_base.gd"
## Production facade extending the stable 0.1.0 water coordinator with the
## transactional planet-coarse -> sparse-SWE ownership bridge.
##
## Automatic promotion deliberately remains OFF. The public methods below expose
## explicit/manual promotion while the conservation gates are being validated.

signal planet_promotion_bridge_state_changed(state: String)
signal planet_promotion_bridge_ready
signal planet_promotion_completed(report: Dictionary)
signal planet_promotion_failed(error: Error, stage: String)

## Policy switch only. No automatic candidate loop is installed yet; keeping this
## false makes the absence of automatic promotion explicit in production stats.
var automatic_coarse_promotion_enabled := false

var _planet_promotion_bridge: PlanetHydroPromotionBridge
var _planet_promotion_state := "offline"


func _ready() -> void:
	super._ready()
	if not sparse_runtime_ready.is_connected(_on_sparse_runtime_ready_for_promotion):
		sparse_runtime_ready.connect(_on_sparse_runtime_ready_for_promotion)
	if not PersistentHydrologySystem.store_rebuilt.is_connected(
			_on_persistent_hydrology_store_rebuilt):
		PersistentHydrologySystem.store_rebuilt.connect(
			_on_persistent_hydrology_store_rebuilt)
	call_deferred(&"_try_bind_planet_promotion_bridge")


func planet_promotion_bridge_available() -> bool:
	return _planet_promotion_state == "ready" \
		and _planet_promotion_bridge != null \
		and _planet_promotion_bridge.initialized_ok()


func planet_promotion_bridge_state() -> String:
	return _planet_promotion_state


func planet_promotion_bridge() -> PlanetHydroPromotionBridge:
	return _planet_promotion_bridge


func coarse_promotion_candidates(max_count: int = 64,
		surface_depth_threshold_m: float = 0.025,
		discharge_ratio_threshold: float = 2.0) -> Array[Dictionary]:
	if not PersistentHydrologySystem.available():
		return []
	return PersistentHydrologySystem.promotion_candidates(max_count,
		surface_depth_threshold_m, discharge_ratio_threshold)


func suggested_surface_promotion_volume_m3(cell: int) -> float:
	if not planet_promotion_bridge_available():
		return 0.0
	return _planet_promotion_bridge.suggested_surface_volume_m3(cell)


## Explicit coarse -> fine transfer. requested_volume_m3 < 0 selects the
## flood-oriented default parcel (coarse surface depth over one fine tile area).
## Returns the asynchronous bridge request ID, or -1 when unavailable/rejected.
func promote_coarse_surface_cell(cell: int, requested_volume_m3: float = -1.0,
		local_velocity: Vector2 = Vector2.ZERO) -> int:
	if not planet_promotion_bridge_available() or _planet_promotion_bridge.busy():
		return -1
	var volume := requested_volume_m3
	if volume < 0.0:
		volume = _planet_promotion_bridge.suggested_surface_volume_m3(cell)
	if not is_finite(volume) or volume <= 0.0:
		return -1
	return _planet_promotion_bridge.promote_cell(cell, volume, local_velocity)


func gpu_stats() -> Dictionary:
	var out := super.gpu_stats()
	out["planet_promotion"] = {
		"state": _planet_promotion_state,
		"available": planet_promotion_bridge_available(),
		"busy": _planet_promotion_bridge != null and _planet_promotion_bridge.busy(),
		"automatic_enabled": automatic_coarse_promotion_enabled,
		"coarse_store_available": PersistentHydrologySystem.available(),
	}
	return out


func _on_sparse_runtime_ready_for_promotion() -> void:
	_try_bind_planet_promotion_bridge()


func _on_persistent_hydrology_store_rebuilt() -> void:
	# A rebuilt planet store invalidates the bridge's old coarse-state reference.
	_release_planet_promotion_bridge()
	call_deferred(&"_try_bind_planet_promotion_bridge")


func _try_bind_planet_promotion_bridge() -> void:
	if _planet_promotion_bridge != null:
		return
	if not sparse_runtime_available():
		_set_planet_promotion_state("waiting_for_sparse_runtime")
		return
	if not PersistentHydrologySystem.available():
		_set_planet_promotion_state("waiting_for_coarse_store")
		return

	var runtime := sparse_runtime()
	if runtime == null or runtime.terrain_bed == null \
			or not runtime.terrain_bed.initialized_ok():
		_set_planet_promotion_state("waiting_for_sparse_terrain")
		return

	var bridge := PlanetHydroPromotionBridge.new()
	bridge.name = "PlanetHydroPromotionBridge"
	_planet_promotion_bridge = bridge
	add_child(bridge)
	bridge.initialized.connect(
		func(): _on_planet_promotion_bridge_initialized(bridge))
	bridge.initialization_failed.connect(
		func(error: Error): _on_planet_promotion_bridge_initialization_failed(
			bridge, error))
	bridge.promotion_completed.connect(
		func(_request_id: int, report: Dictionary):
			if bridge == _planet_promotion_bridge:
				planet_promotion_completed.emit(report.duplicate(true)))
	bridge.promotion_failed.connect(
		func(_request_id: int, error: Error, stage: String):
			if bridge == _planet_promotion_bridge:
				planet_promotion_failed.emit(error, stage))

	_set_planet_promotion_state("initializing")
	var err := bridge.initialize(PersistentHydrologySystem.store(),
		runtime.scheduler, runtime.atlas, runtime.connectivity,
		runtime.identity_bridge, runtime.terrain_bed, runtime)
	if err != OK:
		_on_planet_promotion_bridge_initialization_failed(bridge, err)


func _on_planet_promotion_bridge_initialized(
		bridge: PlanetHydroPromotionBridge) -> void:
	if bridge != _planet_promotion_bridge:
		return
	_set_planet_promotion_state("ready")
	planet_promotion_bridge_ready.emit()


func _on_planet_promotion_bridge_initialization_failed(
		bridge: PlanetHydroPromotionBridge, error: Error) -> void:
	if bridge != _planet_promotion_bridge:
		return
	_set_planet_promotion_state("failed")
	planet_promotion_failed.emit(error, "bridge_initialize")
	bridge.release()
	bridge.queue_free()
	_planet_promotion_bridge = null


func _set_planet_promotion_state(state: String) -> void:
	if _planet_promotion_state == state:
		return
	_planet_promotion_state = state
	planet_promotion_bridge_state_changed.emit(state)


func _release_planet_promotion_bridge() -> void:
	var bridge := _planet_promotion_bridge
	_planet_promotion_bridge = null
	if bridge != null and is_instance_valid(bridge):
		bridge.release()
		bridge.queue_free()
	_set_planet_promotion_state("offline")


## Parent bootstrap calls this whenever a world/runtime is recycled. Releasing the
## ownership bridge first guarantees an in-flight reservation is rolled back while
## both its coarse store and sparse scheduler are still valid.
func _release_sparse_runtime() -> void:
	_release_planet_promotion_bridge()
	super._release_sparse_runtime()


func _exit_tree() -> void:
	if PersistentHydrologySystem.store_rebuilt.is_connected(
			_on_persistent_hydrology_store_rebuilt):
		PersistentHydrologySystem.store_rebuilt.disconnect(
			_on_persistent_hydrology_store_rebuilt)
	if sparse_runtime_ready.is_connected(_on_sparse_runtime_ready_for_promotion):
		sparse_runtime_ready.disconnect(_on_sparse_runtime_ready_for_promotion)
	super._exit_tree()
