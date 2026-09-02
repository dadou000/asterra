class_name HydroRiverReachClusterCoupling
extends HydroRiverReachCouplingCollapse
## Continuous 1D <-> sparse-2D coupling for ordered multi-tile river clusters.
##
## A cluster is one coarse refined reach with N sparse members:
##   1D -> [member 0] -> [member 1] -> ... -> [member N-1] -> residual 1D
##
## Only member 0 owns the upstream exchange mouth. Only the final member owns the
## downstream exchange mouth. Internal boundaries are normal sparse SWE interfaces.


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
	var exchange_records: Array[Dictionary] = []
	for cell_value: Variant in _records.keys():
		var cell := int(cell_value)
		var rec := _records[cell] as Dictionary
		var members := _record_members(rec)
		if members.is_empty() or not _members_are_resident_and_connected(members):
			_fail_closed(ERR_CANT_ACQUIRE_RESOURCE, "cluster_member_identity")
			return
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
	if exchange_records.is_empty():
		return

	_runtime_was_enabled = runtime.enabled
	_coarse_was_enabled = bool(PersistentHydrologySystem.enabled)
	runtime.enabled = false
	PersistentHydrologySystem.enabled = false
	_pending_advanced_dt_s = dt
	_pending_request_id = exchange_gpu.exchange(exchange_records)
	if _pending_request_id < 0:
		_fail_closed(ERR_BUSY, "cluster_exchange_submit")


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
	}
	_pending_advanced_dt_s = 0.0
	if invalid_status:
		_fail_closed(ERR_INVALID_DATA, "cluster_exchange_record_status")
		return
	_restore_owners_and_pump()
	exchange_completed.emit(completed, exchange_report)
	if _release_requested:
		_finish_release()


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
	return out
