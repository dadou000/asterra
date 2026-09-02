extends "res://scripts/water/water_system_river.gd"
## Final production WaterSystem facade with continuous operator-split 1D<->2D
## through-flow for explicitly promoted river reaches.

signal river_reach_coupling_ready
signal river_reach_exchange_completed(report: Dictionary)
signal river_reach_coupling_failed(error: Error, stage: String)

var _river_reach_coupling: HydroRiverReachCoupling
var _deferred_sparse_release_for_coupling := false


func _ready() -> void:
	super._ready()
	if not river_reach_promotion_completed.is_connected(_on_river_promotion_for_coupling):
		river_reach_promotion_completed.connect(_on_river_promotion_for_coupling)
	if not sparse_runtime_ready.is_connected(_try_bind_river_reach_coupling):
		sparse_runtime_ready.connect(_try_bind_river_reach_coupling)
	if not PersistentHydrologySystem.store_rebuilt.is_connected(_on_coupled_store_rebuilt):
		PersistentHydrologySystem.store_rebuilt.connect(_on_coupled_store_rebuilt)
	call_deferred(&"_try_bind_river_reach_coupling")


func river_reach_coupling_available() -> bool:
	return _river_reach_coupling != null and _river_reach_coupling.initialized_ok()


func river_reach_coupling() -> HydroRiverReachCoupling:
	return _river_reach_coupling


func promote_coarse_river_reach(cell: int, requested_volume_m3: float = -1.0) -> int:
	if _river_reach_coupling != null \
			and (_river_reach_coupling.pending() or _river_reach_coupling.failed()):
		return -1
	return super.promote_coarse_river_reach(cell, requested_volume_m3)


func promote_coarse_surface_cell(cell: int, requested_volume_m3: float = -1.0,
		local_velocity: Vector2 = Vector2.ZERO) -> int:
	if _river_reach_coupling != null and _river_reach_coupling.pending():
		return -1
	return super.promote_coarse_surface_cell(cell, requested_volume_m3, local_velocity)


func demote_fine_surface_cell(cell: int) -> int:
	if _river_reach_coupling != null and _river_reach_coupling.pending():
		return -1
	return super.demote_fine_surface_cell(cell)


func gpu_stats() -> Dictionary:
	var out := super.gpu_stats()
	out["river_reach_coupling"] = {} if _river_reach_coupling == null \
		else _river_reach_coupling.stats()
	return out


func _on_river_promotion_for_coupling(report: Dictionary) -> void:
	if not river_reach_coupling_available():
		river_reach_coupling_failed.emit(ERR_UNCONFIGURED, "promotion_without_coupling")
		return
	var err := _river_reach_coupling.register_promoted_reach(report)
	if err != OK:
		river_reach_coupling_failed.emit(err, "register_promoted_reach")


func _try_bind_river_reach_coupling() -> void:
	if _river_reach_coupling != null or not sparse_runtime_available() \
			or not PersistentHydrologySystem.available() \
			or not (PersistentHydrologySystem.store() is PlanetHydrologyRiverCoupledStore):
		return
	var runtime := sparse_runtime()
	if runtime == null or not runtime.initialized_ok():
		return
	var coupling := HydroRiverReachCoupling.new()
	coupling.name = "HydroRiverReachCoupling"
	_river_reach_coupling = coupling
	add_child(coupling)
	coupling.initialized.connect(func():
		if coupling == _river_reach_coupling:
			river_reach_coupling_ready.emit())
	coupling.initialization_failed.connect(func(error: Error):
		if coupling == _river_reach_coupling:
			river_reach_coupling_failed.emit(error, "coupling_initialize")
			_release_river_reach_coupling())
	coupling.exchange_completed.connect(func(_request_id: int, report: Dictionary):
		if coupling == _river_reach_coupling:
			river_reach_exchange_completed.emit(report.duplicate(true)))
	coupling.coupling_failed.connect(func(error: Error, stage: String):
		if coupling == _river_reach_coupling:
			river_reach_coupling_failed.emit(error, stage))
	coupling.released.connect(func():
		if coupling == _river_reach_coupling:
			_river_reach_coupling = null
		if _deferred_sparse_release_for_coupling:
			_deferred_sparse_release_for_coupling = false
			call_deferred(&"_continue_sparse_release_after_coupling"))
	var err := coupling.initialize(
		PersistentHydrologySystem.store() as PlanetHydrologyRiverCoupledStore, runtime)
	if err != OK:
		river_reach_coupling_failed.emit(err, "coupling_initialize")
		_release_river_reach_coupling()


func _on_coupled_store_rebuilt() -> void:
	_release_river_reach_coupling()
	call_deferred(&"_try_bind_river_reach_coupling")


func _release_river_reach_coupling() -> void:
	var coupling := _river_reach_coupling
	if coupling == null:
		return
	coupling.release()
	if not coupling.pending():
		_river_reach_coupling = null
		if is_instance_valid(coupling): coupling.queue_free()


func _release_sparse_runtime() -> void:
	if _river_reach_coupling != null and _river_reach_coupling.pending():
		_deferred_sparse_release_for_coupling = true
		_river_reach_coupling.release()
		return
	_release_river_reach_coupling()
	super._release_sparse_runtime()


func _continue_sparse_release_after_coupling() -> void:
	if _river_reach_coupling != null:
		return
	super._release_sparse_runtime()
