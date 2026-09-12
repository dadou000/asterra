class_name HydroRiverReachCoupling
extends Node
## Continuous operator-split coupling for explicitly promoted river reaches.
##
## At every completed sparse SWE macro cycle:
##   coarse pending donor/confluence water -> fine upstream corridor mouth
##   fine measured downstream advective parcel -> coarse residual 1D reach
##
## The sparse runtime and coarse owner are paused only while the compact GPU result
## is pending, so the state and the corresponding ownership ledger are reconciled at
## one stable representation boundary. Automatic channel promotion remains policy-
## disabled; this controller only services reaches that were explicitly promoted.

signal initialized
signal initialization_failed(error: Error)
signal reach_registered(cell: int, tile_id: int, slot: int)
signal exchange_completed(request_id: int, report: Dictionary)
signal coupling_failed(error: Error, stage: String)
signal released

var max_records := 256
var mouth_cells := 1.25

var store: PlanetHydrologyRiverCoupledStore
var runtime: SparseHydrologyRuntime
var exchange_gpu: HydroRiverReachExchangeGPU

var _records: Dictionary = {} # cell -> Dictionary
var _initialized := false
var _pending_request_id := -1
var _pending_advanced_dt_s := 0.0
var _runtime_was_enabled := true
var _coarse_was_enabled := true
var _failed := false
var _release_requested := false


func initialize(p_store: PlanetHydrologyRiverCoupledStore,
		p_runtime: SparseHydrologyRuntime) -> Error:
	if _initialized or exchange_gpu != null:
		return ERR_BUSY
	if p_store == null or not p_store.initialized or p_runtime == null \
			or not p_runtime.initialized_ok() or p_runtime.atlas == null:
		return ERR_UNCONFIGURED
	store = p_store
	runtime = p_runtime
	if not runtime.cycle_completed.is_connected(_on_runtime_cycle_completed):
		runtime.cycle_completed.connect(_on_runtime_cycle_completed)

	exchange_gpu = HydroRiverReachExchangeGPU.new()
	exchange_gpu.name = "HydroRiverReachExchangeGPU"
	add_child(exchange_gpu)
	exchange_gpu.initialized.connect(_on_exchange_initialized)
	exchange_gpu.initialization_failed.connect(_on_exchange_initialization_failed)
	exchange_gpu.exchange_ready.connect(_on_exchange_ready)
	exchange_gpu.exchange_failed.connect(_on_exchange_failed)
	var err := exchange_gpu.initialize(runtime.atlas, max_records)
	if err != OK:
		_disconnect_runtime()
		exchange_gpu.queue_free()
		exchange_gpu = null
		store = null
		runtime = null
		return err
	return OK


func initialized_ok() -> bool:
	return _initialized and not _failed


func pending() -> bool:
	return _pending_request_id >= 0


func failed() -> bool:
	return _failed


func registered_count() -> int:
	return _records.size()


## Register the exact tile/corridor emitted by PlanetRiverReachPromotionBridge.
func register_promoted_reach(report: Dictionary) -> Error:
	if not initialized_ok() or pending():
		return ERR_BUSY
	var cell := int(report.get("cell", -1))
	var tile_id := int(report.get("tile_id", -1))
	var slot := int(report.get("slot", -1))
	var represented := float(report.get("represented_volume_m3", 0.0))
	var center_value: Variant = report.get("corridor_center_cell", null)
	var direction_value: Variant = report.get("corridor_direction_cell", null)
	var velocity_value: Variant = report.get("local_velocity_mps", null)
	if cell < 0 or tile_id < 0 or slot < 0 or represented <= 0.0 \
			or not (center_value is Vector2) or not (direction_value is Vector2) \
			or not (velocity_value is Vector2):
		return ERR_INVALID_PARAMETER
	var center := center_value as Vector2
	var direction := direction_value as Vector2
	var velocity := velocity_value as Vector2
	if direction.length_squared() <= 1.0e-12:
		return ERR_INVALID_PARAMETER
	var registration := store.register_refined_reach(cell, tile_id, slot, represented)
	if int(registration.get("error", FAILED)) != OK:
		return int(registration.get("error", ERR_INVALID_DATA))
	_records[cell] = {
		"cell": cell,
		"tile_id": tile_id,
		"slot": slot,
		"center_cell": center,
		"direction_cell": direction.normalized(),
		"half_width_m": maxf(float(report.get("corridor_half_width_m", 0.0)),
			runtime.atlas.cell_size_m * 0.5),
		"add_velocity": velocity,
		"last_downstream_q_m3s": 0.0,
		"cumulative_added_m3": 0.0,
		"cumulative_removed_m3": 0.0,
	}
	reach_registered.emit(cell, tile_id, slot)
	return OK


func registered_reach(cell: int) -> Dictionary:
	if not _records.has(cell):
		return {}
	return (_records[cell] as Dictionary).duplicate(true)


func stats() -> Dictionary:
	var total_added := 0.0
	var total_removed := 0.0
	for value: Variant in _records.values():
		var rec := value as Dictionary
		total_added += float(rec.get("cumulative_added_m3", 0.0))
		total_removed += float(rec.get("cumulative_removed_m3", 0.0))
	return {
		"initialized": _initialized,
		"failed": _failed,
		"pending": pending(),
		"registered_reaches": _records.size(),
		"cumulative_1d_to_2d_m3": total_added,
		"cumulative_2d_to_1d_m3": total_removed,
		"gpu": {} if exchange_gpu == null else {
			"initialized": exchange_gpu.initialized_ok(),
			"pending": exchange_gpu.pending(),
			"bytes": exchange_gpu.gpu_bytes_estimate(),
		},
	}


func _on_runtime_cycle_completed(_cycle_id: int, report: Dictionary) -> void:
	if not initialized_ok() or pending() or _records.is_empty():
		return
	var dt := maxf(float(report.get("advanced_dt_s", 0.0)), 0.0)
	if dt <= 0.0:
		return
	var exchange_records: Array[Dictionary] = []
	var stale_cells: Array[int] = []
	for cell_variant: Variant in _records.keys():
		var cell := int(cell_variant)
		var rec := _records[cell] as Dictionary
		var tile_id := int(rec.get("tile_id", -1))
		var slot := int(rec.get("slot", -1))
		var key := HydroTileKey.unpack(tile_id)
		if runtime.scheduler == null or runtime.scheduler.pool == null \
				or not runtime.scheduler.pool.contains(key) \
				or runtime.scheduler.pool.slot_for(key) != slot:
			stale_cells.append(cell)
			continue
		var pending_volume := store.refined_inflow_available_m3(cell)
		var rate := store.refined_inflow_rate(cell)
		var add_volume := minf(pending_volume, maxf(rate, 0.0) * dt)
		exchange_records.append({
			"cell": cell,
			"slot": slot,
			"center_cell": rec["center_cell"],
			"direction_cell": rec["direction_cell"],
			"half_width_m": rec["half_width_m"],
			"add_volume_m3": add_volume,
			"exchange_dt_s": dt,
			"mouth_cells": mouth_cells,
			"add_velocity": rec["add_velocity"],
		})
	for cell in stale_cells:
		# Metadata is stale only if another subsystem removed the fine tile. Pending
		# coarse inflow remains coarse-owned and is returned to channel storage.
		store.unregister_refined_reach(cell, true)
		_records.erase(cell)
	if exchange_records.is_empty():
		return

	_runtime_was_enabled = runtime.enabled
	_coarse_was_enabled = bool(PersistentHydrologySystem.enabled)
	runtime.enabled = false
	PersistentHydrologySystem.enabled = false
	_pending_advanced_dt_s = dt
	_pending_request_id = exchange_gpu.exchange(exchange_records)
	if _pending_request_id < 0:
		_fail_closed(ERR_BUSY, "exchange_submit")


func _on_exchange_ready(request_id: int, results: Array[Dictionary]) -> void:
	if request_id != _pending_request_id:
		return
	var invalid_status := false
	var total_added := 0.0
	var total_removed := 0.0
	var max_q := 0.0
	for result in results:
		var cell := int(result.get("cell", -1))
		if not _records.has(cell):
			invalid_status = true
			continue
		var status := float(result.get("mouth_or_status", -1.0))
		if status < 0.0:
			invalid_status = true
			continue
		var added := maxf(float(result.get("added_m3", 0.0)), 0.0)
		var removed := maxf(float(result.get("removed_m3", 0.0)), 0.0)
		var q := maxf(float(result.get("measured_downstream_q_m3s", 0.0)), 0.0)
		if added > 0.0:
			var consume := store.consume_refined_inflow(cell, added)
			if int(consume.get("error", FAILED)) != OK:
				_fail_closed(int(consume.get("error", ERR_INVALID_DATA)), "consume_1d_inflow")
				return
		if removed > 0.0:
			var accept := store.accept_refined_outflow(cell, removed)
			if int(accept.get("error", FAILED)) != OK:
				_fail_closed(int(accept.get("error", ERR_INVALID_DATA)), "accept_2d_outflow")
				return
		var rec := _records[cell] as Dictionary
		rec["last_downstream_q_m3s"] = q
		rec["cumulative_added_m3"] = float(rec.get("cumulative_added_m3", 0.0)) + added
		rec["cumulative_removed_m3"] = float(rec.get("cumulative_removed_m3", 0.0)) + removed
		_records[cell] = rec
		total_added += added
		total_removed += removed
		max_q = maxf(max_q, q)

	var completed_id := _pending_request_id
	_pending_request_id = -1
	var report := {
		"request_id": completed_id,
		"advanced_dt_s": _pending_advanced_dt_s,
		"reach_count": results.size(),
		"added_1d_to_2d_m3": total_added,
		"removed_2d_to_1d_m3": total_removed,
		"max_measured_downstream_q_m3s": max_q,
		"invalid_record_status": invalid_status,
	}
	_pending_advanced_dt_s = 0.0
	if invalid_status:
		_fail_closed(ERR_INVALID_DATA, "exchange_record_status")
		return
	_restore_owners_and_pump()
	exchange_completed.emit(completed_id, report)
	if _release_requested:
		_finish_release()


func _on_exchange_failed(request_id: int, error: Error) -> void:
	if request_id == _pending_request_id:
		_fail_closed(error, "exchange_gpu")


func _fail_closed(error: Error, stage: String) -> void:
	_failed = true
	_pending_request_id = -1
	_pending_advanced_dt_s = 0.0
	# The exact GPU/coarse transfer result is unknown or could not be reconciled.
	# Keep both owners paused rather than allowing a mass-ambiguous generation on.
	if runtime != null:
		runtime.enabled = false
	PersistentHydrologySystem.enabled = false
	coupling_failed.emit(error, stage)


func _restore_owners_and_pump() -> void:
	if runtime != null:
		runtime.enabled = _runtime_was_enabled
	PersistentHydrologySystem.enabled = _coarse_was_enabled
	if runtime != null and runtime.enabled:
		runtime.advance_time(0.0)


func _on_exchange_initialized() -> void:
	_initialized = true
	initialized.emit()


func _on_exchange_initialization_failed(error: Error) -> void:
	initialization_failed.emit(error)


func _disconnect_runtime() -> void:
	if runtime != null and runtime.cycle_completed.is_connected(_on_runtime_cycle_completed):
		runtime.cycle_completed.disconnect(_on_runtime_cycle_completed)


func release() -> void:
	if _release_requested:
		return
	_release_requested = true
	_disconnect_runtime()
	if pending():
		# Let the in-flight compact readback reconcile ownership first.
		return
	_finish_release()


func _finish_release() -> void:
	if exchange_gpu != null and is_instance_valid(exchange_gpu):
		exchange_gpu.release()
		exchange_gpu.queue_free()
	exchange_gpu = null
	_initialized = false
	_records.clear()
	store = null
	runtime = null
	_release_requested = false
	released.emit()


func _exit_tree() -> void:
	release()
