class_name PlanetRiverReachClusterCollapseBridge
extends Node
## Atomic sparse multi-tile river cluster -> persistent 1D reach collapse.
##
## All member states are reduced before ownership changes. One incoming channel
## transaction covers the aggregate fine volume. Members are batch-unpublished and
## connectivity is rebuilt once. If a late failure occurs, the LIFO tile pool lets
## us reacquire the exact released slots by reserving members in reverse order.

signal initialized
signal initialization_failed(error: Error)
signal collapse_completed(request_id: int, report: Dictionary)
signal collapse_failed(request_id: int, error: Error, stage: String)
signal released

var minimum_quiet_time_s := 20.0
var maximum_velocity_mps := 0.01
var maximum_outgoing_flux_m3s := 0.01
var maximum_disturbance_energy := 5.0e-5
var require_settling_state := true

var store: PlanetHydrologyRiverClusterStore
var scheduler: SparseHydroScheduler
var atlas: SparseHydroAtlasGPU
var connectivity: SparseHydroConnectivityGPU
var runtime: SparseHydrologyRuntime
var coupling: HydroRiverReachClusterCoupling
var diagnostic: SparseHydroTileStateDiagnosticsGPU

var _initialized := false
var _busy := false
var _release_requested := false
var _next_request_id := 1
var _request_id := -1
var _state_request_id := -1
var _transaction_id := -1
var _cell := -1
var _members: Array[Dictionary] = []
var _member_records: Array[Dictionary] = []
var _reduce_index := 0
var _total_volume_m3 := 0.0
var _max_depth_m := 0.0
var _last_member_state: Dictionary = {}
var _runtime_was_enabled := true
var _coarse_was_enabled := true
var _fine_unpublished := false


func initialize(p_store: PlanetHydrologyRiverClusterStore,
		p_scheduler: SparseHydroScheduler, p_atlas: SparseHydroAtlasGPU,
		p_connectivity: SparseHydroConnectivityGPU,
		p_runtime: SparseHydrologyRuntime,
		p_coupling: HydroRiverReachClusterCoupling) -> Error:
	if _initialized or diagnostic != null:
		return ERR_BUSY
	if p_store == null or not p_store.initialized or p_scheduler == null \
			or p_scheduler.pool == null or p_atlas == null or not p_atlas.initialized_ok() \
			or p_connectivity == null or not p_connectivity.initialized_ok() \
			or p_runtime == null or not p_runtime.initialized_ok() \
			or p_coupling == null or not p_coupling.initialized_ok():
		return ERR_UNCONFIGURED
	store = p_store
	scheduler = p_scheduler
	atlas = p_atlas
	connectivity = p_connectivity
	runtime = p_runtime
	coupling = p_coupling
	diagnostic = SparseHydroTileStateDiagnosticsGPU.new()
	diagnostic.name = "SparseHydroClusterTileStateDiagnosticsGPU"
	add_child(diagnostic)
	diagnostic.initialized.connect(func():
		_initialized = true
		initialized.emit())
	diagnostic.initialization_failed.connect(func(error: Error):
		initialization_failed.emit(error))
	diagnostic.state_ready.connect(_on_state_ready)
	diagnostic.readback_failed.connect(_on_state_failed)
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
	var members := coupling.cluster_members(cell)
	if members.size() <= 1:
		return false
	for member in members:
		var key := HydroTileKey.unpack(int(member.get("tile_id", -1)))
		var slot := int(member.get("slot", -1))
		if key == null or not scheduler.pool.contains(key) \
				or scheduler.pool.slot_for(key) != slot:
			return false
		var record := scheduler.pool.record(key)
		if not HydroRiverCollapsePolicy.eligible_record(record,
				minimum_quiet_time_s, maximum_velocity_mps, maximum_outgoing_flux_m3s,
				maximum_disturbance_energy, require_settling_state):
			return false
	return true


func collapse_cluster(cell: int, ignore_quiet_policy: bool = false) -> int:
	if not _initialized or _busy or coupling.pending() or runtime.busy() \
			or store.pending_ownership_transaction_count() > 0:
		return -1
	if not ignore_quiet_policy and not eligible(cell):
		return -1
	var members := coupling.cluster_members(cell)
	if members.size() <= 1:
		return -1
	var records: Array[Dictionary] = []
	for member in members:
		var key := HydroTileKey.unpack(int(member.get("tile_id", -1)))
		if key == null or scheduler.pool.slot_for(key) != int(member.get("slot", -1)):
			return -1
		var record := scheduler.pool.record(key)
		if record.is_empty():
			return -1
		record["tile_id"] = key.packed()
		record["slot"] = scheduler.pool.slot_for(key)
		records.append(record)
	var suspended := coupling.suspend_reach(cell)
	if int(suspended.get("error", FAILED)) != OK:
		return -1

	_busy = true
	_request_id = _next_request_id
	_next_request_id += 1
	_cell = cell
	_members = members
	_member_records = records
	_reduce_index = 0
	_total_volume_m3 = 0.0
	_max_depth_m = 0.0
	_last_member_state = {}
	_transaction_id = -1
	_fine_unpublished = false
	_runtime_was_enabled = runtime.enabled
	_coarse_was_enabled = bool(PersistentHydrologySystem.enabled)
	runtime.enabled = false
	PersistentHydrologySystem.enabled = false
	if not _request_next_state():
		_fail(ERR_BUSY, "state_request")
		return -1
	return _request_id


func _request_next_state() -> bool:
	if _reduce_index >= _members.size():
		_finish_reduction()
		return true
	var slot := int(_members[_reduce_index].get("slot", -1))
	_state_request_id = diagnostic.request_state(slot)
	return _state_request_id >= 0


func _on_state_ready(request_id: int, slot: int, state: Dictionary) -> void:
	if not _busy or request_id != _state_request_id or _reduce_index >= _members.size():
		return
	if slot != int(_members[_reduce_index].get("slot", -1)):
		_fail(ERR_INVALID_DATA, "state_identity")
		return
	var volume := float(state.get("volume_m3", -1.0))
	if not is_finite(volume) or volume < 0.0:
		_fail(ERR_INVALID_DATA, "state_readback")
		return
	_total_volume_m3 += volume
	_max_depth_m = maxf(_max_depth_m, float(state.get("max_depth_m", 0.0)))
	if _reduce_index == _members.size() - 1:
		_last_member_state = state.duplicate(true)
	_reduce_index += 1
	if not _request_next_state():
		_fail(ERR_BUSY, "state_request_next")


func _on_state_failed(request_id: int, slot: int, error: Error) -> void:
	if _busy and request_id == _state_request_id \
			and _reduce_index < _members.size() \
			and slot == int(_members[_reduce_index].get("slot", -1)):
		_fail(error, "state_readback")


func _finish_reduction() -> void:
	if _total_volume_m3 > 0.0:
		var prepared := store.prepare_demotion(_cell, 0.0, _total_volume_m3)
		if int(prepared.get("error", FAILED)) != OK:
			_fail(int(prepared.get("error", ERR_INVALID_DATA)), "coarse_prepare")
			return
		_transaction_id = int(prepared.get("transaction_id", -1))
		if _transaction_id < 0:
			_fail(ERR_INVALID_DATA, "coarse_prepare_identity")
			return

	var keys: Array[HydroTileKey] = []
	for member in _members:
		var key := HydroTileKey.unpack(int(member.get("tile_id", -1)))
		if key == null:
			_fail(ERR_INVALID_DATA, "member_key")
			return
		keys.append(key)
	var released := HydroSchedulerBatchOps.force_release(scheduler, keys,
		"river_cluster_collapse")
	if released.size() != keys.size():
		_fail(ERR_CANT_ACQUIRE_RESOURCE, "cluster_release")
		return
	_fine_unpublished = true
	var conn_error := connectivity.sync_pool(scheduler.pool)
	if conn_error != OK:
		_restore_or_fallback(conn_error, "connectivity")
		return
	if _transaction_id >= 0:
		var committed := store.commit_demotion(_transaction_id)
		if int(committed.get("error", FAILED)) != OK:
			_restore_or_fallback(int(committed.get("error", ERR_INVALID_DATA)), "coarse_commit")
			return
		_transaction_id = -1
	var finalized := coupling.finalize_suspended_reach(_cell, true)
	if int(finalized.get("error", FAILED)) != OK:
		_fail(int(finalized.get("error", ERR_INVALID_DATA)), "cluster_finalize", false, false)
		return

	var last_member := _members[_members.size() - 1]
	var downstream_dir: Vector2 = last_member.get("direction_cell", Vector2.RIGHT)
	if downstream_dir.length_squared() > 1.0e-12:
		downstream_dir = downstream_dir.normalized()
	var mean_velocity: Vector2 = _last_member_state.get("mean_velocity_local_mps", Vector2.ZERO)
	var projected_velocity := maxf(mean_velocity.dot(downstream_dir), 0.0)
	var depth := store.river_reaches.depth_from_storage(_cell, store.channel_storage_m3[_cell])
	var cross_area := store.river_reaches.cross_section_area(_cell, depth)
	store.channel_discharge_m3s[_cell] = cross_area * projected_velocity
	_complete({
		"cell": _cell,
		"member_count": _members.size(),
		"fine_volume_m3": _total_volume_m3,
		"pending_returned_m3": float(finalized.get("returned_pending_m3", 0.0)),
		"max_fine_depth_m": _max_depth_m,
		"downstream_velocity_mps": projected_velocity,
		"channel_storage_m3": store.channel_storage_m3[_cell],
		"representation": "persistent_1d_river_reach",
	})


func _restore_or_fallback(original_error: Error, stage: String) -> void:
	# The pool appends released slots and allocates from the back. Reserving keys in
	# reverse order therefore restores each member to its exact previous slot.
	var restored := true
	for i in range(_members.size() - 1, -1, -1):
		var key := HydroTileKey.unpack(int(_members[i].get("tile_id", -1)))
		var expected := int(_members[i].get("slot", -1))
		var lod := int(_member_records[i].get("physical_lod", 0))
		if key == null or scheduler.reserve(key, lod, "river_cluster_restore") != expected:
			restored = false
			break
	if restored:
		var keys: Array[HydroTileKey] = []
		for member in _members:
			keys.append(HydroTileKey.unpack(int(member.get("tile_id", -1))))
		var slots := HydroSchedulerBatchOps.activate_reserved(scheduler, keys,
			"river_cluster_restore")
		restored = slots.size() == keys.size() and connectivity.sync_pool(scheduler.pool) == OK
	if restored:
		_fine_unpublished = false
		if _transaction_id >= 0:
			store.rollback_demotion(_transaction_id)
			_transaction_id = -1
		if coupling.resume_suspended_reach(_cell) == OK:
			_fail(original_error, stage + "_restored_fine", true, true)
			return

	# Fine publication could not be reconstructed. The aggregate parcel has already
	# been measured exactly, so complete ownership on the coarse side instead.
	if _transaction_id >= 0:
		var committed := store.commit_demotion(_transaction_id)
		if int(committed.get("error", FAILED)) != OK:
			_fail(ERR_CANT_ACQUIRE_RESOURCE, stage + "_ownership_recovery", false, false)
			return
		_transaction_id = -1
	var finalized := coupling.finalize_suspended_reach(_cell, true)
	if int(finalized.get("error", FAILED)) != OK:
		_fail(ERR_CANT_ACQUIRE_RESOURCE, stage + "_finalize_recovery", false, false)
		return
	_fail(original_error, stage + "_coarse_fallback", false, true)


func _complete(report: Dictionary) -> void:
	var completed := _request_id
	var out := report.duplicate(true)
	out["request_id"] = completed
	_finish_request(true)
	collapse_completed.emit(completed, out)


func _fail(error: Error, stage: String,
		restore_owners: bool = true, resume_coupling: bool = true) -> void:
	var failed := _request_id
	if not _fine_unpublished and _transaction_id >= 0:
		store.rollback_demotion(_transaction_id)
		_transaction_id = -1
	if resume_coupling and not _fine_unpublished and coupling != null and _cell >= 0:
		coupling.resume_suspended_reach(_cell)
	_finish_request(restore_owners)
	collapse_failed.emit(failed, error, stage)


func _finish_request(restore_owners: bool) -> void:
	if restore_owners:
		runtime.enabled = _runtime_was_enabled
		PersistentHydrologySystem.enabled = _coarse_was_enabled
		if runtime.enabled:
			runtime.advance_time(0.0)
	_busy = false
	_request_id = -1
	_state_request_id = -1
	_transaction_id = -1
	_cell = -1
	_members = []
	_member_records = []
	_reduce_index = 0
	_total_volume_m3 = 0.0
	_max_depth_m = 0.0
	_last_member_state = {}
	_fine_unpublished = false
	if _release_requested:
		call_deferred(&"_finish_release")


func _clear_refs() -> void:
	store = null
	scheduler = null
	atlas = null
	connectivity = null
	runtime = null
	coupling = null


func release() -> void:
	if _release_requested:
		return
	_release_requested = true
	if _busy or (diagnostic != null and diagnostic.pending()):
		return
	_finish_release()


func _finish_release() -> void:
	if _busy:
		return
	if diagnostic != null and is_instance_valid(diagnostic):
		diagnostic.release()
		diagnostic.queue_free()
	diagnostic = null
	_initialized = false
	_release_requested = false
	_clear_refs()
	released.emit()


func _exit_tree() -> void:
	release()
