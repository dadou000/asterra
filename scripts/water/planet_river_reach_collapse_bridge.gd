class_name PlanetRiverReachCollapseBridge
extends Node
## Transactional sparse-2D river -> persistent-1D reach collapse.
##
## Sequence at a stable representation boundary:
##   require a quiet registered refined reach
##   -> suspend continuous mouth exchange
##   -> pause sparse + coarse advancement
##   -> compact GPU volume/momentum reduction of canonical atlas A
##   -> prepare channel-only incoming coarse parcel
##   -> unpublish fine tile + rebuild connectivity
##   -> commit exact fine volume to channel storage
##   -> return still-coarse pending confluence inflow to channel storage
##   -> remove refinement hole and resume pure 1D routing
##
## The integrated fine momentum is used only as a downstream velocity/discharge
## reconstruction hint. Channel volume/stage remain authoritative in the 1D model.

signal initialized
signal initialization_failed(error: Error)
signal collapse_started(request_id: int, cell: int, tile_id: int, slot: int)
signal collapse_completed(request_id: int, report: Dictionary)
signal collapse_failed(request_id: int, error: Error, stage: String)
signal released

var minimum_quiet_time_s := 20.0
var maximum_velocity_mps := 0.01
var maximum_outgoing_flux_m3s := 0.01
var maximum_disturbance_energy := 5.0e-5
var require_settling_state := true

var store: PlanetHydrologyRiverCoupledStore
var scheduler: SparseHydroScheduler
var atlas: SparseHydroAtlasGPU
var connectivity: SparseHydroConnectivityGPU
var runtime: SparseHydrologyRuntime
var coupling: HydroRiverReachCouplingCollapse
var diagnostic: SparseHydroTileStateDiagnosticsGPU

var _initialized := false
var _busy := false
var _next_request_id := 1
var _request_id := -1
var _state_request_id := -1
var _transaction_id := -1
var _cell := -1
var _key: HydroTileKey
var _slot := -1
var _physical_lod := 0
var _coupling_record: Dictionary = {}
var _fine_state: Dictionary = {}
var _runtime_was_enabled := true
var _coarse_was_enabled := true
var _fine_unpublished := false
var _coarse_committed := false


func initialize(p_store: PlanetHydrologyRiverCoupledStore,
		p_scheduler: SparseHydroScheduler, p_atlas: SparseHydroAtlasGPU,
		p_connectivity: SparseHydroConnectivityGPU,
		p_runtime: SparseHydrologyRuntime,
		p_coupling: HydroRiverReachCouplingCollapse) -> Error:
	if _initialized or diagnostic != null:
		return ERR_BUSY
	if p_store == null or not p_store.initialized or p_scheduler == null \
			or p_scheduler.pool == null or p_atlas == null or not p_atlas.initialized_ok() \
			or p_connectivity == null or not p_connectivity.initialized_ok() \
			or p_runtime == null or not p_runtime.initialized_ok() \
			or p_coupling == null or not p_coupling.initialized_ok():
		return ERR_UNCONFIGURED
	if p_scheduler.pool.capacity != p_atlas.capacity \
			or p_connectivity.capacity != p_atlas.capacity:
		return ERR_INVALID_PARAMETER

	store = p_store
	scheduler = p_scheduler
	atlas = p_atlas
	connectivity = p_connectivity
	runtime = p_runtime
	coupling = p_coupling

	diagnostic = SparseHydroTileStateDiagnosticsGPU.new()
	diagnostic.name = "SparseHydroTileStateDiagnosticsGPU"
	add_child(diagnostic)
	diagnostic.initialized.connect(_on_diagnostic_initialized)
	diagnostic.initialization_failed.connect(_on_diagnostic_initialization_failed)
	diagnostic.state_ready.connect(_on_fine_state_ready)
	diagnostic.readback_failed.connect(_on_fine_state_failed)
	var err := diagnostic.initialize_from_atlas(atlas)
	if err != OK:
		diagnostic.queue_free()
		diagnostic = null
		_clear_refs()
		return err
	return OK


func initialized_ok() -> bool:
	return _initialized


func busy() -> bool:
	return _busy


func eligible(cell: int) -> bool:
	if not _initialized or _busy or coupling == null or coupling.pending() \
			or runtime == null or runtime.busy() or store == null \
			or store.pending_ownership_transaction_count() > 0:
		return false
	var rec := coupling.registered_reach(cell)
	if rec.is_empty():
		return false
	var key := HydroTileKey.unpack(int(rec.get("tile_id", -1)))
	if key == null or scheduler == null or scheduler.pool == null \
			or not scheduler.pool.contains(key):
		return false
	var pool_record := scheduler.pool.record(key)
	if pool_record.is_empty() or scheduler.pool.slot_for(key) != int(rec.get("slot", -1)):
		return false
	if require_settling_state:
		var state := int(pool_record.get("state", HydroTilePool.TileState.ACTIVE))
		if state not in [HydroTilePool.TileState.SETTLING, HydroTilePool.TileState.FROZEN_WATER]:
			return false
	if float(pool_record.get("quiet_time_s", 0.0)) < maxf(minimum_quiet_time_s, 0.0):
		return false
	if float(pool_record.get("max_velocity_mps", INF)) > maxf(maximum_velocity_mps, 0.0):
		return false
	if float(pool_record.get("max_outgoing_flux_m3s", INF)) \
			> maxf(maximum_outgoing_flux_m3s, 0.0):
		return false
	if float(pool_record.get("disturbance_energy", INF)) \
			> maxf(maximum_disturbance_energy, 0.0):
		return false
	return true


func collapse_reach(cell: int, ignore_quiet_policy: bool = false) -> int:
	if not _initialized or _busy or coupling == null or coupling.pending() \
			or runtime == null or runtime.busy() or store == null \
			or store.pending_ownership_transaction_count() > 0:
		return -1
	if not ignore_quiet_policy and not eligible(cell):
		return -1
	var rec := coupling.registered_reach(cell)
	if rec.is_empty():
		return -1
	var key := HydroTileKey.unpack(int(rec.get("tile_id", -1)))
	var slot := int(rec.get("slot", -1))
	if key == null or not scheduler.pool.contains(key) or scheduler.pool.slot_for(key) != slot:
		return -1
	var pool_record := scheduler.pool.record(key)
	if pool_record.is_empty():
		return -1

	var suspended := coupling.suspend_reach(cell)
	if int(suspended.get("error", FAILED)) != OK:
		return -1

	_busy = true
	_request_id = _next_request_id
	_next_request_id += 1
	_cell = cell
	_key = key
	_slot = slot
	_physical_lod = int(pool_record.get("physical_lod", 0))
	_coupling_record = rec.duplicate(true)
	_fine_state = {}
	_fine_unpublished = false
	_coarse_committed = false
	_transaction_id = -1
	_runtime_was_enabled = runtime.enabled
	_coarse_was_enabled = bool(PersistentHydrologySystem.enabled)
	runtime.enabled = false
	PersistentHydrologySystem.enabled = false

	_state_request_id = diagnostic.request_state(slot)
	if _state_request_id < 0:
		_fail(ERR_BUSY, "state_request")
		return -1
	collapse_started.emit(_request_id, cell, key.packed(), slot)
	return _request_id


func _on_diagnostic_initialized() -> void:
	_initialized = true
	initialized.emit()


func _on_diagnostic_initialization_failed(error: Error) -> void:
	_initialized = false
	initialization_failed.emit(error)


func _on_fine_state_ready(request_id: int, slot: int, state: Dictionary) -> void:
	if not _busy or request_id != _state_request_id or slot != _slot:
		return
	if _key == null or scheduler.pool.slot_for(_key) != _slot:
		_fail(ERR_BUSY, "tile_identity_changed")
		return
	var volume := float(state.get("volume_m3", -1.0))
	if not is_finite(volume) or volume < 0.0:
		_fail(ERR_INVALID_DATA, "state_readback")
		return
	_fine_state = state.duplicate(true)

	if volume > 0.0:
		var prepared := store.prepare_demotion(_cell, 0.0, volume)
		if int(prepared.get("error", FAILED)) != OK:
			_fail(int(prepared.get("error", ERR_INVALID_DATA)), "coarse_prepare")
			return
		_transaction_id = int(prepared.get("transaction_id", -1))
		if _transaction_id < 0:
			_fail(ERR_INVALID_DATA, "coarse_prepare_identity")
			return

	if not scheduler.force_release(_key, "river_reach_collapse"):
		_fail(ERR_CANT_ACQUIRE_RESOURCE, "fine_release")
		return
	_fine_unpublished = true
	var conn_error := connectivity.sync_pool(scheduler.pool)
	if conn_error != OK:
		_restore_fine_after_release(conn_error, "connectivity")
		return

	if _transaction_id >= 0:
		var committed := store.commit_demotion(_transaction_id)
		if int(committed.get("error", FAILED)) != OK:
			_restore_fine_after_release(
				int(committed.get("error", ERR_INVALID_DATA)), "coarse_commit")
			return
		_transaction_id = -1
		_coarse_committed = true

	var finalized := coupling.finalize_suspended_reach(_cell, true)
	if int(finalized.get("error", FAILED)) != OK:
		# Fine volume is already represented in coarse channel storage and pending
		# donor water remains coarse-owned in the refined queue. Fail closed rather
		# than restart either simulation with a stale refinement hole.
		_fail(int(finalized.get("error", ERR_INVALID_DATA)),
			"refinement_finalize", false, false)
		return

	var depth := store.river_reaches.depth_from_storage(_cell,
		store.channel_storage_m3[_cell])
	var cross_area := store.river_reaches.cross_section_area(_cell, depth)
	var mean_velocity: Vector2 = _fine_state.get("mean_velocity_local_mps", Vector2.ZERO)
	var downstream_dir: Vector2 = _coupling_record.get("direction_cell", Vector2.RIGHT)
	if downstream_dir.length_squared() > 1.0e-12:
		downstream_dir = downstream_dir.normalized()
	var projected_velocity := maxf(mean_velocity.dot(downstream_dir), 0.0)
	var discharge_hint := cross_area * projected_velocity
	store.channel_discharge_m3s[_cell] = discharge_hint
	var reach_state := store.river_reaches.reach_state(
		_cell, store.channel_storage_m3[_cell])
	var report := {
		"cell": _cell,
		"tile_id": _key.packed(),
		"slot": _slot,
		"fine_volume_m3": volume,
		"pending_returned_m3": float(finalized.get("returned_pending_m3", 0.0)),
		"mean_velocity_local_mps": mean_velocity,
		"downstream_velocity_mps": projected_velocity,
		"reconstructed_discharge_hint_m3s": discharge_hint,
		"max_fine_depth_m": float(_fine_state.get("max_depth_m", 0.0)),
		"channel_storage_m3": store.channel_storage_m3[_cell],
		"reach_state": reach_state,
		"representation": "persistent_1d_river_reach",
	}
	_complete(report)


func _on_fine_state_failed(request_id: int, slot: int, error: Error) -> void:
	if _busy and request_id == _state_request_id and slot == _slot:
		_fail(error, "state_readback")


func _restore_fine_after_release(original_error: Error, stage: String) -> void:
	if _key == null:
		_fail(original_error, stage + "_missing_key", false, false)
		return
	var restored_slot := scheduler.reserve(_key, _physical_lod, "river_collapse_restore")
	if restored_slot == _slot:
		var activated := scheduler.activate_reserved(_key, "river_collapse_restore")
		var conn_error := connectivity.sync_pool(scheduler.pool)
		if activated == _slot and conn_error == OK:
			_fine_unpublished = false
			if _transaction_id >= 0:
				store.rollback_demotion(_transaction_id)
				_transaction_id = -1
			var resumed := coupling.resume_suspended_reach(_cell)
			if resumed == OK:
				_fail(original_error, stage + "_restored_fine", true, true)
				return

	# Fine publication could not be restored. Preserve the measured parcel by
	# committing the validated channel return, then close the refinement hole so
	# pending coarse inflow also becomes ordinary 1D channel storage.
	if scheduler.pool.contains(_key):
		scheduler.force_release(_key, "river_collapse_restore_failed")
	_fine_unpublished = true
	if _transaction_id >= 0:
		var committed := store.commit_demotion(_transaction_id)
		if int(committed.get("error", FAILED)) != OK:
			_fail(ERR_CANT_ACQUIRE_RESOURCE,
				stage + "_ownership_recovery_failed", false, false)
			return
		_transaction_id = -1
		_coarse_committed = true
	var finalized := coupling.finalize_suspended_reach(_cell, true)
	if int(finalized.get("error", FAILED)) != OK:
		_fail(ERR_CANT_ACQUIRE_RESOURCE,
			stage + "_refinement_recovery_failed", false, false)
		return
	_fail(original_error, stage + "_coarse_fallback", false, true)


func _complete(report: Dictionary) -> void:
	var completed_id := _request_id
	var published := report.duplicate(true)
	published["request_id"] = completed_id
	_finish_request(true)
	collapse_completed.emit(completed_id, published)


func _fail(error: Error, stage: String,
		restore_owners: bool = true, resume_coupling: bool = true) -> void:
	var failed_id := _request_id
	if not _fine_unpublished and _transaction_id >= 0 and store != null:
		store.rollback_demotion(_transaction_id)
		_transaction_id = -1
	if resume_coupling and not _fine_unpublished and coupling != null and _cell >= 0:
		coupling.resume_suspended_reach(_cell)
	_finish_request(restore_owners)
	collapse_failed.emit(failed_id, error, stage)


func _finish_request(restore_owners: bool) -> void:
	if restore_owners:
		if runtime != null:
			runtime.enabled = _runtime_was_enabled
		PersistentHydrologySystem.enabled = _coarse_was_enabled
		if runtime != null and runtime.enabled:
			runtime.advance_time(0.0)
	_busy = false
	_request_id = -1
	_state_request_id = -1
	_transaction_id = -1
	_cell = -1
	_key = null
	_slot = -1
	_physical_lod = 0
	_coupling_record = {}
	_fine_state = {}
	_fine_unpublished = false
	_coarse_committed = false


func _clear_refs() -> void:
	store = null
	scheduler = null
	atlas = null
	connectivity = null
	runtime = null
	coupling = null


func release() -> void:
	if _busy:
		if not _fine_unpublished:
			if _transaction_id >= 0 and store != null:
				store.rollback_demotion(_transaction_id)
			if coupling != null and _cell >= 0:
				coupling.resume_suspended_reach(_cell)
		else:
			# Fine authority already left. A positive measured parcel must finish on
			# the coarse side before this bridge may disappear.
			if _transaction_id >= 0 and store != null:
				store.commit_demotion(_transaction_id)
			if coupling != null and _cell >= 0:
				coupling.finalize_suspended_reach(_cell, true)
		_finish_request(false)
	if diagnostic != null and is_instance_valid(diagnostic):
		diagnostic.release()
		diagnostic.queue_free()
	diagnostic = null
	_initialized = false
	_clear_refs()
	released.emit()


func _exit_tree() -> void:
	release()
