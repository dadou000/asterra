class_name HydroRiverComponentCoupling
extends HydroRiverReachClusterCoupling
## Component lifecycle layer over the existing cluster exchange controller.
##
## Registration and suspension move every coarse reach record together. The parent
## continues to own the compact GPU multi-mouth exchange implementation.


func register_promoted_component(report: Dictionary) -> Error:
	if not initialized_ok() or pending() or not (store is PlanetHydrologyRiverClusterStore):
		return ERR_BUSY
	var reach_reports_value: Variant = report.get("reach_reports", null)
	if not (reach_reports_value is Array) or (reach_reports_value as Array).size() < 2:
		return ERR_INVALID_PARAMETER
	var normalized_reports: Array[Dictionary] = []
	var coupling_records: Dictionary = {}
	for value: Variant in reach_reports_value:
		if not (value is Dictionary):
			return ERR_INVALID_PARAMETER
		var reach := (value as Dictionary).duplicate(true)
		var cell := int(reach.get("cell", -1))
		var represented := float(reach.get("represented_volume_m3", 0.0))
		var members_value: Variant = reach.get("members", null)
		if cell < 0 or represented <= 0.0 or coupling_records.has(cell) \
				or not (members_value is Array) or (members_value as Array).is_empty():
			return ERR_INVALID_PARAMETER
		var normalized: Array[Dictionary] = []
		var members: Array = members_value
		for i in members.size():
			var member_value: Variant = members[i]
			if not (member_value is Dictionary):
				return ERR_INVALID_PARAMETER
			var member := (member_value as Dictionary).duplicate(true)
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
		reach["members"] = normalized
		normalized_reports.append(reach)
		var first := normalized[0]
		coupling_records[cell] = {
			"cell": cell,
			"tile_id": int(first["tile_id"]),
			"slot": int(first["slot"]),
			"members": normalized,
			"member_count": normalized.size(),
			"last_downstream_q_m3s": 0.0,
			"cumulative_added_m3": 0.0,
			"cumulative_removed_m3": 0.0,
			"representation": "sparse_2d_river_component_member",
		}

	var junctions: Array[Dictionary] = []
	var junctions_value: Variant = report.get("junctions", [])
	if junctions_value is Array:
		for value: Variant in junctions_value:
			if value is Dictionary:
				junctions.append((value as Dictionary).duplicate(true))
	var cluster_store := store as PlanetHydrologyRiverClusterStore
	var registration := cluster_store.register_refined_component(normalized_reports, junctions)
	if int(registration.get("error", FAILED)) != OK:
		return int(registration.get("error", ERR_INVALID_DATA))
	var component_id := int(registration.get("component_id", -1))
	if component_id < 0:
		return ERR_INVALID_DATA

	for cell_value: Variant in coupling_records.keys():
		var cell := int(cell_value)
		_records[cell] = (coupling_records[cell] as Dictionary).duplicate(true)
		var rec := _records[cell] as Dictionary
		reach_registered.emit(cell, int(rec["tile_id"]), int(rec["slot"]))
	report["component_id"] = component_id
	report["component"] = registration.get("component", {}).duplicate(true)
	return OK


func suspend_component(component_id: int) -> Dictionary:
	if not initialized_ok() or pending() or _release_requested \
			or not (store is PlanetHydrologyRiverClusterStore):
		return {"error": ERR_BUSY, "reason": "coupling_not_quiescent"}
	var cluster_store := store as PlanetHydrologyRiverClusterStore
	var component := cluster_store.refined_component(component_id)
	if component.is_empty():
		return {"error": ERR_DOES_NOT_EXIST, "reason": "component_not_found"}
	var cells := component.get("cells", PackedInt32Array()) as PackedInt32Array
	var snapshots: Dictionary = {}
	for raw_cell in cells:
		var cell := int(raw_cell)
		if not _records.has(cell) or _suspended_records.has(cell):
			return {"error": ERR_DOES_NOT_EXIST, "reason": "component_reach_not_active", "cell": cell}
		var record := _records[cell] as Dictionary
		var members := _record_members(record)
		if members.is_empty() or not _members_are_resident_and_connected(members):
			return {"error": ERR_CANT_ACQUIRE_RESOURCE,
				"reason": "component_member_not_resident", "cell": cell}
		snapshots[cell] = record.duplicate(true)

	# Commit point: no partial component suspension.
	for raw_cell in cells:
		var cell := int(raw_cell)
		_records.erase(cell)
		_suspended_records[cell] = (snapshots[cell] as Dictionary).duplicate(true)
	return {
		"error": OK,
		"component_id": component_id,
		"cells": cells.duplicate(),
		"records": snapshots,
	}


func resume_suspended_component(component_id: int) -> Error:
	if pending() or _release_requested or not (store is PlanetHydrologyRiverClusterStore):
		return ERR_BUSY
	var cluster_store := store as PlanetHydrologyRiverClusterStore
	var component := cluster_store.refined_component(component_id)
	if component.is_empty():
		return ERR_DOES_NOT_EXIST
	var cells := component.get("cells", PackedInt32Array()) as PackedInt32Array
	for raw_cell in cells:
		var cell := int(raw_cell)
		if not _suspended_records.has(cell) or _records.has(cell):
			return ERR_DOES_NOT_EXIST
		var members := _record_members(_suspended_records[cell] as Dictionary)
		if members.is_empty() or not _members_are_resident_and_connected(members):
			return ERR_CANT_ACQUIRE_RESOURCE
	for raw_cell in cells:
		var cell := int(raw_cell)
		_records[cell] = (_suspended_records[cell] as Dictionary).duplicate(true)
		_suspended_records.erase(cell)
	return OK


## Finalize only after all component fine tiles have been removed and any measured
## fine volume has been committed through the store's component demotion transaction.
func finalize_suspended_component(component_id: int,
		return_pending_to_channel: bool = true) -> Dictionary:
	if pending() or not (store is PlanetHydrologyRiverClusterStore):
		return {"error": ERR_BUSY, "reason": "exchange_pending"}
	var cluster_store := store as PlanetHydrologyRiverClusterStore
	var component := cluster_store.refined_component(component_id)
	if component.is_empty():
		return {"error": ERR_DOES_NOT_EXIST, "reason": "component_not_found"}
	var cells := component.get("cells", PackedInt32Array()) as PackedInt32Array
	for raw_cell in cells:
		if not _suspended_records.has(int(raw_cell)):
			return {"error": ERR_DOES_NOT_EXIST,
				"reason": "component_not_fully_suspended", "cell": int(raw_cell)}
	var result := cluster_store.unregister_refined_component(component_id,
		return_pending_to_channel)
	if int(result.get("error", FAILED)) != OK:
		return result
	for raw_cell in cells:
		_suspended_records.erase(int(raw_cell))
	return result


func component_records(component_id: int) -> Dictionary:
	if not (store is PlanetHydrologyRiverClusterStore):
		return {}
	var component := (store as PlanetHydrologyRiverClusterStore).refined_component(component_id)
	if component.is_empty():
		return {}
	var cells := component.get("cells", PackedInt32Array()) as PackedInt32Array
	var out: Dictionary = {}
	for raw_cell in cells:
		var cell := int(raw_cell)
		if _records.has(cell):
			out[cell] = (_records[cell] as Dictionary).duplicate(true)
		elif _suspended_records.has(cell):
			out[cell] = (_suspended_records[cell] as Dictionary).duplicate(true)
	return out


func stats() -> Dictionary:
	var out := super.stats()
	var suspended_components: Dictionary = {}
	if store is PlanetHydrologyRiverClusterStore:
		var cluster_store := store as PlanetHydrologyRiverClusterStore
		for cell_value: Variant in _suspended_records.keys():
			var component_id := cluster_store.refined_component_id_for_cell(int(cell_value))
			if component_id >= 0:
				suspended_components[component_id] = true
	out["component_atomic_registration"] = true
	out["component_atomic_suspend"] = true
	out["suspended_components"] = suspended_components.size()
	return out
