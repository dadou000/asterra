extends Node
## Production bridge from WeatherSystem's published global precipitation texture
## into the sparse SWE atmospheric source layer.
##
## The bridge is deliberately outside SparseHydrologyRuntime. It updates only at
## an idle boundary, preferentially from `cycle_completed`: SparseHydrologyRuntime
## has already returned to IDLE there and defers its next pump, so the atmospheric
## compute dispatch is recorded before the next SWE command chain. One macro cycle
## therefore sees one coherent weather-forcing snapshot.
##
## The atmospheric buffer is indexed by transient atlas slot, so every completed
## hydro cycle forces a refresh. This is also an identity-safety rule: a slot that
## was released/recycled during the cycle cannot carry its former owner's weather
## value into the next solve.
##
## Precipitation authority is spatially exclusive. Before the first fine forcing
## dispatch is submitted, PersistentHydrologySystem synchronously relinquishes each
## solver-visible fine footprint from coarse native precipitation. Only after that
## ownership handoff succeeds is the GPU update queued. If the first update fails,
## coarse authority is restored. Later refresh failures keep authority because the
## previously published fine forcing buffer remains valid. Disable/native failure/
## runtime teardown always return coarse authority before clearing fine forcing.

signal coupling_ready
signal coupling_updated(request_id: int)
signal coupling_failed(error: Error)
signal fine_precipitation_authority_changed(active: bool)

var enabled := true
var refresh_interval_s := 0.25
var maximum_precipitation_mm_h := 30.0
var land_infiltration_capacity_mm_h := 4.0
var weather_gain := 1.0
var land_bed_threshold_m := 0.0
var min_output_mm_h := 0.001

var _runtime: SparseHydrologyRuntime
var _forcing: HydroWeatherForcingGPU
var _ownership_map: HydroCoarseFineOwnershipMap
var _refresh_accum := 999.0
var _ready_coupling := false
var _forcing_published_once := false
## True once coarse has relinquished the mapped fine footprints. This may become
## true immediately before the first GPU forcing dispatch is recorded; that brief
## reservation closes the submit/callback race without double-injecting rainfall.
var _authority_claimed := false
var _last_authority_active := false
var _update_requested := true
var _updates_recorded := 0
var _last_request_id := -1


func _ready() -> void:
	process_priority = 20
	if not WaterSystem.sparse_runtime_ready.is_connected(_on_sparse_runtime_ready):
		WaterSystem.sparse_runtime_ready.connect(_on_sparse_runtime_ready)
	if not WaterSystem.sparse_runtime_state_changed.is_connected(_on_sparse_state_changed):
		WaterSystem.sparse_runtime_state_changed.connect(_on_sparse_state_changed)
	if WeatherSystem.has_signal(&"simulation_weight_changed") \
			and not WeatherSystem.simulation_weight_changed.is_connected(_on_weather_weight_changed):
		WeatherSystem.simulation_weight_changed.connect(_on_weather_weight_changed)
	if not WeatherSystem.native_ready.is_connected(_on_weather_native_ready):
		WeatherSystem.native_ready.connect(_on_weather_native_ready)
	if not WeatherSystem.native_failed.is_connected(_on_weather_native_failed):
		WeatherSystem.native_failed.connect(_on_weather_native_failed)
	if not PersistentHydrologySystem.store_rebuilt.is_connected(_on_coarse_store_rebuilt):
		PersistentHydrologySystem.store_rebuilt.connect(_on_coarse_store_rebuilt)
	call_deferred("_try_bind")


func available() -> bool:
	return _ready_coupling and _forcing != null and _forcing.initialized_ok() \
		and _runtime != null and _runtime.initialized_ok()


## True when coarse distributed precipitation has relinquished the current fine
## footprints. During the first update this becomes true immediately before the
## fine GPU forcing dispatch is queued, rather than waiting for its async callback.
func fine_precipitation_authority_active() -> bool:
	return enabled and available() and _authority_claimed \
		and bool(WeatherSystem.native_available) and _ownership_map != null \
		and _ownership_map.initialized_ok()


func request_refresh() -> void:
	_update_requested = true
	_refresh_accum = maxf(_refresh_accum, maxf(refresh_interval_s, 0.01))


func set_enabled(value: bool) -> void:
	if enabled == value:
		return
	enabled = value
	_forcing_published_once = false
	_authority_claimed = false
	if not enabled:
		# Coarse must reclaim rainfall before the fine atmospheric buffer is cleared.
		_sync_precipitation_authority()
		_clear_runtime_forcing()
	else:
		_sync_precipitation_authority()
		request_refresh()


func stats() -> Dictionary:
	return {
		"available": available(),
		"enabled": enabled,
		"fine_precipitation_authority": fine_precipitation_authority_active(),
		"authority_claimed": _authority_claimed,
		"forcing_published_once": _forcing_published_once,
		"refresh_interval_s": refresh_interval_s,
		"updates_recorded": _updates_recorded,
		"last_request_id": _last_request_id,
		"pending_refresh": _update_requested,
		"ownership_map": {} if _ownership_map == null else _ownership_map.stats(),
		"forcing": {} if _forcing == null else _forcing.stats(),
	}


func _process(delta: float) -> void:
	if not is_finite(delta) or delta < 0.0:
		return
	_refresh_accum += delta
	# Fallback for an idle runtime with no active hydro cycle. Active runtimes refresh
	# unconditionally from _on_runtime_cycle_completed for slot-identity safety.
	_try_refresh_now(false)


func _on_sparse_runtime_ready() -> void:
	_try_bind()


func _on_sparse_state_changed(state: String) -> void:
	if state != "ready":
		_teardown()
	else:
		call_deferred("_try_bind")


func _on_weather_weight_changed(_weight: float) -> void:
	request_refresh()


func _on_weather_native_ready() -> void:
	# Rebind in case the published weather texture RID was recreated.
	_teardown()
	call_deferred("_try_bind")


func _on_weather_native_failed(_reason: String) -> void:
	_forcing_published_once = false
	_authority_claimed = false
	# Reclaim authority before removing the fine atmospheric contribution.
	_sync_precipitation_authority()
	_clear_runtime_forcing()
	_update_requested = true


func _on_coarse_store_rebuilt() -> void:
	# WaterSystem treats a coarse-store rebuild as a sparse generation boundary.
	# Tear down this coupling immediately as well, so no old atlas can claim forcing
	# authority from the replacement coarse store while the runtime is recycling.
	_teardown()
	call_deferred("_try_bind")


func _on_runtime_cycle_completed(_cycle_id: int, _report: Dictionary) -> void:
	_try_refresh_now(true)


func _try_refresh_now(force_cycle_refresh: bool = false) -> void:
	if not enabled or not bool(WeatherSystem.native_available) \
			or not available() or _forcing.pending() or _runtime.busy():
		return
	if _ownership_map == null or not _ownership_map.initialized_ok() \
			or not PersistentHydrologySystem.available():
		# Fine forcing cannot become authoritative until the matching coarse owner can
		# relinquish the same spatial footprint.
		return
	if not force_cycle_refresh and not _update_requested \
			and _refresh_accum < maxf(refresh_interval_s, 0.01):
		return
	_apply_policy()

	var claimed_for_first_submit := false
	if not _forcing_published_once and not _authority_claimed:
		_authority_claimed = true
		var authority_error := _sync_precipitation_authority()
		if authority_error != OK:
			_authority_claimed = false
			_sync_precipitation_authority()
			return
		claimed_for_first_submit = true

	var request_id := _forcing.request_update()
	if request_id < 0:
		if claimed_for_first_submit:
			_authority_claimed = false
			_sync_precipitation_authority()
		return
	_last_request_id = request_id
	_update_requested = false
	_refresh_accum = 0.0


func _try_bind() -> void:
	if WaterSystem.sparse_runtime_state() != "ready":
		return
	var runtime := WaterSystem.sparse_runtime()
	if runtime == null or not runtime.initialized_ok() or runtime.atlas == null \
			or runtime.solver == null or not runtime.solver.initialized_ok():
		return
	var weather_texture := WeatherSystem.global_texture()
	if weather_texture == null or not bool(WeatherSystem.native_available):
		return
	if _runtime == runtime and _forcing != null:
		_try_bind_ownership_map()
		return

	_teardown()
	_runtime = runtime
	if not _runtime.cycle_completed.is_connected(_on_runtime_cycle_completed):
		_runtime.cycle_completed.connect(_on_runtime_cycle_completed)
	_forcing = HydroWeatherForcingGPU.new()
	_forcing.name = "HydroWeatherForcingGPU"
	add_child(_forcing)
	_forcing.initialized.connect(_on_forcing_initialized)
	_forcing.initialization_failed.connect(_on_forcing_init_failed)
	_forcing.update_recorded.connect(_on_forcing_update_recorded)
	_forcing.update_failed.connect(_on_forcing_update_failed)
	_apply_policy()
	var err := _forcing.initialize(runtime.atlas,
		runtime.solver.atmospheric_source_rid(), weather_texture)
	if err != OK:
		_fail(err)


func _try_bind_ownership_map() -> void:
	if _ownership_map != null or _runtime == null or not _runtime.initialized_ok() \
			or _runtime.scheduler == null or _runtime.atlas == null \
			or not PersistentHydrologySystem.available():
		return
	var tile_area := _runtime.atlas.cell_size_m * _runtime.atlas.cell_size_m \
		* float(_runtime.atlas.cells_per_tile())
	var ownership := HydroCoarseFineOwnershipMap.new()
	var err := ownership.initialize(PersistentHydrologySystem.store(),
		_runtime.scheduler, tile_area)
	if err != OK:
		coupling_failed.emit(err)
		push_warning("HydroWeatherCoupling: ownership map failed (%d)." % int(err))
		return
	_ownership_map = ownership
	_ownership_map.fractions_changed.connect(_on_ownership_fractions_changed)
	if _authority_claimed:
		_sync_precipitation_authority()


func _apply_policy() -> void:
	if _forcing == null:
		return
	_forcing.maximum_precipitation_mm_h = maxf(maximum_precipitation_mm_h, 0.0)
	_forcing.land_infiltration_capacity_mm_h = maxf(
		land_infiltration_capacity_mm_h, 0.0)
	_forcing.weather_gain = maxf(weather_gain, 0.0)
	_forcing.land_bed_threshold_m = land_bed_threshold_m
	_forcing.min_output_mm_h = maxf(min_output_mm_h, 0.0)


func _on_forcing_initialized() -> void:
	_ready_coupling = true
	_forcing_published_once = false
	_authority_claimed = false
	_try_bind_ownership_map()
	_sync_precipitation_authority()
	request_refresh()
	_try_refresh_now(false)
	coupling_ready.emit()


func _on_forcing_init_failed(error: Error) -> void:
	_fail(error)


func _on_forcing_update_recorded(request_id: int) -> void:
	_updates_recorded += 1
	_forcing_published_once = true
	# Defensive recovery only: the first submit path claims coarse authority before
	# queuing the GPU dispatch, so a recorded update should never arrive unclaimed.
	if not _authority_claimed:
		_authority_claimed = true
		_sync_precipitation_authority()
	coupling_updated.emit(request_id)


func _on_forcing_update_failed(_request_id: int, error: Error) -> void:
	# If no forcing snapshot has ever succeeded, the fine side owns nothing usable;
	# return the pre-submit authority reservation. Once a snapshot has succeeded,
	# keep authority because the previous atmospheric buffer remains authoritative.
	if not _forcing_published_once and _authority_claimed:
		_authority_claimed = false
		_sync_precipitation_authority()
	coupling_failed.emit(error)
	push_warning("HydroWeatherCoupling: precipitation update failed (%d)." % int(error))
	_update_requested = true


func _on_ownership_fractions_changed(_fractions: PackedFloat64Array) -> void:
	if _authority_claimed:
		_sync_precipitation_authority()


func _sync_precipitation_authority() -> Error:
	var active := fine_precipitation_authority_active()
	var result := OK
	if PersistentHydrologySystem.available():
		if active:
			result = PersistentHydrologySystem.set_precipitation_authority_fractions(
				_ownership_map.coarse_precipitation_fractions())
			if result != OK:
				coupling_failed.emit(result)
				push_warning("HydroWeatherCoupling: coarse authority update failed (%d)." % int(result))
		else:
			PersistentHydrologySystem.clear_precipitation_authority_fractions()
	elif active:
		result = ERR_UNCONFIGURED
	if active != _last_authority_active:
		_last_authority_active = active
		fine_precipitation_authority_changed.emit(active)
	return result


func _clear_runtime_forcing() -> void:
	if _runtime != null and is_instance_valid(_runtime) and _runtime.solver != null \
			and is_instance_valid(_runtime.solver):
		_runtime.solver.clear_atmospheric_sources()


func _fail(error: Error) -> void:
	coupling_failed.emit(error)
	push_warning("HydroWeatherCoupling: initialization failed (%d)." % int(error))
	_teardown()


func _release_ownership_map() -> void:
	var ownership := _ownership_map
	_ownership_map = null
	if ownership != null:
		if ownership.fractions_changed.is_connected(_on_ownership_fractions_changed):
			ownership.fractions_changed.disconnect(_on_ownership_fractions_changed)
		ownership.release()


func _teardown() -> void:
	# Ordering matters: coarse takes rainfall authority back before fine forcing is
	# cleared and before the ownership map loses the footprint information.
	_forcing_published_once = false
	_authority_claimed = false
	_sync_precipitation_authority()
	_ready_coupling = false
	_clear_runtime_forcing()
	_release_ownership_map()
	if _runtime != null and is_instance_valid(_runtime) \
			and _runtime.cycle_completed.is_connected(_on_runtime_cycle_completed):
		_runtime.cycle_completed.disconnect(_on_runtime_cycle_completed)
	if _forcing != null and is_instance_valid(_forcing):
		_forcing.release()
		_forcing.queue_free()
	_forcing = null
	_runtime = null
	_update_requested = true
	_refresh_accum = 999.0
	_last_request_id = -1


func _exit_tree() -> void:
	if WaterSystem.sparse_runtime_ready.is_connected(_on_sparse_runtime_ready):
		WaterSystem.sparse_runtime_ready.disconnect(_on_sparse_runtime_ready)
	if WaterSystem.sparse_runtime_state_changed.is_connected(_on_sparse_state_changed):
		WaterSystem.sparse_runtime_state_changed.disconnect(_on_sparse_state_changed)
	if WeatherSystem.has_signal(&"simulation_weight_changed") \
			and WeatherSystem.simulation_weight_changed.is_connected(_on_weather_weight_changed):
		WeatherSystem.simulation_weight_changed.disconnect(_on_weather_weight_changed)
	if WeatherSystem.native_ready.is_connected(_on_weather_native_ready):
		WeatherSystem.native_ready.disconnect(_on_weather_native_ready)
	if WeatherSystem.native_failed.is_connected(_on_weather_native_failed):
		WeatherSystem.native_failed.disconnect(_on_weather_native_failed)
	if PersistentHydrologySystem.store_rebuilt.is_connected(_on_coarse_store_rebuilt):
		PersistentHydrologySystem.store_rebuilt.disconnect(_on_coarse_store_rebuilt)
	_teardown()