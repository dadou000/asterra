class_name HydroAutomaticSurfaceDemotionPolicy
extends Node
## Low-cadence policy for collapsing quiet automatically-promoted surface-flood
## tiles back into the persistent coarse hydrology store.
##
## This first policy is deliberately narrower than the explicit demotion bridge:
## - disabled unless WaterSystem.automatic_fine_demotion_enabled is true;
## - considers only tiles registered by HydroAutomaticSurfacePromotion after one of
##   *its own* successful promotions;
## - never touches manual promotions, point-source domains or frontier-created tiles;
## - requires SETTLING/FROZEN state and a long GPU-derived quiet interval;
## - requires low velocity, outgoing flux, disturbance energy and shallow depth;
## - refuses any tile with a resident cardinal neighbor, avoiding holes in a
##   connected fine domain until cluster-aware collapse exists;
## - starts at most one transactional fine->coarse handoff per scan.

signal automatic_surface_demotion_started(cell: int, tile_id: int, request_id: int)
signal automatic_surface_demotion_completed(cell: int, tile_id: int, report: Dictionary)
signal automatic_surface_demotion_failed(cell: int, tile_id: int, error: Error, stage: String)

var scan_interval_s := 5.0
var minimum_quiet_time_s := 20.0
var maximum_surface_depth_m := 0.15
var maximum_velocity_mps := 0.004
var maximum_outgoing_flux_m3s := 0.002
var maximum_disturbance_energy := 2.5e-5
var failure_retry_scans := 3

var _timer: Timer
var _tracked_tiles: Dictionary = {} # packed tile id -> coarse cell
var _retry_after_scan: Dictionary = {} # packed tile id -> scan index
var _active_request_id := -1
var _active_cell := -1
var _active_tile_id := -1
var _water_override: Node
var _persistent_override: Node

var _scan_count := 0
var _demotion_started_count := 0
var _demotion_completed_count := 0
var _demotion_failed_count := 0
var _neighbor_suppressed_count := 0
var _activity_suppressed_count := 0
var _missing_tile_pruned_count := 0
var _last_reason := "disabled"
var _last_failure_stage := ""


## Test seam only. Production resolves the real autoload singletons.
func bind_facades_for_test(water_facade: Node, persistent_facade: Node) -> Error:
	if is_inside_tree() or water_facade == null or persistent_facade == null:
		return ERR_INVALID_PARAMETER
	_water_override = water_facade
	_persistent_override = persistent_facade
	return OK


func _ready() -> void:
	process_priority = 21
	_timer = Timer.new()
	_timer.name = "SurfaceDemotionTimer"
	_timer.one_shot = false
	_timer.wait_time = maxf(scan_interval_s, 0.1)
	_timer.timeout.connect(scan_once)
	add_child(_timer)
	_timer.start()

	var water := _water_facade()
	var persistent := _persistent_facade()
	if water != null:
		if water.has_signal("planet_demotion_completed") \
				and not water.is_connected("planet_demotion_completed", _on_demotion_completed):
			water.connect("planet_demotion_completed", _on_demotion_completed)
		if water.has_signal("planet_demotion_failed") \
				and not water.is_connected("planet_demotion_failed", _on_demotion_failed):
			water.connect("planet_demotion_failed", _on_demotion_failed)
		if water.has_signal("sparse_runtime_state_changed") \
				and not water.is_connected("sparse_runtime_state_changed", _on_sparse_state_changed):
			water.connect("sparse_runtime_state_changed", _on_sparse_state_changed)
	if persistent != null and persistent.has_signal("store_rebuilt") \
			and not persistent.is_connected("store_rebuilt", _on_store_rebuilt):
		persistent.connect("store_rebuilt", _on_store_rebuilt)


## Called only by the automatic promotion policy after its own successful request.
func register_promoted_tile(cell: int, tile_id: int) -> void:
	if cell < 0 or tile_id < 0:
		return
	_tracked_tiles[tile_id] = cell
	_retry_after_scan.erase(tile_id)


func forget_tracked_tile(tile_id: int) -> void:
	_tracked_tiles.erase(tile_id)
	_retry_after_scan.erase(tile_id)


func tracked_tile_count() -> int:
	return _tracked_tiles.size()


func tracked_tiles() -> Dictionary:
	return _tracked_tiles.duplicate(true)


func stats() -> Dictionary:
	return {
		"enabled": _automatic_enabled(),
		"scan_interval_s": scan_interval_s,
		"minimum_quiet_time_s": minimum_quiet_time_s,
		"maximum_surface_depth_m": maximum_surface_depth_m,
		"maximum_velocity_mps": maximum_velocity_mps,
		"maximum_outgoing_flux_m3s": maximum_outgoing_flux_m3s,
		"maximum_disturbance_energy": maximum_disturbance_energy,
		"failure_retry_scans": failure_retry_scans,
		"tracked_tiles": _tracked_tiles.size(),
		"scan_count": _scan_count,
		"active_request_id": _active_request_id,
		"active_cell": _active_cell,
		"active_tile_id": _active_tile_id,
		"demotions_started": _demotion_started_count,
		"demotions_completed": _demotion_completed_count,
		"demotions_failed": _demotion_failed_count,
		"neighbor_suppressed": _neighbor_suppressed_count,
		"activity_suppressed": _activity_suppressed_count,
		"missing_tile_pruned": _missing_tile_pruned_count,
		"last_reason": _last_reason,
		"last_failure_stage": _last_failure_stage,
		"automatic_scope": "auto_promoted_isolated_surface_tiles_only",
	}


## Pure activity/state policy helper for headless tests.
static func quiet_surface_record_eligible(record: Dictionary,
		min_quiet_s: float, max_depth_m: float, max_velocity: float,
		max_flux_m3s: float, max_disturbance: float) -> bool:
	if record.is_empty():
		return false
	var state := int(record.get("state", HydroTilePool.TileState.ALLOCATING))
	if state != HydroTilePool.TileState.SETTLING \
			and state != HydroTilePool.TileState.FROZEN_WATER:
		return false
	var quiet := float(record.get("quiet_time_s", 0.0))
	var depth := float(record.get("max_depth_m", INF))
	var velocity := float(record.get("max_velocity_mps", INF))
	var flux := float(record.get("max_outgoing_flux_m3s", INF))
	var disturbance := float(record.get("disturbance_energy", INF))
	if not is_finite(quiet) or not is_finite(depth) or not is_finite(velocity) \
			or not is_finite(flux) or not is_finite(disturbance):
		return false
	return quiet >= maxf(min_quiet_s, 0.0) \
		and depth >= 0.0 and depth <= maxf(max_depth_m, 0.0) \
		and velocity >= 0.0 and velocity <= maxf(max_velocity, 0.0) \
		and flux >= 0.0 and flux <= maxf(max_flux_m3s, 0.0) \
		and disturbance >= 0.0 and disturbance <= maxf(max_disturbance, 0.0)


static func has_resident_cardinal_neighbor(key: HydroTileKey,
		pool: HydroTilePool) -> bool:
	if key == null or pool == null:
		return true
	for direction in [HydroTileTopology.DIR_WEST, HydroTileTopology.DIR_EAST,
			HydroTileTopology.DIR_SOUTH, HydroTileTopology.DIR_NORTH]:
		var link := HydroTileTopology.neighbor(key, direction)
		if link.is_empty():
			continue
		var neighbor: HydroTileKey = link.get("key", null)
		if neighbor != null and pool.contains(neighbor):
			return true
	return false


## One deterministic policy iteration. Production calls it from the timer; tests
## call the same method directly.
func scan_once() -> void:
	if _timer != null:
		_timer.wait_time = maxf(scan_interval_s, 0.1)
	_scan_count += 1

	var water := _water_facade()
	var persistent := _persistent_facade()
	if water == null or persistent == null:
		_last_reason = "facade_unavailable"
		return
	if not _automatic_enabled():
		_last_reason = "disabled"
		return
	if _active_request_id >= 0:
		_last_reason = "policy_transaction_active"
		return
	if not bool(persistent.call("available")):
		_last_reason = "coarse_store_unavailable"
		return
	if not bool(water.call("planet_demotion_bridge_available")):
		_last_reason = "demotion_bridge_unavailable"
		return

	var store := persistent.call("store") as PlanetHydrologyOwnershipStore
	var bridge := water.call("planet_demotion_bridge") as PlanetHydroDemotionBridge
	var runtime := water.call("sparse_runtime") as SparseHydrologyRuntime
	if store == null or bridge == null or runtime == null or not runtime.initialized_ok() \
			or runtime.scheduler == null or runtime.scheduler.pool == null:
		_last_reason = "demotion_dependencies_unavailable"
		return
	if store.pending_ownership_transaction_count() > 0 or bridge.busy() or runtime.busy():
		_last_reason = "ownership_or_runtime_busy"
		return

	var pool := runtime.scheduler.pool
	var best_tile_id := -1
	var best_cell := -1
	var best_quiet := -1.0
	var prune: Array[int] = []
	for tile_variant: Variant in _tracked_tiles.keys():
		var tile_id := int(tile_variant)
		var retry_scan := int(_retry_after_scan.get(tile_id, 0))
		if _scan_count < retry_scan:
			continue
		if not pool.contains(tile_id):
			prune.append(tile_id)
			_missing_tile_pruned_count += 1
			continue
		var record := pool.record(tile_id)
		if not quiet_surface_record_eligible(record, minimum_quiet_time_s,
				maximum_surface_depth_m, maximum_velocity_mps,
				maximum_outgoing_flux_m3s, maximum_disturbance_energy):
			_activity_suppressed_count += 1
			continue
		var key := HydroTileKey.unpack(tile_id)
		if key == null:
			prune.append(tile_id)
			continue
		if has_resident_cardinal_neighbor(key, pool):
			_neighbor_suppressed_count += 1
			continue
		var cell := int(_tracked_tiles[tile_id])
		var expected_key := bridge.tile_key_for_cell(cell)
		if expected_key == null or expected_key.packed() != tile_id:
			# The coarse/fine metric identity changed; a world/runtime rebuild should
			# normally clear the registry, but fail closed if stale metadata survives.
			prune.append(tile_id)
			continue
		var quiet := float(record.get("quiet_time_s", 0.0))
		if quiet > best_quiet:
			best_quiet = quiet
			best_tile_id = tile_id
			best_cell = cell
	for tile_id in prune:
		forget_tracked_tile(tile_id)

	if best_tile_id < 0 or best_cell < 0:
		_last_reason = "no_eligible_quiet_surface_tile"
		return

	var request_id := int(water.call("demote_fine_surface_cell", best_cell))
	if request_id < 0:
		_retry_after_scan[best_tile_id] = _scan_count + maxi(failure_retry_scans, 1)
		_last_reason = "demotion_rejected"
		return
	_active_request_id = request_id
	_active_cell = best_cell
	_active_tile_id = best_tile_id
	_demotion_started_count += 1
	_last_reason = "demotion_started"
	automatic_surface_demotion_started.emit(best_cell, best_tile_id, request_id)


func _on_demotion_completed(report: Dictionary) -> void:
	if _active_request_id < 0:
		return
	var request_id := int(report.get("request_id", -1))
	if request_id != _active_request_id:
		return
	var cell := _active_cell
	var tile_id := _active_tile_id
	_demotion_completed_count += 1
	forget_tracked_tile(tile_id)
	_clear_active_request()
	_last_reason = "demotion_completed"
	automatic_surface_demotion_completed.emit(cell, tile_id, report.duplicate(true))


func _on_demotion_failed(error: Error, stage: String) -> void:
	if _active_request_id < 0:
		return
	var cell := _active_cell
	var tile_id := _active_tile_id
	_demotion_failed_count += 1
	_last_failure_stage = stage
	_retry_after_scan[tile_id] = _scan_count + maxi(failure_retry_scans, 1)
	_clear_active_request()
	_last_reason = "demotion_failed"
	automatic_surface_demotion_failed.emit(cell, tile_id, error, stage)


func _on_sparse_state_changed(state: String) -> void:
	if state == "ready":
		return
	_clear_generation_state("sparse_state_" + state)


func _on_store_rebuilt() -> void:
	_clear_generation_state("store_rebuilt")


func _clear_generation_state(reason: String) -> void:
	_tracked_tiles.clear()
	_retry_after_scan.clear()
	_clear_active_request()
	_last_reason = reason


func _automatic_enabled() -> bool:
	var water := _water_facade()
	return water != null and bool(water.get("automatic_fine_demotion_enabled"))


func _water_facade() -> Node:
	return _water_override if _water_override != null else WaterSystem


func _persistent_facade() -> Node:
	return _persistent_override if _persistent_override != null else PersistentHydrologySystem


func _clear_active_request() -> void:
	_active_request_id = -1
	_active_cell = -1
	_active_tile_id = -1
