class_name PlanetRiverComponentCollapseBridge
extends Node
## Atomic connected sparse river component -> persistent 1D graph collapse.
##
## All member tiles are reduced before any ownership change. Fine volume is grouped
## by the coarse reach that owns each tile, then one component demotion transaction
## credits all reaches together. The complete sparse graph is unpublished in one
## scheduler batch and the coupling component is finalized only after commit.

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
var coupling: HydroRiverComponentCoupling
var diagnostic: SparseHydroTileStateDiagnosticsGPU

var _initialized := false
var _busy := false
var _release_requested := false
var _next_request_id := 1
var _request_id := -1
var _state_request_id := -1
var _transaction_id := -1
var _component_id := -1
var _component: Dictionary = {}
var _members: Array[Dictionary] = []
var _member_records: Array[Dictionary] = []
var _reduce_index := 0
var _volume_by_cell: Dictionary = {}
var _total_volume_m3 := 0.0
var _max_depth_m := 0.0
var _outlet_state: Dictionary = {}
var _runtime_was_enabled := true
var _coarse_was_enabled := true
var _fine_unpublished := false


func initialize(p_store: PlanetHydrologyRiverClusterStore,
		p_scheduler: SparseHydroScheduler, p_atlas: SparseHydroAtlasGPU,
		p_connectivity: SparseHydroConnectivityGPU,
		p_runtime: SparseHydrologyRuntime,
		p_coupling: HydroRiverComponentCoupling) -> Error:
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
	diagnostic.name = "SparseHydroComponentTileStateDiagnosticsGPU"
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


func eligible(component_id: int) -> bool:
	if not _initialized or _busy or coupling == null or coupling.pending() \
			or runtime == null or runtime.busy() or store == null \
			or store.pending_ownership_transaction_count() > 0:
		return false
	var prepared := _collect_component_members(component_id, true)
	return int(prepared.get("error", FAILED)) == OK


func collapse_component(component_id: int,
		ignore_quiet_policy: bool = false) -> int:
	if not _initialized or _busy or coupling.pending() or runtime.busy() \
			or store.pending_ownership_transaction_count() > 0:
		return -1
	var prepared := _collect_component_members(component_id, not ignore_quiet_policy)
	if int(prepared.get("error", FAILED)) != OK:
		return -1
	var suspended := coupling.suspend_component(component_id)
	if int(suspended.get("error", FAILED)) != OK:
		return -1

	_busy = true
	_request_id = _next_request_id
	_next_request_id += 1
	_component_id = component_id
	_component = prepared["component"] as Dictionary
	_members = prepared["members"] as Array[Dictionary]
	_member_records = prepared["records"] as Array[Dictionary]
	_reduce_index = 0
	_volume_by_cell.clear()
	_total_volume_m3 = 0.0
	_max_depth_m = 0.0
	_outlet_state = {}
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


func _collect_component_members(component_id: int,
		require_quiet: bool) -> Dictionary:
	var component := store.refined_component(component_id)
	if component.is_empty():
		return {"error": ERR_DOES_NOT_EXIST}
	var coupling_records := coupling.component_records(component_id)
	var cells := component.get("cells", PackedInt32Array()) as PackedInt32Array
	if coupling_records.size() != cells.size():
		return {"error": ERR_UNCONFIGURED, "reason": "component_coupling_incomplete"}
	var members: Array[Dictionary] = []
	var records: Array[Dictionary] = []
	var seen_slots: Dictionary = {}
	for raw_cell in cells:
		var cell := int(raw_cell)
		var rec := coupling_records[cell] as Dictionary
		var cell_members := coupling.cluster_members(cell)
		if cell_members.is_empty():
			return {"error": ERR_INVALID_DATA, "reason": "component_members_missing"}
		for member_value: Variant in cell_members:
			var member := (member_value as Dictionary).duplicate(true)
			var key := HydroTileKey.unpack(int(member.get("tile_id", -1)))
			var slot := int(member.get("slot", -1))
			if key == null or seen_slots.has(slot) or not scheduler.pool.contains(key) \
					or scheduler.pool.slot_for(key) != slot:
				return {"error": ERR_CANT_ACQUIRE_RESOURCE,
					"reason": "component_member_identity"}
			var pool_record := scheduler.pool.record(key)
			if pool_record.is_empty():
				return {"error": ERR_INVALID_DATA, "reason": "component_pool_record"}
			if require_quiet and not HydroRiverCollapsePolicy.eligible_record(pool_record,
					minimum_quiet_time_s, maximum_velocity_mps, maximum_outgoing_flux_m3s,
					maximum_disturbance_energy, require_settling_state):
				return {"error": ERR_BUSY, "reason": "component_not_quiet"}
			member["owner_cell"] = cell
			pool_record["tile_id"] = key.packed()
			pool_record["slot"] = slot
			seen_slots[slot] = true
			members.append(member)
			records.append(pool_record)
	return {"error": OK, "component": component, "members": members, "records": records}


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
	var member := _members[_reduce_index]
	if slot != int(member.get("slot", -1)):
		_fail(ERR_INVALID_DATA, "state_identity")
		return
	var volume := float(state.get("volume_m3", -1.0))
	if not is_finite(volume) or volume < 0.0:
		_fail(ERR_INVALID_DATA, "state_readback")
		return
	var cell := int(member.get("owner_cell", -1))
	_volume_by_cell[cell] = float(_volume_by_cell.get(cell, 0.0)) + volume
	_total_volume_m3 += volume
	_max_depth_m = maxf(_max_depth_m, float(state.get("max_depth_m", 0.0)))
	var outlet_cell := int(_component.get("downstream_outlet_cell", -1))
	if cell == outlet_cell:
		var outlet_members := coupling.cluster_members(outlet_cell)
		if not outlet_members.is_empty() \
				and int(member.get("tile_id", -1)) == int(outlet_members[outlet_members.size() - 1].get("tile_id", -2)):
			_outlet_state = state.duplicate(true)
	_reduce_index += 1
	if not _request_next_state():
		_fail(ERR_BUSY, "state_request_next")


func _on_state_failed(request_id: int, slot: int, error: Error) -> void:
	if _busy and request_id == _state_request_id and _reduce_index < _members.size() \
			and slot == int(_members[_reduce_index].get("slot", -1)):
		_fail(error, "state_readback")


func _finish_reduction() -> void:
	if _total_volume_m3 > 0.0:
		var prepared := store.prepare_component_channel_demotion(_component_id, _volume_by_cell)
		if int(prepared.get("error", FAILED)) != OK:
			_fail(int(prepared.get("error", ERR_INVALID_DATA)), "coarse_component_prepare")
			return
		_transaction_id = int(prepared.get("transaction_id", -1))
		if _transaction_id < 0:
			_fail(ERR_INVALID_DATA, "coarse_component_prepare_identity")
			return

	var keys: Array[HydroTileKey] = []
	for member in _members:
		var key := HydroTileKey.unpack(int(member.get("tile_id", -1)))
		if key == null:
			_fail(ERR_INVALID_DATA, "component_member_key")
			return
		keys.append(key)
	var released := HydroSchedulerBatchOps.force_release(scheduler, keys,
		"river_component_collapse")
	if released.size() != keys.size():
		_fail(ERR_CANT_ACQUIRE_RESOURCE, "component_release")
		return
	_fine_unpublished = true
	var conn_error := connectivity.sync_pool(scheduler.pool)
	if conn_error != OK:
		_restore_or_fallback(conn_error, "connectivity")
		return
	if _transaction_id >= 0:
		var committed := store.commit_component_channel_demotion(_transaction_id)
		if int(committed.get("error", FAILED)) != OK:
			_restore_or_fallback(int(committed.get("error", ERR_INVALID_DATA)),
				"coarse_component_commit")
			return
		_transaction_id = -1
	var finalized := coupling.finalize_suspended_component(_component_id, true)
	if int(finalized.get("error", FAILED)) != OK:
		_fail(int(finalized.get("error", ERR_INVALID_DATA)), "component_finalize", false, false)
		return

	var outlet_cell := int(_component.get("downstream_outlet_cell", -1))
	var outlet_members := (_component_outlet_members(outlet_cell))
	var downstream_dir := Vector2.RIGHT
	if not outlet_members.is_empty():
		downstream_dir = outlet_members[outlet_members.size() - 1].get(
			"direction_cell", Vector2.RIGHT) as Vector2
	if downstream_dir.length_squared() > 1.0e-12:
		downstream_dir = downstream_dir.normalized()
	var mean_velocity: Vector2 = _outlet_state.get("mean_velocity_local_mps", Vector2.ZERO)
	var projected_velocity := maxf(mean_velocity.dot(downstream_dir), 0.0)
	if outlet_cell >= 0 and store.river_reaches.is_reach_cell(outlet_cell):
		var depth := store.river_reaches.depth_from_storage(outlet_cell,
			store.channel_storage_m3[outlet_cell])
		var cross_area := store.river_reaches.cross_section_area(outlet_cell, depth)
		store.channel_discharge_m3s[outlet_cell] = cross_area * projected_velocity
	_complete({
		"component_id": _component_id,
		"reach_count": int(_component.get("reach_count", 0)),
		"member_count": _members.size(),
		"fine_volume_m3": _total_volume_m3,
		"fine_volume_by_cell_m3": _volume_by_cell.duplicate(true),
		"pending_returned_m3": float(finalized.get("returned_pending_m3", 0.0)),
		"max_fine_depth_m": _max_depth_m,
		"downstream_velocity_mps": projected_velocity,
		"representation": "persistent_1d_river_component",
	})


func _component_outlet_members(outlet_cell: int) -> Array[Dictionary]:
	var value: Variant = _component.get("cells", PackedInt32Array())
	if not (value is PackedInt32Array):
		return []
	# Coupling records have been finalized by this point, so use the member snapshot.
	var out: Array[Dictionary] = []
	for member in _members:
		if int(member.get("owner_cell", -1)) == outlet_cell:
			out.append(member.duplicate(true))
	return out


func _restore_or_fallback(original_error: Error, stage: String) -> void:
	# HydroTilePool is LIFO. Restore in reverse release order to recover exact slots.
	var restored := true
	for i in range(_members.size() - 1, -1, -1):
		var key := HydroTileKey.unpack(int(_members[i].get("tile_id", -1)))
		var expected := int(_members[i].get("slot", -1))
		var lod := int(_member_records[i].get("physical_lod", 0))
		if key == null or scheduler.reserve(key, lod, "river_component_restore") != expected:
			restored = false
			break
	if restored:
		var keys: Array[HydroTileKey] = []
		for member in _members:
			keys.append(HydroTileKey.unpack(int(member.get("tile_id", -1))))
		var slots := HydroSchedulerBatchOps.activate_reserved(scheduler, keys,
			"river_component_restore")
		restored = slots.size() == keys.size() and connectivity.sync_pool(scheduler.pool) == OK
	if restored:
		_fine_unpublished = false
		if _transaction_id >= 0:
			store.rollback_component_channel_demotion(_transaction_id)
			_transaction_id = -1
		if coupling.resume_suspended_component(_component_id) == OK:
			_fail(original_error, stage + "_restored_fine", true, true)
			return

	# Exact fine publication could not be recovered. We already measured every tile,
	# so commit the complete measured parcel to coarse ownership rather than leaving
	# a mixed generation.
	if _transaction_id >= 0:
		var committed := store.commit_component_channel_demotion(_transaction_id)
		if int(committed.get("error", FAILED)) != OK:
			_fail(ERR_CANT_ACQUIRE_RESOURCE, stage + "_ownership_recovery", false, false)
			return
		_transaction_id = -1
	var finalized := coupling.finalize_suspended_component(_component_id, true)
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
		restore_owners: bool = true, resume_component: bool = true) -> void:
	var failed := _request_id
	if not _fine_unpublished and _transaction_id >= 0:
		store.rollback_component_channel_demotion(_transaction_id)
		_transaction_id = -1
	if resume_component and not _fine_unpublished and coupling != null and _component_id >= 0:
		coupling.resume_suspended_component(_component_id)
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
	_component_id = -1
	_component = {}
	_members = []
	_member_records = []
	_reduce_index = 0
	_volume_by_cell.clear()
	_total_volume_m3 = 0.0
	_max_depth_m = 0.0
	_outlet_state = {}
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
		diagnostic.release(); diagnostic.queue_free()
	diagnostic = null
	_initialized = false
	_release_requested = false
	_clear_refs()
	released.emit()


func _exit_tree() -> void:
	release()
