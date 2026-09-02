extends "res://scripts/water/persistent_hydrology_system.gd"
## Production PersistentHydrologySystem facade selecting the reach-aware ownership
## store while preserving the existing autoload API and weather/transaction logic.


func _rebuild_store() -> void:
	if not Planet.ready_state or Planet.fields == null or Planet.grid == null:
		return
	var next_store := PlanetHydrologyRiverPromotionStore.new()
	var err := next_store.initialize(Planet.fields)
	if err != OK:
		push_error("PersistentHydrologySystem: reach store initialization failed (%d)." % int(err))
		return
	_store = next_store
	_precipitation_native_snapshot_mps = PackedFloat64Array()
	_precipitation_native_snapshot_mps.resize(Planet.grid.cell_count)
	_precipitation_native_snapshot_mps.fill(0.0)
	_precipitation_snapshot_mps = PackedFloat64Array()
	_precipitation_snapshot_mps.resize(Planet.grid.cell_count)
	_precipitation_snapshot_mps.fill(0.0)
	_precipitation_authority_fractions = PackedFloat64Array()
	_precipitation_authority_fractions.resize(Planet.grid.cell_count)
	_precipitation_authority_fractions.fill(1.0)
	_time_debt_s = 0.0
	_last_simulation_seconds = CelestialSystem.simulation_seconds
	_last_weather_sample_sim_s = -1.0e30
	_last_weather_revision = -1
	_weather_native_revision_upper_bound = -1
	_weather_snapshot_valid = false
	_store.set_climatology_fallback_enabled(true)
	_refresh_weather_snapshot(_last_simulation_seconds)
	store_ready.emit()
	store_rebuilt.emit()


func channel_reach_candidates(max_count: int = 64,
		discharge_ratio_threshold: float = 2.0,
		bankfull_ratio_threshold: float = 0.85) -> Array[Dictionary]:
	if _store == null or not (_store is PlanetHydrologyReachOwnershipStore):
		return []
	return (_store as PlanetHydrologyReachOwnershipStore).channel_reach_candidates(
		max_count, discharge_ratio_threshold, bankfull_ratio_threshold)


func river_reach_state(cell: int) -> Dictionary:
	if _store == null or not (_store is PlanetHydrologyReachOwnershipStore):
		return {}
	var reach_store := _store as PlanetHydrologyReachOwnershipStore
	if reach_store.river_reaches == null:
		return {}
	return reach_store.river_reaches.reach_state(cell, reach_store.channel_storage_m3[cell]) \
		if cell >= 0 and cell < reach_store.cell_count() else {}


func suggested_channel_tile_volume_m3(cell: int, fine_tile_span_m: float) -> float:
	if _store == null or not (_store is PlanetHydrologyRiverPromotionStore):
		return 0.0
	return (_store as PlanetHydrologyRiverPromotionStore).suggested_channel_tile_volume_m3(
		cell, fine_tile_span_m)
