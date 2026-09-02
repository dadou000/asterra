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

signal coupling_ready
signal coupling_updated(request_id: int)
signal coupling_failed(error: Error)

var enabled := true
var refresh_interval_s := 0.25
var maximum_precipitation_mm_h := 30.0
var land_infiltration_capacity_mm_h := 4.0
var weather_gain := 1.0
var land_bed_threshold_m := 0.0
var min_output_mm_h := 0.001

var _runtime: SparseHydrologyRuntime
var _forcing: HydroWeatherForcingGPU
var _refresh_accum := 999.0
var _ready_coupling := false
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
	call_deferred("_try_bind")


func available() -> bool:
	return _ready_coupling and _forcing != null and _forcing.initialized_ok() \
		and _runtime != null and _runtime.initialized_ok()


func request_refresh() -> void:
	_update_requested = true
	_refresh_accum = maxf(_refresh_accum, maxf(refresh_interval_s, 0.01))


func set_enabled(value: bool) -> void:
	if enabled == value:
		return
	enabled = value
	if not enabled:
		_clear_runtime_forcing()
	else:
		request_refresh()


func stats() -> Dictionary:
	return {
		"available": available(),
		"enabled": enabled,
		"refresh_interval_s": refresh_interval_s,
		"updates_recorded": _updates_recorded,
		"last_request_id": _last_request_id,
		"pending_refresh": _update_requested,
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


func _on_runtime_cycle_completed(_cycle_id: int, _report: Dictionary) -> void:
	_try_refresh_now(true)


func _try_refresh_now(force_cycle_refresh: bool = false) -> void:
	if not enabled or not available() or _forcing.pending() or _runtime.busy():
		return
	if not force_cycle_refresh and not _update_requested \
			and _refresh_accum < maxf(refresh_interval_s, 0.01):
		return
	_apply_policy()
	var request_id := _forcing.request_update()
	if request_id < 0:
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
	if weather_texture == null:
		return
	if _runtime == runtime and _forcing != null:
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
	request_refresh()
	_try_refresh_now(false)
	coupling_ready.emit()


func _on_forcing_init_failed(error: Error) -> void:
	_fail(error)


func _on_forcing_update_recorded(request_id: int) -> void:
	_updates_recorded += 1
	coupling_updated.emit(request_id)


func _on_forcing_update_failed(_request_id: int, error: Error) -> void:
	coupling_failed.emit(error)
	push_warning("HydroWeatherCoupling: precipitation update failed (%d)." % int(error))
	_update_requested = true


func _clear_runtime_forcing() -> void:
	if _runtime != null and is_instance_valid(_runtime) and _runtime.solver != null \
			and is_instance_valid(_runtime.solver):
		_runtime.solver.clear_atmospheric_sources()


func _fail(error: Error) -> void:
	coupling_failed.emit(error)
	push_warning("HydroWeatherCoupling: initialization failed (%d)." % int(error))
	_teardown()


func _teardown() -> void:
	_ready_coupling = false
	_clear_runtime_forcing()
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
	_teardown()
