class_name HydroRiverReachClusterCoupling
extends HydroRiverReachCouplingCollapse
## Continuous 1D <-> sparse-2D coupling for ordered multi-tile river clusters.
##
## One coarse reach normally exchanges at its first/last sparse members:
##   1D -> [member 0] -> ... -> [member N-1] -> residual 1D
##
## A validated cross-reach component may instead bypass internal coarse mouths:
##   external/local coarse queues -> fine branches -> shared fine component -> 1D
##
## Component mode is conservative and opportunistic. It activates only when the
## CPU component contract proves every internal upstream reach is fully represented,
## has no residual coarse parcel, and the corresponding sparse boundary members are
## physically/corridor continuous. Otherwise the existing per-reach exchange remains active.

var _pending_component_count := 0
var _pending_component_fallback_count := 0
var _pending_component_boundary_records := 0
var _pending_component_injection_records := 0
var _pending_component_external_mouths := 0
var _last_component_fallback_reason := ""


func register_promoted_reach(report: Dictionary) -> Error:
	var members_value: Variant = report.get("members", null)
	if members_value is Array and not (members_value as Array).is_empty():
		return register_promoted_cluster(report)
	return super.register_promoted_reach(report)


func register_promoted_cluster(report: Dictionary) -> Error:
	if not initialized_ok() or pending() or not (store is PlanetHydrologyRiverClusterStore):
		return ERR_BUSY
	var cell := int(report.get("cell", -1))
	var represented := float(report.get("represented_volume_m3", 0.0))
	var members_value: Variant = report.get("members", null)
	if cell < 0 or represented <= 0.0 or not (members_value is Array):
		return ERR_INVALID_PARAMETER
	var input: Array = members_value
	if input.is_empty():
		return ERR_INVALID_PARAMETER
	var normalized: Array[Dictionary] = []
	for i in input.size():
		var value: Variant = input[i]
		if not (value is Dictionary):
			return ERR_INVALID_PARAMETER
		var member := (value as Dictionary).duplicate(true)
		var tile_id := int(member.get("tile_id", -1))
		var slot := int(member.get("slot", -1))
		var center_value: Variant = member.get("center_cell", null)
		var direction_value: Variant = member.get("direction_cell", null)
		var velocity_value: Variant = member.get("local_velocity", null)
		if tile_id < 0 or slot < 0 or not (center_value is Vector2) \
				or not (direction_value is Vector2) or not (velocity_value is Vector2):
			return ERR_INVALID_PARAMETER
		var direction := direction_value as Vector2
		if direction.length_squared() <= 1.0e-12:
			return ERR_INVALID_PARAMETER
		member["index"] = i
		member["direction_cell"] = direction.normalized()
		member["half_width_m"] = maxf(float(member.get("half_width_m", 0.0)),
			runtime.atlas.cell_size_m * 0.5)
		normalized.append(member)

	if not _members_are_resident_and_connected(normalized):
		return ERR_CANT_ACQUIRE_RESOURCE
	var cluster_store := store as PlanetHydrologyRiverClusterStore
	var registration := cluster_store.register_refined_cluster(cell, normalized, represented)
	if int(registration.get("error", FAILED)) != OK:
		return int(registration.get("error", ERR_INVALID_DATA))
	var first := normalized[0]
	_records[cell] = {
		"cell": cell,
		"tile_id": int(first["tile_id"]),
		"slot": int(first["slot"]),
		"members": normalized,
		"member_count": normalized.size(),
		"last_downstream_q_m3s": 0.0,
		"cumulative_added_m3": 0.0,
		"cumulative_removed_m3": 0.0,
		"representation": "sparse_2d_river_cluster",
	}
	reach_registered.emit(cell, int(first["tile_id"]), int(first["slot"]))
	return OK


func _on_runtime_cycle_completed(_cycle_id: int, report: Dictionary) -> void:
	if not initialized_ok() or pending() or _records.is_empty():
		return
	var dt := maxf(float(report.get("advanced_dt_s", 0.0)), 0.0)
	if dt <= 0.0:
		return
	_reset_pending_component_context()

	var exchange_records: Array[Dictionary] = []
	var handled: Dictionary = {}
	var ordered_cells: Array[int] = []
	for value: Variant in _records.keys():
		ordered_cells.append(int(value))
	ordered_cells.sort()
	var cluster_store := store as PlanetHydrologyRiverClusterStore

	for cell in ordered_cells:
		if handled.has(cell):
			continue
		var component_id := cluster_store.refined_component_id_for_cell(cell)
		if component_id >= 0:
			var component := cluster_store.refined_component(component_id)
			var component_cells := component.get("cells", PackedInt32Array()) as PackedInt32Array
			var all_registered := not component_cells.is_empty()
			for raw_component_cell in component_cells:
				var component_cell := int(raw_component_cell)
				if not _records.has(component_cell):
					all_registered = false
					continue
				var members := _record_members(_records[component_cell] as Dictionary)
				if members.is_empty() or not _members_are_resident_and_connected(members):
					_fail_closed(ERR_CANT_ACQUIRE_RESOURCE, "component_cluster_member_identity")
					return

			if all_registered:
				var component_plan := HydroRiverComponentExchangePlanner.plan(
					cluster_store, component_id, _records, dt, mouth_cells,
					runtime.atlas.cell_size_m, runtime.atlas.tile_resolution)
				if int(component_plan.get("error", FAILED)) == OK:
					var planned_records := component_plan.get("exchange_records", []) as Array
					for planned: Variant in planned_records:
						if planned is Dictionary:
							exchange_records.append((planned as Dictionary).duplicate(true))
					for raw_component_cell in component_cells:
						handled[int(raw_component_cell)] = true
					_pending_component_count += 1
					_pending_component_boundary_records += int(component_plan.get(
						"boundary_record_count", 0))
					_pending_component_injection_records += int(component_plan.get(
						"injection_record_count", 0))
					_pending_component_external_mouths += int(component_plan.get(
						"external_upstream_mouth_count", 0))
					continue
				_last_component_fallback_reason = String(component_plan.get(
					"reason", "component_contract_not_ready"))
			else:
				_last_component_fallback_reason = "component_coupling_record_missing"

			# A logical component that is not yet physically/ownership continuous keeps
			# the proven per-reach mouth semantics. This is a conservative fallback, not
			# an error: pending water remains associated with its original coarse parcel.
			_pending_component_fallback_count += 1
			for raw_component_cell in component_cells:
				var component_cell := int(raw_component_cell)
				handled[component_cell] = true
				if _records.has(component_cell):
					if not _append_legacy_exchange_records(component_cell, dt, exchange_records):
						return
			continue

		handled[cell] = true
		if not _append_legacy_exchange_records(cell, dt, exchange_records):
			return

	if exchange_records.is_empty():
		_reset_pending_component_context()
		return

	_runtime_was_enabled = runtime.enabled
	_coarse_was_enabled = bool(PersistentHydrologySystem.enabled)
	runtime.enabled = false
	PersistentHydrologySystem.enabled = false
	_pending_advanced_dt_s = dt
	_pending_request_id = exchange_gpu.exchange(exchange_records)
	if _pending_request_id < 0:
		_fail_closed(ERR_BUSY, "cluster_exchange_submit")


func _append_legacy_exchange_records(cell: int, dt: float,
		exchange_records: Array[Dictionary]) -> bool:
	if not _records.has(cell):
		return true
	var rec := _records[cell] as Dictionary
	var members := _record_members(rec)
	if members.is_empty() or not _members_are_resident_and_connected(members):
		_fail_closed(ERR_CANT_ACQUIRE_RESOURCE, "cluster_member_identity")
		return false
	var pending_volume := store.refined_inflow_available_m3(cell)
	var rate := store.refined_inflow_rate(cell)
	var add_volume := minf(pending_volume, maxf(rate, 0.0) * dt)
	var first := members[0]
	var last := members[members.size() - 1]
	exchange_records.append(_exchange_record(cell, first, add_volume, dt,
		true, members.size() == 1))
	if members.size() > 1:
		exchange_records.append(_exchange_record(cell, last, 0.0, dt,
			false, true))
	return true


func _on_exchange_ready(request_id: int, results: Array[Dictionary]) -> void:
	if request_id != _pending_request_id:
		return
	var aggregate: Dictionary = {}
	var invalid_status := false
	for result in results:
		var cell := int(result.get("cell", -1))
		if not _records.has(cell) or float(result.get("mouth_or_status", -1.0)) < 0.0:
			invalid_status = true
			continue
		var agg: Dictionary = aggregate.get(cell, {
			"added": 0.0, "removed": 0.0, "q": 0.0,
		})
		agg["added"] = float(agg["added"]) + maxf(float(result.get("added_m3", 0.0)), 0.0)
		agg["removed"] = float(agg["removed"]) + maxf(float(result.get("removed_m3", 0.0)), 0.0)
		agg["q"] = maxf(float(agg["q"]),
		maxf(float(result.get("measured_downstream_q_m3s", 0.0)), 0.0))
		aggregate[cell] = agg

	var total_added := 0.0
	var total_removed := 0.0
	var max_q := 0.0
	for cell_value: Variant in aggregate.keys():
		var cell := int(cell_value)
		var agg := aggregate[cell] as Dictionary
		var added := float(agg["added"])
		var removed := float(agg["removed"])
		var q := float(agg["q"])
		if added > 0.0:
			var consume := store.consume_refined_inflow(cell, added)
			if int(consume.get("error", FAILED)) != OK:
				_fail_closed(int(consume.get("error", ERR_INVALID_DATA)), "cluster_consume_1d_inflow")
				return
		if removed > 0.0:
			var accept := store.accept_refined_outflow(cell, removed)
			if int(accept.get("error", FAILED)) != OK:
				_fail_closed(int(accept.get("error", ERR_INVALID_DATA)), "cluster_accept_2d_outflow")
				return
		var rec := _records[cell] as Dictionary
		rec["last_downstream_q_m3s"] = q
		rec["cumulative_added_m3"] = float(rec.get("cumulative_added_m3", 0.0)) + added
		rec["cumulative_removed_m3"] = float(rec.get("cumulative_removed_m3", 0.0)) + removed
		_records[cell] = rec
		total_added += added
		total_removed += removed
		max_q = maxf(max_q, q)

	var completed := _pending_request_id
	_pending_request_id = -1
	var component_count := _pending_component_count
	var fallback_count := _pending_component_fallback_count
	var component_boundary_records := _pending_component_boundary_records
	var component_injection_records := _pending_component_injection_records
	var component_external_mouths := _pending_component_external_mouths
	var exchange_report := {
		"request_id": completed,
		"advanced_dt_s": _pending_advanced_dt_s,
		"reach_count": aggregate.size(),
		"boundary_record_count": results.size(),
		"added_1d_to_2d_m3": total_added,
		"removed_2d_to_1d_m3": total_removed,
		"max_measured_downstream_q_m3s": max_q,
		"invalid_record_status": invalid_status,
		"cluster_boundary_exchange": true,
		"component_multi_mouth_exchange": component_count > 0,
		"component_count": component_count,
		"component_fallback_count": fallback_count,
		"component_boundary_record_count": component_boundary_records,
		"component_injection_record_count": component_injection_records,
		"component_external_upstream_mouth_count": component_external_mouths,
	}
	_pending_advanced_dt_s = 0.0
	_reset_pending_component_context()
	if invalid_status:
		_fail_closed(ERR_INVALID_DATA, "cluster_exchange_record_status")
		return
	_restore_owners_and_pump()
	exchange_completed.emit(completed, exchange_report)
	if _release_requested:
		_finish_release()


## Individual suspension would break a component-wide fine topology while leaving
## the other component mouths live. Until component-level collapse lands, require
## callers to dissolve the component first rather than creating a partial suspend.
func suspend_reach(cell: int) -> Dictionary:
	if store is PlanetHydrologyRiverClusterStore:
		var cluster_store := store as PlanetHydrologyRiverClusterStore
		if cluster_store.refined_component_id_for_cell(cell) >= 0:
			return {"error": ERR_BUSY, "reason": "reach_belongs_to_refined_component"}
	return super.suspend_reach(cell)


func resume_suspended_reach(cell: int) -> Error:
	if pending() or _release_requested:
		return ERR_BUSY
	if not _suspended_records.has(cell):
		return ERR_DOES_NOT_EXIST
	if store == null or not store.is_refined_reach(cell):
		return ERR_UNCONFIGURED
	var record := _suspended_records[cell] as Dictionary
	var members := _record_members(record)
	if members.is_empty() or not _members_are_resident_and_connected(members):
		return ERR_CANT_ACQUIRE_RESOURCE
	_records[cell] = record.duplicate(true)
	_suspended_records.erase(cell)
	return OK


func cluster_members(cell: int) -> Array[Dictionary]:
	if _records.has(cell):
		return _record_members(_records[cell] as Dictionary)
	if _suspended_records.has(cell):
		return _record_members(_suspended_records[cell] as Dictionary)
	return []


func _record_members(record: Dictionary) -> Array[Dictionary]:
	var value: Variant = record.get("members", null)
	if value is Array and not (value as Array).is_empty():
		var out: Array[Dictionary] = []
		for item: Variant in value:
			if item is Dictionary:
				out.append((item as Dictionary).duplicate(true))
		return out
	return [{
		"index": 0,
		"tile_id": int(record.get("tile_id", -1)),
		"slot": int(record.get("slot", -1)),
		"center_cell": record.get("center_cell", Vector2.ZERO),
		"direction_cell": record.get("direction_cell", Vector2.RIGHT),
		"half_width_m": float(record.get("half_width_m", runtime.atlas.cell_size_m)),
		"local_velocity": record.get("add_velocity", Vector2.ZERO),
	}]


func _exchange_record(cell: int, member: Dictionary, add_volume: float, dt: float,
		upstream: bool, downstream: bool) -> Dictionary:
	return {
		"cell": cell,
		"slot": int(member.get("slot", -1)),
		"center_cell": member.get("center_cell", Vector2.ZERO),
		"direction_cell": member.get("direction_cell", Vector2.RIGHT),
		"half_width_m": float(member.get("half_width_m", runtime.atlas.cell_size_m)),
		"add_volume_m3": add_volume,
		"exchange_dt_s": dt,
		"mouth_cells": mouth_cells,
		"add_velocity": member.get("local_velocity", Vector2.ZERO),
		"upstream_enabled": upstream,
		"downstream_enabled": downstream,
	}


func _members_are_resident_and_connected(members: Array[Dictionary]) -> bool:
	if runtime == null or runtime.scheduler == null or runtime.scheduler.pool == null:
		return false
	for i in members.size():
		var member := members[i]
		var key := HydroTileKey.unpack(int(member.get("tile_id", -1)))
		var slot := int(member.get("slot", -1))
		if key == null or not runtime.scheduler.pool.contains(key) \
				or runtime.scheduler.pool.slot_for(key) != slot:
			return false
		if i + 1 < members.size():
			var next := HydroTileKey.unpack(int(members[i + 1].get("tile_id", -1)))
			if next == null or not _cardinal_neighbors(key, next):
				return false
	return true


func _cardinal_neighbors(a: HydroTileKey, b: HydroTileKey) -> bool:
	for direction in 4:
		var link := HydroTileTopology.neighbor(a, direction)
		if link.is_empty():
			continue
		var key := link.get("key") as HydroTileKey
		if key != null and key.equals(b):
			return true
	return false


func stats() -> Dictionary:
	var out := super.stats()
	var cluster_count := 0
	var member_count := 0
	for value: Variant in _records.values():
		var members := _record_members(value as Dictionary)
		if members.size() > 1:
			cluster_count += 1
		member_count += members.size()
	out["multi_tile_clusters"] = cluster_count
	out["cluster_members"] = member_count
	out["component_multi_mouth_coupling"] = true
	out["last_component_fallback_reason"] = _last_component_fallback_reason
	if store is PlanetHydrologyRiverClusterStore:
		var cluster_store := store as PlanetHydrologyRiverClusterStore
		var seen_components: Dictionary = {}
		var ready_components := 0
		for cell_value: Variant in _records.keys():
			var component_id := cluster_store.refined_component_id_for_cell(int(cell_value))
			if component_id < 0 or seen_components.has(component_id):
				continue
			seen_components[component_id] = true
			var contract := HydroRiverComponentCouplingContract.evaluate(cluster_store, component_id,
				runtime.atlas.tile_resolution, runtime.atlas.cell_size_m)
			if int(contract.get("error", FAILED)) == OK and bool(contract.get("ready", false)):
				ready_components += 1
		out["registered_refined_components"] = seen_components.size()
		out["coupling_ready_refined_components"] = ready_components
	return out


func _fail_closed(error: Error, stage: String) -> void:
	_reset_pending_component_context()
	super._fail_closed(error, stage)


func _reset_pending_component_context() -> void:
	_pending_component_count = 0
	_pending_component_fallback_count = 0
	_pending_component_boundary_records = 0
	_pending_component_injection_records = 0
	_pending_component_external_mouths = 0


func _finish_release() -> void:
	_reset_pending_component_context()
	_last_component_fallback_reason = ""
	super._finish_release()
