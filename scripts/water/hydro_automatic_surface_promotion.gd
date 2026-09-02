class_name HydroAutomaticSurfacePromotionPolicy
extends Node
## Low-cadence policy for promoting persistent coarse *surface flood* water into
## sparse SWE. This node never promotes channel-only anomalies. It delegates every
## physical ownership transfer to WaterSystem/PlanetHydroPromotionBridge.
##
## Policy invariants:
## - disabled unless WaterSystem.automatic_coarse_promotion_enabled is true;
## - at most one coarse->fine transaction is started per timer tick;
## - never starts while any coarse promotion reservation is unresolved;
## - only surface depth participates in candidate selection;
## - hysteresis prevents threshold chatter;
## - cells mapping to an already-resident fine tile are suppressed;
## - requested volume is capped by current coarse surface storage, so automatic
##   promotion never borrows channel storage;
## - initial velocity is zero until the coarse representation owns a trustworthy
##   local 2D flood velocity/direction field.

var scan_interval_s := 3.0
var surface_enter_depth_m := 0.05
var surface_exit_depth_m := 0.025
var candidate_limit := 64
var minimum_parcel_volume_m3 := 1.0e-4

var _timer: Timer
var _latched_cells: Dictionary = {} # cell -> true while above hysteresis exit
var _active_request_id := -1
var _active_cell := -1
var _active_tile_id := -1

var _scan_count := 0
var _promotion_started_count := 0
var _promotion_completed_count := 0
var _promotion_failed_count := 0
var _resident_suppressed_count := 0
var _transaction_busy_suppressed_count := 0
var _last_scan_candidates := 0
var _last_reason := "disabled"
var _last_failure_stage := ""


func _ready() -> void:
	process_priority = 20
	_timer = Timer.new()
	_timer.name = "SurfacePromotionTimer"
	_timer.one_shot = false
	_timer.wait_time = maxf(scan_interval_s, 0.1)
	_timer.timeout.connect(_on_scan_timeout)
	add_child(_timer)
	_timer.start()

	if not WaterSystem.planet_promotion_completed.is_connected(_on_promotion_completed):
		WaterSystem.planet_promotion_completed.connect(_on_promotion_completed)
	if not WaterSystem.planet_promotion_failed.is_connected(_on_promotion_failed):
		WaterSystem.planet_promotion_failed.connect(_on_promotion_failed)
	if not PersistentHydrologySystem.store_rebuilt.is_connected(_on_store_rebuilt):
		PersistentHydrologySystem.store_rebuilt.connect(_on_store_rebuilt)


func stats() -> Dictionary:
	return {
		"enabled": bool(WaterSystem.automatic_coarse_promotion_enabled),
		"scan_interval_s": scan_interval_s,
		"surface_enter_depth_m": surface_enter_depth_m,
		"surface_exit_depth_m": surface_exit_depth_m,
		"candidate_limit": candidate_limit,
		"minimum_parcel_volume_m3": minimum_parcel_volume_m3,
		"scan_count": _scan_count,
		"last_scan_candidates": _last_scan_candidates,
		"latched_cells": _latched_cells.size(),
		"active_request_id": _active_request_id,
		"active_cell": _active_cell,
		"active_tile_id": _active_tile_id,
		"promotions_started": _promotion_started_count,
		"promotions_completed": _promotion_completed_count,
		"promotions_failed": _promotion_failed_count,
		"resident_suppressed": _resident_suppressed_count,
		"transaction_busy_suppressed": _transaction_busy_suppressed_count,
		"last_reason": _last_reason,
		"last_failure_stage": _last_failure_stage,
		"channel_only_promotion_enabled": false,
	}


## Pure hysteresis helper used by the headless policy gate.
static func surface_depth_eligible(depth_m: float, was_latched: bool,
		enter_depth_m: float, exit_depth_m: float) -> bool:
	if not is_finite(depth_m):
		return false
	var enter := maxf(enter_depth_m, 1.0e-6)
	var exit := clampf(exit_depth_m, 0.0, enter)
	if was_latched:
		return depth_m >= exit
	return depth_m >= enter


## Pure parcel cap: automatic surface promotion cannot consume channel storage.
static func surface_only_parcel_m3(suggested_volume_m3: float,
		surface_storage_m3: float, minimum_volume_m3: float = 0.0) -> float:
	if not is_finite(suggested_volume_m3) or not is_finite(surface_storage_m3):
		return 0.0
	var parcel := minf(maxf(suggested_volume_m3, 0.0), maxf(surface_storage_m3, 0.0))
	return parcel if parcel >= maxf(minimum_volume_m3, 0.0) else 0.0


func _on_scan_timeout() -> void:
	if _timer != null:
		_timer.wait_time = maxf(scan_interval_s, 0.1)
	_scan_count += 1
	_last_scan_candidates = 0

	if not bool(WaterSystem.automatic_coarse_promotion_enabled):
		_last_reason = "disabled"
		return
	if _active_request_id >= 0:
		_last_reason = "policy_transaction_active"
		return
	if not PersistentHydrologySystem.available():
		_last_reason = "coarse_store_unavailable"
		return
	if not WaterSystem.planet_promotion_bridge_available():
		_last_reason = "promotion_bridge_unavailable"
		return

	var store := PersistentHydrologySystem.store()
	var bridge := WaterSystem.planet_promotion_bridge()
	if store == null or bridge == null or bridge.scheduler == null \
			or bridge.scheduler.pool == null:
		_last_reason = "promotion_dependencies_unavailable"
		return
	if store.pending_promotion_count() > 0 or bridge.busy():
		_transaction_busy_suppressed_count += 1
		_last_reason = "ownership_transaction_busy"
		return
	var runtime := WaterSystem.sparse_runtime()
	if runtime == null or not runtime.initialized_ok() or runtime.busy():
		_last_reason = "sparse_runtime_busy"
		return

	_prune_hysteresis_latches()
	var enter := maxf(surface_enter_depth_m, 1.0e-6)
	var exit := clampf(surface_exit_depth_m, 0.0, enter)
	# Existing mixed candidate query is forced into a surface-only mode by making
	# the discharge ratio threshold effectively unreachable. Since it is score-sorted,
	# every cell above the higher enter threshold remains ahead of the hysteresis band.
	var candidates := WaterSystem.coarse_promotion_candidates(
		maxi(candidate_limit, 1), maxf(exit, 1.0e-6), 1.0e30)
	_last_scan_candidates = candidates.size()

	for candidate in candidates:
		var cell := int(candidate.get("cell", -1))
		if cell < 0:
			continue
		var depth := maxf(float(candidate.get("surface_depth_m", 0.0)), 0.0)
		var latched := bool(_latched_cells.get(cell, false))
		if not surface_depth_eligible(depth, latched, enter, exit):
			continue
		_latched_cells[cell] = true

		var key := bridge.tile_key_for_cell(cell)
		if key == null:
			continue
		if bridge.scheduler.pool.contains(key):
			_latched_cells.erase(cell)
			_resident_suppressed_count += 1
			continue

		var state := PersistentHydrologySystem.cell_state(cell)
		if state.is_empty():
			continue
		var suggested := WaterSystem.suggested_surface_promotion_volume_m3(cell)
		var parcel := surface_only_parcel_m3(suggested,
			float(state.get("surface_storage_m3", 0.0)), minimum_parcel_volume_m3)
		if parcel <= 0.0:
			continue

		var request_id := WaterSystem.promote_coarse_surface_cell(
			cell, parcel, Vector2.ZERO)
		if request_id < 0:
			_last_reason = "promotion_rejected"
			continue

		_active_request_id = request_id
		_active_cell = cell
		_active_tile_id = key.packed()
		_promotion_started_count += 1
		_last_reason = "promotion_started"
		return

	_last_reason = "no_eligible_surface_candidate"


func _prune_hysteresis_latches() -> void:
	if _latched_cells.is_empty():
		return
	var enter := maxf(surface_enter_depth_m, 1.0e-6)
	var exit := clampf(surface_exit_depth_m, 0.0, enter)
	for cell_variant: Variant in _latched_cells.keys():
		var cell := int(cell_variant)
		var state := PersistentHydrologySystem.cell_state(cell)
		if state.is_empty() or not surface_depth_eligible(
				float(state.get("surface_storage_depth_m", 0.0)), true, enter, exit):
			_latched_cells.erase(cell)


func _on_promotion_completed(report: Dictionary) -> void:
	if _active_request_id < 0:
		return
	var request_id := int(report.get("request_id", -1))
	if request_id != _active_request_id:
		return
	_promotion_completed_count += 1
	_latched_cells.erase(_active_cell)
	_clear_active_request()
	_last_reason = "promotion_completed"


func _on_promotion_failed(_error: Error, stage: String) -> void:
	if _active_request_id < 0:
		return
	# PlanetHydroPromotionBridge is strictly single-flight. A failure emitted while
	# this policy owns the active request therefore belongs to this transaction.
	_promotion_failed_count += 1
	_last_failure_stage = stage
	_clear_active_request()
	_last_reason = "promotion_failed"


func _on_store_rebuilt() -> void:
	# WaterSystem treats a store rebuild as a sparse generation boundary. Forget any
	# policy-local tile/cell identity from the previous generation as well.
	_latched_cells.clear()
	_clear_active_request()
	_last_reason = "store_rebuilt"


func _clear_active_request() -> void:
	_active_request_id = -1
	_active_cell = -1
	_active_tile_id = -1
