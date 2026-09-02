class_name HydroRiverComponentExchangePlanner
extends RefCounted
## Builds one compact GPU exchange batch for a physically continuous refined river
## component while preserving coarse ownership per macro reach.
##
## Every coarse reach keeps its own pending-inflow queue, so every component member
## may still need an upstream injection record for local/external coarse-owned water.
## Only the component outlet owns a downstream 2D->1D removal mouth. Internal
## downstream mouths are omitted because normal sparse SWE crosses the validated
## fine/fine links between reaches.


static func plan(store: PlanetHydrologyRiverClusterStore, component_id: int,
		coupling_records: Dictionary, dt_s: float, mouth_cells: float,
		cell_size_m: float) -> Dictionary:
	if store == null or not store.initialized or component_id < 0 \
			or not is_finite(dt_s) or dt_s <= 0.0 \
			or not is_finite(cell_size_m) or cell_size_m <= 0.0:
		return {"error": ERR_INVALID_PARAMETER, "reason": "invalid_component_exchange_request"}

	var contract := HydroRiverComponentCouplingContract.evaluate(store, component_id)
	if int(contract.get("error", FAILED)) != OK or not bool(contract.get("ready", false)):
		return contract
	var cells := contract.get("cells", PackedInt32Array()) as PackedInt32Array
	var outlet_cell := int(contract.get("downstream_outlet_cell", -1))
	if outlet_cell < 0:
		return {"error": ERR_INVALID_DATA, "reason": "component_outlet_missing"}

	var exchange_records: Array[Dictionary] = []
	var injection_records := 0
	var requested_add_m3 := 0.0
	var downstream_records := 0
	for raw_cell in cells:
		var cell := int(raw_cell)
		if not coupling_records.has(cell):
			return {
				"error": ERR_UNCONFIGURED,
				"reason": "component_coupling_record_missing",
				"cell": cell,
			}
		var rec := coupling_records[cell] as Dictionary
		var members := _members(rec)
		if members.is_empty():
			return {
				"error": ERR_INVALID_DATA,
				"reason": "component_coupling_members_missing",
				"cell": cell,
			}

		var pending_volume := store.refined_inflow_available_m3(cell)
		var rate := store.refined_inflow_rate(cell)
		var add_volume := minf(pending_volume, maxf(rate, 0.0) * dt_s)
		var is_outlet := cell == outlet_cell
		var first := members[0]
		var last := members[members.size() - 1]

		# Coarse-owned local/external inflow is injected into that reach's first fine
		# member. This is independent of whether the reach is a topological root.
		if add_volume > 0.0:
			var combined_downstream := is_outlet and members.size() == 1
			exchange_records.append(_record(cell, first, add_volume, dt_s,
				mouth_cells, cell_size_m, true, combined_downstream))
			injection_records += 1
			requested_add_m3 += add_volume
			if combined_downstream:
				downstream_records += 1

		# Only the final coarse reach exports fine water back into 1D. If the outlet
		# is one tile and already has an injection record, both flags share one record.
		if is_outlet and not (add_volume > 0.0 and members.size() == 1):
			exchange_records.append(_record(cell, last, 0.0, dt_s,
				mouth_cells, cell_size_m, false, true))
			downstream_records += 1

	if exchange_records.is_empty() or downstream_records != 1:
		return {
			"error": ERR_INVALID_DATA,
			"reason": "component_boundary_plan_invalid",
			"downstream_record_count": downstream_records,
		}

	return {
		"error": OK,
		"component_id": component_id,
		"cells": cells,
		"exchange_records": exchange_records,
		"boundary_record_count": exchange_records.size(),
		"injection_record_count": injection_records,
		"downstream_record_count": downstream_records,
		"requested_add_m3": requested_add_m3,
		"external_upstream_mouth_count": int(contract.get("upstream_mouth_count", 0)),
		"downstream_outlet_cell": outlet_cell,
		"internal_coarse_mouths_bypassed": int(contract.get(
			"internal_coarse_mouths_bypassed", 0)),
		"component_multi_mouth": true,
		"ownership_changed": false,
	}


static func _members(record: Dictionary) -> Array[Dictionary]:
	var value: Variant = record.get("members", null)
	if not (value is Array) or (value as Array).is_empty():
		return []
	var out: Array[Dictionary] = []
	for item: Variant in value:
		if not (item is Dictionary):
			return []
		out.append((item as Dictionary).duplicate(true))
	return out


static func _record(cell: int, member: Dictionary, add_volume: float, dt_s: float,
		mouth_cells: float, cell_size_m: float, upstream: bool,
		downstream: bool) -> Dictionary:
	var direction_value: Variant = member.get("direction_cell", null)
	var center_value: Variant = member.get("center_cell", null)
	var velocity_value: Variant = member.get("local_velocity", null)
	var direction := direction_value as Vector2 if direction_value is Vector2 else Vector2.ZERO
	var center := center_value as Vector2 if center_value is Vector2 else Vector2.ZERO
	var velocity := velocity_value as Vector2 if velocity_value is Vector2 else Vector2.ZERO
	return {
		"cell": cell,
		"slot": int(member.get("slot", -1)),
		"center_cell": center,
		"direction_cell": direction,
		"half_width_m": maxf(float(member.get("half_width_m", 0.0)), cell_size_m * 0.5),
		"add_volume_m3": maxf(add_volume, 0.0),
		"exchange_dt_s": dt_s,
		"mouth_cells": mouth_cells,
		"add_velocity": velocity,
		"upstream_enabled": upstream,
		"downstream_enabled": downstream,
	}
