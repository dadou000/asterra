extends "res://scripts/water/water_system_river_coupled.gd"
## Select the teardown-hardened, component-aware coupling implementation without
## duplicating the WaterSystem river ownership/public API layer.


func _try_bind_river_reach_coupling() -> void:
	if _river_reach_coupling != null or not sparse_runtime_available() \
			or not PersistentHydrologySystem.available() \
			or not (PersistentHydrologySystem.store() is PlanetHydrologyRiverClusterStore):
		return
	var runtime := sparse_runtime()
	if runtime == null or not runtime.initialized_ok():
		return
	var coupling := HydroRiverComponentCoupling.new()
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
		PersistentHydrologySystem.store() as PlanetHydrologyRiverClusterStore, runtime)
	if err != OK:
		river_reach_coupling_failed.emit(err, "coupling_initialize")
		_release_river_reach_coupling()
