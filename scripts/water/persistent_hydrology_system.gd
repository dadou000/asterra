extends Node
## Production owner for the coarse planet-wide hydrology representation.
##
## WeatherSystem keeps the native global CPU weather array unweighted while its
## published GPU texture applies `simulation_weight`. This system samples the raw
## CPU array at PlanetGrid cell centres and applies that same weight explicitly so
## coarse planetary hydrology and resident fine SWE see the same precipitation.
## No GPU readback is involved.
##
## The coarse store intentionally runs much slower than fine SWE. Fine resident
## tiles get current weather directly through HydroWeatherCoupling; this background
## representation preserves rainfall/runoff/channel state everywhere else and is
## the source of promotion requests.
##
## Coarse -> fine ownership transfer is transactional. prepare_promotion() only
## reserves water. The caller must seed a fine representation and acknowledge it
## with commit_promotion(); failures call rollback_promotion(). The coarse store
## never debits water merely because a tile allocation was attempted.

signal store_ready
signal store_rebuilt
## Local committed forcing-snapshot sequence, not WeatherSystem's native solver
## revision. The global CPU publication is asynchronous and may legitimately lag
## that native revision.
signal weather_snapshot_updated(weather_revision: int)
signal coarse_step_completed(report: Dictionary)
signal promotion_prepared(transaction: Dictionary)
signal promotion_committed(transaction: Dictionary)
signal promotion_rolled_back(transaction: Dictionary)
signal demotion_accepted(transfer: Dictionary)

const WEATHER_CHANNELS := 4
const WEATHER_PRECIP_CHANNEL := 2
const MM_H_TO_M_S := 1.0e-3 / 3600.0

var enabled := true
var target_step_s := 60.0
var max_step_s := 3600.0
var max_steps_per_frame := 8
var weather_sample_interval_sim_s := 300.0
var maximum_precipitation_mm_h := 30.0
var weather_gain := 1.0

var _store: PlanetHydrologyOwnershipStore
var _precipitation_snapshot_mps := PackedFloat64Array()
var _time_debt_s := 0.0
var _last_simulation_seconds := 0.0
var _last_weather_sample_sim_s := -1.0e30
## Local publication sequence. Do not equate this with global_state_revision.
var _last_weather_revision := -1
var _weather_native_revision_upper_bound := -1
var _weather_snapshot_valid := false
var _weather_snapshot_count := 0
var _coarse_steps := 0


func _ready() -> void:
	process_priority = 19
	_last_simulation_seconds = CelestialSystem.simulation_seconds
	if not Planet.world_ready.is_connected(_on_world_ready):
		Planet.world_ready.connect(_on_world_ready)
	if not WeatherSystem.simulation_weight_changed.is_connected(_on_simulation_weight_changed):
		WeatherSystem.simulation_weight_changed.connect(_on_simulation_weight_changed)
	if not WeatherSystem.native_ready.is_connected(_on_weather_availability_changed):
		WeatherSystem.native_ready.connect(_on_weather_availability_changed)
	if not WeatherSystem.native_failed.is_connected(_on_weather_native_failed):
		WeatherSystem.native_failed.connect(_on_weather_native_failed)
	if Planet.ready_state and Planet.fields != null:
		call_deferred("_rebuild_store")


func available() -> bool:
	return _store != null and _store.initialized


func store() -> PlanetHydrologyOwnershipStore:
	return _store


func request_weather_snapshot() -> void:
	_last_weather_sample_sim_s = -1.0e30


func cell_state(cell: int) -> Dictionary:
	return {} if _store == null else _store.cell_state(cell)


func promotion_candidates(max_count: int = 64,
		surface_depth_threshold_m: float = 0.025,
		discharge_ratio_threshold: float = 2.0) -> Array[Dictionary]:
	if _store == null:
		return []
	return _store.promotion_candidates(max_count,
		surface_depth_threshold_m, discharge_ratio_threshold)


## Reserve free coarse water without changing physical storage. The returned
## transaction is the seed contract for the fine representation.
func prepare_promotion(cell: int, requested_volume_m3: float) -> Dictionary:
	if _store == null:
		return {"error": ERR_UNCONFIGURED, "reason": "store_unconfigured"}
	var result := _store.prepare_promotion(cell, requested_volume_m3)
	if int(result.get("error", FAILED)) == OK:
		promotion_prepared.emit(result.duplicate(true))
	return result


## Acknowledge that the fine side accepted the exact reserved seed volume. This
## is the only path that removes the transaction's water from coarse storage.
func commit_promotion(transaction_id: int) -> Dictionary:
	if _store == null:
		return {"error": ERR_UNCONFIGURED, "reason": "store_unconfigured"}
	var result := _store.commit_promotion(transaction_id)
	if int(result.get("error", FAILED)) == OK:
		promotion_committed.emit(result.duplicate(true))
	return result


## Cancel a failed fine allocation/seed. Because prepare never debits coarse
## storage, rollback only releases the reservation.
func rollback_promotion(transaction_id: int) -> Dictionary:
	if _store == null:
		return {"error": ERR_UNCONFIGURED, "reason": "store_unconfigured"}
	var result := _store.rollback_promotion(transaction_id)
	if int(result.get("error", FAILED)) == OK:
		promotion_rolled_back.emit(result.duplicate(true))
	return result


## Symmetric fine -> coarse ownership return. The fine caller must remove/reduce
## this exact volume first; this method only accepts the already-conserved parcel.
func accept_demotion(cell: int, surface_volume_m3: float,
		channel_volume_m3: float = 0.0) -> Dictionary:
	if _store == null:
		return {"error": ERR_UNCONFIGURED, "reason": "store_unconfigured"}
	var result := _store.accept_demotion(cell, surface_volume_m3, channel_volume_m3)
	if int(result.get("error", FAILED)) == OK:
		demotion_accepted.emit(result.duplicate(true))
	return result


func snapshot() -> Dictionary:
	return {} if _store == null else _store.snapshot()


func restore_snapshot(data: Dictionary) -> Error:
	return ERR_UNCONFIGURED if _store == null else _store.restore_snapshot(data)


func stats() -> Dictionary:
	return {
		"available": available(),
		"enabled": enabled,
		"target_step_s": target_step_s,
		"max_step_s": max_step_s,
		"max_steps_per_frame": max_steps_per_frame,
		"weather_sample_interval_sim_s": weather_sample_interval_sim_s,
		"maximum_precipitation_mm_h": maximum_precipitation_mm_h,
		"weather_gain": weather_gain,
		"time_debt_s": _time_debt_s,
		"weather_snapshot_count": _weather_snapshot_count,
		"weather_snapshot_valid": _weather_snapshot_valid,
		"weather_sample_revision": _last_weather_revision,
		"weather_native_revision_upper_bound": _weather_native_revision_upper_bound,
		# Compatibility alias: this is now explicitly the local sample revision.
		"last_weather_revision": _last_weather_revision,
		"coarse_steps": _coarse_steps,
		"store": {} if _store == null else _store.stats(),
	}


func _process(_delta: float) -> void:
	var now := CelestialSystem.simulation_seconds
	var simulated_delta := now - _last_simulation_seconds
	_last_simulation_seconds = now
	if simulated_delta < 0.0:
		_time_debt_s = 0.0
		_last_weather_sample_sim_s = -1.0e30
		simulated_delta = 0.0
	if not enabled or not available():
		return

	_time_debt_s += simulated_delta
	_maybe_refresh_weather_snapshot(now)
	_drain_time_debt()


func _on_world_ready(_fields: PlanetFields) -> void:
	call_deferred("_rebuild_store")


func _on_simulation_weight_changed(_weight: float) -> void:
	# The CPU weather array is raw; changing the weight changes our derived forcing
	# even if the native atmospheric state itself did not advance.
	request_weather_snapshot()


func _on_weather_availability_changed() -> void:
	request_weather_snapshot()


func _on_weather_native_failed(_reason: String) -> void:
	request_weather_snapshot()


func _rebuild_store() -> void:
	if not Planet.ready_state or Planet.fields == null or Planet.grid == null:
		return
	var next_store := PlanetHydrologyOwnershipStore.new()
	var err := next_store.initialize(Planet.fields)
	if err != OK:
		push_error("PersistentHydrologySystem: store initialization failed (%d)." % int(err))
		return
	_store = next_store
	_precipitation_snapshot_mps = PackedFloat64Array()
	_precipitation_snapshot_mps.resize(Planet.grid.cell_count)
	_precipitation_snapshot_mps.fill(0.0)
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


func _maybe_refresh_weather_snapshot(simulation_seconds: float) -> void:
	if not available():
		return
	var interval := maxf(weather_sample_interval_sim_s, 1.0)
	if simulation_seconds - _last_weather_sample_sim_s < interval:
		return
	_refresh_weather_snapshot(simulation_seconds)


func _refresh_weather_snapshot(simulation_seconds: float) -> void:
	if not available():
		return
	var n := _store.cell_count()
	if _precipitation_snapshot_mps.size() != n:
		_precipitation_snapshot_mps.resize(n)

	_weather_native_revision_upper_bound = int(WeatherSystem.global_state_revision)
	if not WeatherSystem.native_available:
		_precipitation_snapshot_mps.fill(0.0)
		_store.set_precipitation_field_mps(_precipitation_snapshot_mps)
		_store.set_climatology_fallback_enabled(true)
		_weather_snapshot_valid = false
		_commit_weather_snapshot(simulation_seconds)
		return

	var texture := WeatherSystem.global_texture()
	var values: PackedFloat32Array = WeatherSystem.global_weather_values
	if texture == null:
		# Do not spin every frame while the async global publication is pending. If
		# this world has never received a valid snapshot, keep climatology alive.
		_last_weather_sample_sim_s = simulation_seconds
		_store.set_climatology_fallback_enabled(not _weather_snapshot_valid)
		return
	var width := texture.get_width()
	var height := texture.get_height()
	if width <= 0 or height <= 0 or values.size() < width * height * WEATHER_CHANNELS:
		_last_weather_sample_sim_s = simulation_seconds
		_store.set_climatology_fallback_enabled(not _weather_snapshot_valid)
		return

	var simulation_weight := clampf(float(WeatherSystem.simulation_weight), 0.0, 1.0)
	var scale_mps := maxf(maximum_precipitation_mm_h, 0.0) \
		* MM_H_TO_M_S * maxf(weather_gain, 0.0) * simulation_weight
	for c in n:
		var d: Vector3 = _store.grid.cell_dir(c)
		var normalized_precip := _sample_weather_channel_bilinear(
			values, width, height, d, WEATHER_PRECIP_CHANNEL)
		_precipitation_snapshot_mps[c] = clampf(normalized_precip, 0.0, 1.0) * scale_mps
	_store.set_precipitation_field_mps(_precipitation_snapshot_mps)
	_store.set_climatology_fallback_enabled(false)
	_weather_snapshot_valid = true
	_commit_weather_snapshot(simulation_seconds)


func _commit_weather_snapshot(simulation_seconds: float) -> void:
	_last_weather_sample_sim_s = simulation_seconds
	_last_weather_revision += 1
	_weather_snapshot_count += 1
	weather_snapshot_updated.emit(_last_weather_revision)


func _drain_time_debt() -> void:
	var minimum_step := maxf(target_step_s, 1.0)
	if _time_debt_s < minimum_step:
		return
	var step_cap := maxi(max_steps_per_frame, 1)
	var maximum_step := maxf(max_step_s, minimum_step)
	for step_index in step_cap:
		if _time_debt_s < minimum_step:
			break
		var remaining_slots := maxi(step_cap - step_index, 1)
		var dt := maxf(minimum_step, _time_debt_s / float(remaining_slots))
		dt = minf(dt, maximum_step)
		dt = minf(dt, _time_debt_s)
		var report := _store.step(dt)
		var error := int(report.get("error", FAILED))
		if error != OK:
			# ERR_BUSY is an intentional ownership barrier while a coarse -> fine
			# transaction is awaiting seed acknowledgement. Keep time debt intact and
			# catch up after commit/rollback rather than advancing reserved water.
			if error != ERR_BUSY:
				push_warning("PersistentHydrologySystem: coarse step rejected.")
			break
		_time_debt_s = maxf(_time_debt_s - dt, 0.0)
		_coarse_steps += 1
		coarse_step_completed.emit(report)


## Exact CPU counterpart of WeatherNative's global lat/lon indexing. Longitude
## wraps; latitude clamps at the poles. Values are sampled at texel centres.
func _sample_weather_channel_bilinear(values: PackedFloat32Array,
		width: int, height: int, direction: Vector3, channel: int) -> float:
	var d := direction.normalized()
	var lon := atan2(d.z, d.x)
	if lon < 0.0:
		lon += TAU
	var lat := asin(clampf(d.y, -1.0, 1.0))
	var fx := lon / TAU * float(width) - 0.5
	var fy := (PI * 0.5 - lat) / PI * float(height) - 0.5
	var x0 := int(floor(fx))
	var y0 := int(floor(fy))
	var tx := fx - floor(fx)
	var ty := fy - floor(fy)
	var x1 := x0 + 1
	var y1 := y0 + 1
	x0 = posmod(x0, width)
	x1 = posmod(x1, width)
	y0 = clampi(y0, 0, height - 1)
	y1 = clampi(y1, 0, height - 1)
	var c := clampi(channel, 0, WEATHER_CHANNELS - 1)
	var a := float(values[(x0 + y0 * width) * WEATHER_CHANNELS + c])
	var b := float(values[(x1 + y0 * width) * WEATHER_CHANNELS + c])
	var d0 := float(values[(x0 + y1 * width) * WEATHER_CHANNELS + c])
	var e := float(values[(x1 + y1 * width) * WEATHER_CHANNELS + c])
	return lerpf(lerpf(a, b, tx), lerpf(d0, e, tx), ty)


func _exit_tree() -> void:
	if Planet.world_ready.is_connected(_on_world_ready):
		Planet.world_ready.disconnect(_on_world_ready)
	if WeatherSystem.simulation_weight_changed.is_connected(_on_simulation_weight_changed):
		WeatherSystem.simulation_weight_changed.disconnect(_on_simulation_weight_changed)
	if WeatherSystem.native_ready.is_connected(_on_weather_availability_changed):
		WeatherSystem.native_ready.disconnect(_on_weather_availability_changed)
	if WeatherSystem.native_failed.is_connected(_on_weather_native_failed):
		WeatherSystem.native_failed.disconnect(_on_weather_native_failed)
	_store = null
