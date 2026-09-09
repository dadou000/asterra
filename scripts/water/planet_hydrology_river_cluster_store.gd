class_name PlanetHydrologyRiverClusterStore
extends PlanetHydrologyRiverCoupledStore
## Persistent 1D ownership store for sparse river clusters and cross-reach components.
##
## Individual cluster promotion remains supported. A component promotion/collapse,
## however, owns one transaction spanning every coarse reach so a confluence can
## never expose a half-promoted or half-collapsed ownership generation.

var _refined_components: Dictionary = {} # component_id -> Dictionary
var _component_by_cell: Dictionary = {} # coarse cell -> component_id
var _next_component_id := 1

var _pending_component_promotions: Dictionary = {} # tx -> Dictionary
var _pending_component_demotions: Dictionary = {} # tx -> Dictionary
var _next_component_promotion_transaction_id := 1
var _next_component_demotion_transaction_id := 1


func initialize(p_fields: PlanetFields) -> Error:
	var err := super.initialize(p_fields)
	if err != OK:
		return err
	_refined_components.clear()
	_component_by_cell.clear()
	_pending_component_promotions.clear()
	_pending_component_demotions.clear()
	_next_component_id = 1
	_next_component_promotion_transaction_id = 1
	_next_component_demotion_transaction_id = 1
	return OK


func step(dt_s: float) -> Dictionary:
	if not _pending_component_promotions.is_empty() or not _pending_component_demotions.is_empty():
		return {
			"error": ERR_BUSY,
			"reason": "component_ownership_transaction_pending",
			"pending_component_promotions": _pending_component_promotions.size(),
			"pending_component_demotions": _pending_component_demotions.size(),
		}
	return super.step(dt_s)


func pending_ownership_transaction_count() -> int:
	return super.pending_ownership_transaction_count() \
		+ _pending_component_promotions.size() + _pending_component_demotions.size()


func prepare_promotion(cell: int, requested_volume_m3: float) -> Dictionary:
	if not _pending_component_promotions.is_empty() or not _pending_component_demotions.is_empty():
		return _ownership_error(ERR_BUSY, "component_ownership_transaction_pending")
	return super.prepare_promotion(cell, requested_volume_m3)


func prepare_channel_promotion(cell: int, requested_volume_m3: float) -> Dictionary:
	if not _pending_component_promotions.is_empty() or not _pending_component_demotions.is_empty():
		return _ownership_error(ERR_BUSY, "component_ownership_transaction_pending")
	return super.prepare_channel_promotion(cell, requested_volume_m3)


## Reserve channel parcels for every reach in one component before any sparse state
## is published. `reach_volumes` maps coarse cell -> exact represented fine volume.
func prepare_component_channel_promotion(reach_volumes: Dictionary) -> Dictionary:
	if not initialized:
		return _ownership_error(ERR_UNCONFIGURED, "store_unconfigured")
	if pending_ownership_transaction_count() > 0:
		return _ownership_error(ERR_BUSY, "ownership_transaction_pending")
	if reach_volumes.size() < 2:
		return _ownership_error(ERR_INVALID_PARAMETER, "component_requires_multiple_reaches")

	var cells: Array[int] = []
	var normalized: Dictionary = {}
	var total := 0.0
	for key: Variant in reach_volumes.keys():
		var cell := int(key)
		var volume := float(reach_volumes[key])
		if cell < 0 or cell >= cell_count() or river_reaches == null \
				or not river_reaches.is_reach_cell(cell) or is_refined_reach(cell):
			return _ownership_error(ERR_INVALID_PARAMETER, "invalid_component_reach")
		if not is_finite(volume) or volume <= 0.0:
			return _ownership_error(ERR_INVALID_PARAMETER, "invalid_component_volume")
		var available := maxf(channel_storage_m3[cell] - _reserved_channel_m3[cell], 0.0)
		var epsilon := maxf(1.0e-9, available * 1.0e-12)
		if volume > available + epsilon:
			var rejected := _ownership_error(ERR_CANT_ACQUIRE_RESOURCE,
				"insufficient_free_channel_water")
			rejected["cell"] = cell
			rejected["available_volume_m3"] = available
			return rejected
		cells.append(cell)
		normalized[cell] = volume
		total += volume
	cells.sort()

	# Validate coarse topology before reserving anything.
	var topology := _build_component_topology(cells)
	if int(topology.get("error", FAILED)) != OK:
		return topology
	for cell in cells:
		_reserved_channel_m3[cell] += float(normalized[cell])

	var tx := _next_component_promotion_transaction_id
	_next_component_promotion_transaction_id += 1
	var transaction := {
		"error": OK,
		"transaction_id": tx,
		"transfer_direction": "coarse_component_1d_to_fine_2d",
		"representation": "river_component",
		"cells": PackedInt32Array(cells),
		"reach_volumes_m3": normalized.duplicate(true),
		"reserved_volume_m3": total,
		"topology": topology.get("record", {}).duplicate(true),
		"prepared_at_step": step_count,
		"prepared_at_simulated_seconds": simulated_seconds,
	}
	_pending_component_promotions[tx] = transaction
	return transaction.duplicate(true)


func commit_component_channel_promotion(transaction_id: int) -> Dictionary:
	if not _pending_component_promotions.has(transaction_id):
		return _ownership_error(ERR_DOES_NOT_EXIST, "unknown_component_transaction")
	var transaction := _pending_component_promotions[transaction_id] as Dictionary
	var cells := transaction.get("cells", PackedInt32Array()) as PackedInt32Array
	var volumes := transaction.get("reach_volumes_m3", {}) as Dictionary
	var total := float(transaction.get("reserved_volume_m3", 0.0))
	for raw_cell in cells:
		var cell := int(raw_cell)
		var volume := float(volumes.get(cell, 0.0))
		var epsilon := maxf(1.0e-9, volume * 1.0e-12)
		if channel_storage_m3[cell] + epsilon < volume \
				or _reserved_channel_m3[cell] + epsilon < volume:
			return _ownership_error(ERR_INVALID_DATA,
				"component_reserved_storage_invariant_broken")

	for raw_cell in cells:
		var cell := int(raw_cell)
		var volume := float(volumes[cell])
		channel_storage_m3[cell] = maxf(channel_storage_m3[cell] - volume, 0.0)
		_reserved_channel_m3[cell] = maxf(_reserved_channel_m3[cell] - volume, 0.0)
	_pending_component_promotions.erase(transaction_id)
	cumulative_promoted_to_fine_m3 += total
	var out := transaction.duplicate(true)
	out["committed"] = true
	out["coarse_storage_m3"] = total_storage_m3()
	out["mass_error_m3"] = mass_error_m3()
	return out


func rollback_component_channel_promotion(transaction_id: int) -> Dictionary:
	if not _pending_component_promotions.has(transaction_id):
		return _ownership_error(ERR_DOES_NOT_EXIST, "unknown_component_transaction")
	var transaction := _pending_component_promotions[transaction_id] as Dictionary
	var cells := transaction.get("cells", PackedInt32Array()) as PackedInt32Array
	var volumes := transaction.get("reach_volumes_m3", {}) as Dictionary
	for raw_cell in cells:
		var cell := int(raw_cell)
		_reserved_channel_m3[cell] = maxf(_reserved_channel_m3[cell]
			- float(volumes.get(cell, 0.0)), 0.0)
	_pending_component_promotions.erase(transaction_id)
	var out := transaction.duplicate(true)
	out["rolled_back"] = true
	return out


## Reserve all fine->coarse component parcels as one incoming transaction. Fine
## tiles remain authoritative until the caller has unpublished the entire component.
func prepare_component_channel_demotion(component_id: int,
		reach_volumes: Dictionary) -> Dictionary:
	if not initialized or component_id < 0 or not _refined_components.has(component_id):
		return _ownership_error(ERR_INVALID_PARAMETER, "component_not_found")
	if pending_ownership_transaction_count() > 0:
		return _ownership_error(ERR_BUSY, "ownership_transaction_pending")
	var component := _refined_components[component_id] as Dictionary
	var cells := component.get("cells", PackedInt32Array()) as PackedInt32Array
	var normalized: Dictionary = {}
	var total := 0.0
	for raw_cell in cells:
		var cell := int(raw_cell)
		var volume := float(reach_volumes.get(cell, 0.0))
		if not is_finite(volume) or volume < 0.0:
			return _ownership_error(ERR_INVALID_PARAMETER, "invalid_component_demotion_volume")
		normalized[cell] = volume
		total += volume
	if total <= 0.0:
		return _ownership_error(ERR_INVALID_PARAMETER, "empty_component_demotion")

	var tx := _next_component_demotion_transaction_id
	_next_component_demotion_transaction_id += 1
	var transaction := {
		"error": OK,
		"transaction_id": tx,
		"component_id": component_id,
		"transfer_direction": "fine_component_2d_to_coarse_1d",
		"cells": cells.duplicate(),
		"reach_volumes_m3": normalized,
		"incoming_volume_m3": total,
		"prepared_at_step": step_count,
		"prepared_at_simulated_seconds": simulated_seconds,
	}
	_pending_component_demotions[tx] = transaction
	return transaction.duplicate(true)


func commit_component_channel_demotion(transaction_id: int) -> Dictionary:
	if not _pending_component_demotions.has(transaction_id):
		return _ownership_error(ERR_DOES_NOT_EXIST, "unknown_component_transaction")
	var transaction := _pending_component_demotions[transaction_id] as Dictionary
	var cells := transaction.get("cells", PackedInt32Array()) as PackedInt32Array
	var volumes := transaction.get("reach_volumes_m3", {}) as Dictionary
	var total := float(transaction.get("incoming_volume_m3", 0.0))
	for raw_cell in cells:
		var cell := int(raw_cell)
		channel_storage_m3[cell] += maxf(float(volumes.get(cell, 0.0)), 0.0)
	_pending_component_demotions.erase(transaction_id)
	cumulative_demoted_from_fine_m3 += total
	var out := transaction.duplicate(true)
	out["committed"] = true
	out["accepted_volume_m3"] = total
	out["coarse_storage_m3"] = total_storage_m3()
	out["mass_error_m3"] = mass_error_m3()
	return out


func rollback_component_channel_demotion(transaction_id: int) -> Dictionary:
	if not _pending_component_demotions.has(transaction_id):
		return _ownership_error(ERR_DOES_NOT_EXIST, "unknown_component_transaction")
	var transaction := (_pending_component_demotions[transaction_id] as Dictionary).duplicate(true)
	_pending_component_demotions.erase(transaction_id)
	transaction["rolled_back"] = true
	return transaction


func register_refined_cluster(cell: int, members: Array[Dictionary],
		represented_volume_m3: float) -> Dictionary:
	if not initialized or river_reaches == null or not river_reaches.is_reach_cell(cell):
		return _ownership_error(ERR_INVALID_PARAMETER, "not_a_river_reach")
	if is_refined_reach(cell):
		return _ownership_error(ERR_ALREADY_EXISTS, "reach_already_refined")
	var record_result := _make_refined_record(cell, members, represented_volume_m3, {})
	if int(record_result.get("error", FAILED)) != OK:
		return record_result
	var record := record_result["record"] as Dictionary
	_publish_refined_record(cell, record)
	return {"error": OK, "record": record.duplicate(true)}


## Atomically publish all reach holes plus the component topology after every fine
## tile has been seeded. No component member is visible in the store before this call.
func register_refined_component(reach_reports: Array[Dictionary],
		junctions: Array[Dictionary] = []) -> Dictionary:
	if not initialized or reach_reports.size() < 2 or pending_ownership_transaction_count() > 0:
		return _ownership_error(ERR_BUSY, "component_registration_not_quiescent")
	var records: Dictionary = {}
	var cells: Array[int] = []
	var identity_seen: Dictionary = {}
	for report in reach_reports:
		var cell := int(report.get("cell", -1))
		var represented := float(report.get("represented_volume_m3", 0.0))
		var members_value: Variant = report.get("members", null)
		if cell < 0 or not (members_value is Array) or is_refined_reach(cell) \
				or records.has(cell):
			return _ownership_error(ERR_INVALID_PARAMETER, "invalid_component_reach_report")
		var members: Array[Dictionary] = []
		for value: Variant in members_value:
			if not (value is Dictionary):
				return _ownership_error(ERR_INVALID_PARAMETER, "invalid_component_member")
			members.append((value as Dictionary).duplicate(true))
		var made := _make_refined_record(cell, members, represented, identity_seen)
		if int(made.get("error", FAILED)) != OK:
			return made
		records[cell] = made["record"]
		cells.append(cell)
	cells.sort()

	var component_id := _next_component_id
	var built := _build_component_record_from_records(cells, component_id, records)
	if int(built.get("error", FAILED)) != OK:
		return built
	var component := built["record"] as Dictionary
	var junction_result := _normalize_component_junctions(component, junctions, records)
	if int(junction_result.get("error", FAILED)) != OK:
		return junction_result
	component["junctions"] = junction_result.get("junctions", [])
	component["junction_count"] = int(junction_result.get("junction_count", 0))
	component["fine_junction_verified"] = bool(junction_result.get("verified", false))
	component["physical_topology_verified"] = bool(junction_result.get("verified", false)) \
		or int(component.get("upstream_mouth_count", 0)) <= 1
	component["published_atomically"] = true

	# Commit point: all validation above is side-effect free.
	for cell in cells:
		_publish_refined_record(cell, records[cell] as Dictionary)
	_refined_components[component_id] = component
	for cell in cells:
		_component_by_cell[cell] = component_id
	_next_component_id += 1
	return {
		"error": OK,
		"component_id": component_id,
		"component": component.duplicate(true),
		"reach_records": records.duplicate(true),
	}


## Atomically remove the component topology and every individual refinement record.
## Fine state must already have left the atlas before this is called.
func unregister_refined_component(component_id: int,
		return_pending_to_channel: bool = true) -> Dictionary:
	if component_id < 0 or not _refined_components.has(component_id):
		return _ownership_error(ERR_DOES_NOT_EXIST, "component_not_found")
	var component := (_refined_components[component_id] as Dictionary).duplicate(true)
	var cells := component.get("cells", PackedInt32Array()) as PackedInt32Array
	var returned_by_cell: Dictionary = {}
	var total_returned := 0.0
	for raw_cell in cells:
		var cell := int(raw_cell)
		if not is_refined_reach(cell):
			return _ownership_error(ERR_INVALID_DATA, "component_member_not_refined")
	for raw_cell in cells:
		var cell := int(raw_cell)
		var pending := maxf(refined_pending_inflow_m3[cell], 0.0)
		if return_pending_to_channel and pending > 0.0:
			channel_storage_m3[cell] += pending
		returned_by_cell[cell] = pending
		total_returned += pending
		refined_pending_inflow_m3[cell] = 0.0
		refined_inflow_rate_m3s[cell] = 0.0
		_refined_step_inflow_m3[cell] = 0.0
		refined_mask[cell] = 0
		_refined_records.erase(cell)
		_component_by_cell.erase(cell)
	_refined_components.erase(component_id)
	return {
		"error": OK,
		"component_id": component_id,
		"component": component,
		"returned_pending_by_cell_m3": returned_by_cell,
		"returned_pending_m3": total_returned,
	}


## Metadata-only union retained for manual/pre-existing refined reaches.
func merge_refined_clusters(cells: PackedInt32Array) -> Dictionary:
	if not initialized or cells.size() < 2:
		return _ownership_error(ERR_INVALID_PARAMETER, "invalid_component_request")
	var candidate: Dictionary = {}
	var source_component_ids: Array[int] = []
	for raw_cell in cells:
		var cell := int(raw_cell)
		if not is_refined_reach(cell):
			return _ownership_error(ERR_INVALID_PARAMETER, "component_reach_not_refined")
		candidate[cell] = true
		var existing_id := int(_component_by_cell.get(cell, -1))
		if existing_id >= 0:
			if not _refined_components.has(existing_id):
				return _ownership_error(ERR_INVALID_DATA, "component_registry_corrupt")
			if not source_component_ids.has(existing_id):
				source_component_ids.append(existing_id)
			var existing := _refined_components[existing_id] as Dictionary
			var existing_cells := existing.get("cells", PackedInt32Array()) as PackedInt32Array
			for existing_cell in existing_cells:
				candidate[int(existing_cell)] = true

	var normalized_cells: Array[int] = []
	for key: Variant in candidate.keys():
		normalized_cells.append(int(key))
	normalized_cells.sort()
	var provisional_id := source_component_ids[0] if not source_component_ids.is_empty() \
		else _next_component_id
	var built := _build_component_record(normalized_cells, provisional_id)
	if int(built.get("error", FAILED)) != OK:
		return built
	var record := built["record"] as Dictionary
	if source_component_ids.size() == 1:
		var old := _refined_components[source_component_ids[0]] as Dictionary
		if _same_cells(old, record):
			return {
				"error": OK, "component_id": source_component_ids[0],
				"component": old.duplicate(true), "changed": false,
				"merged_component_ids": PackedInt32Array(source_component_ids),
			}
	var component_id := provisional_id
	if source_component_ids.is_empty():
		_next_component_id += 1
	for old_id in source_component_ids:
		_erase_component(old_id)
	record["component_id"] = component_id
	record["merged_component_ids"] = PackedInt32Array(source_component_ids)
	record["fine_junction_verified"] = false
	record["physical_topology_verified"] = int(record.get("upstream_mouth_count", 0)) <= 1
	_refined_components[component_id] = record
	var component_cells := record.get("cells", PackedInt32Array()) as PackedInt32Array
	for cell in component_cells:
		_component_by_cell[int(cell)] = component_id
	return {
		"error": OK, "component_id": component_id,
		"component": record.duplicate(true), "changed": true,
		"merged_component_ids": PackedInt32Array(source_component_ids),
	}


func dissolve_refined_component(component_id: int) -> Dictionary:
	if component_id < 0 or not _refined_components.has(component_id):
		return _ownership_error(ERR_DOES_NOT_EXIST, "component_not_found")
	var record := (_refined_components[component_id] as Dictionary).duplicate(true)
	_erase_component(component_id)
	return {"error": OK, "component_id": component_id, "component": record}


func refined_component_count() -> int:
	return _refined_components.size()


func refined_component_id_for_cell(cell: int) -> int:
	return int(_component_by_cell.get(cell, -1))


func refined_component_for_cell(cell: int) -> Dictionary:
	var component_id := refined_component_id_for_cell(cell)
	return refined_component(component_id)


func refined_component(component_id: int) -> Dictionary:
	if component_id < 0 or not _refined_components.has(component_id):
		return {}
	return (_refined_components[component_id] as Dictionary).duplicate(true)


func unregister_refined_reach(cell: int,
		return_pending_to_channel: bool = true) -> Dictionary:
	if _component_by_cell.has(cell):
		return _ownership_error(ERR_BUSY, "reach_belongs_to_refined_component")
	return super.unregister_refined_reach(cell, return_pending_to_channel)


func refined_cluster_members(cell: int) -> Array[Dictionary]:
	if not is_refined_reach(cell):
		return []
	var record := _refined_records[cell] as Dictionary
	var value: Variant = record.get("members", [])
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
	}]


func refined_cluster_size(cell: int) -> int:
	return refined_cluster_members(cell).size()


func refined_sparse_member_count() -> int:
	var members := 0
	for value: Variant in _refined_records.values():
		if value is Dictionary:
			members += maxi(int((value as Dictionary).get("member_count", 1)), 1)
	return members


func snapshot() -> Dictionary:
	if not _pending_component_promotions.is_empty() or not _pending_component_demotions.is_empty():
		return {}
	return super.snapshot()


func _make_refined_record(cell: int, members: Array[Dictionary],
		represented_volume_m3: float, identity_seen: Dictionary) -> Dictionary:
	if river_reaches == null or not river_reaches.is_reach_cell(cell) \
			or members.is_empty() or not is_finite(represented_volume_m3) \
			or represented_volume_m3 <= 0.0:
		return _ownership_error(ERR_INVALID_PARAMETER, "invalid_cluster")
	var local_tiles: Dictionary = {}
	var local_slots: Dictionary = {}
	var normalized: Array[Dictionary] = []
	for i in members.size():
		var member := members[i]
		var tile_id := int(member.get("tile_id", -1))
		var slot := int(member.get("slot", -1))
		if tile_id < 0 or slot < 0 or local_tiles.has(tile_id) or local_slots.has(slot) \
				or identity_seen.has("t:%d" % tile_id) or identity_seen.has("s:%d" % slot) \
				or _refined_identity_in_use(tile_id, slot):
			return _ownership_error(ERR_ALREADY_EXISTS, "cluster_member_identity_in_use")
		local_tiles[tile_id] = true
		local_slots[slot] = true
		identity_seen["t:%d" % tile_id] = true
		identity_seen["s:%d" % slot] = true
		var out := member.duplicate(true)
		out["index"] = i
		normalized.append(out)

	var storage_before := channel_storage_m3[cell] + represented_volume_m3
	var depth := river_reaches.depth_from_storage(cell,
		minf(storage_before, river_reaches.bankfull_storage_for_cell(cell)))
	var area := river_reaches.cross_section_area(cell, depth)
	var represented_length := represented_volume_m3 / maxf(area, 1.0e-9)
	represented_length = clampf(represented_length, 0.0,
		maxf(river_reaches.reach_length_m[cell], 0.0))
	var reach_length := maxf(river_reaches.reach_length_m[cell], 0.0)
	var residual_length := maxf(reach_length - represented_length, 0.0)
	var first := normalized[0]
	return {
		"error": OK,
		"record": {
			"cell": cell,
			"tile_id": int(first["tile_id"]),
			"slot": int(first["slot"]),
			"represented_volume_m3": represented_volume_m3,
			"represented_length_m": represented_length,
			"residual_length_m": residual_length,
			"reach_length_m": reach_length,
			"receiver": receiver[cell],
			"members": normalized,
			"member_count": normalized.size(),
			"fully_represented": residual_length <= maxf(1.0e-4, reach_length * 1.0e-6),
			"representation": "sparse_2d_river_cluster",
		},
	}


func _publish_refined_record(cell: int, record: Dictionary) -> void:
	_refined_records[cell] = record
	refined_mask[cell] = 1
	refined_pending_inflow_m3[cell] = 0.0
	refined_inflow_rate_m3s[cell] = 0.0
	_refined_step_inflow_m3[cell] = 0.0


func _build_component_topology(cells: Array[int]) -> Dictionary:
	var membership: Dictionary = {}
	var indegree: Dictionary = {}
	for cell in cells:
		if membership.has(cell):
			return _ownership_error(ERR_INVALID_PARAMETER, "duplicate_component_cell")
		membership[cell] = true
		indegree[cell] = 0
	var edges: Array[Dictionary] = []
	var outlets: Array[int] = []
	for cell in cells:
		var downstream := int(receiver[cell])
		if membership.has(downstream):
			edges.append({"from_cell": cell, "to_cell": downstream})
			indegree[downstream] = int(indegree.get(downstream, 0)) + 1
		else:
			outlets.append(cell)
	if outlets.size() != 1:
		return _ownership_error(ERR_CANT_RESOLVE, "component_requires_single_downstream_outlet")
	var roots: Array[int] = []
	for cell in cells:
		if int(indegree[cell]) == 0:
			roots.append(cell)
	roots.sort()
	if roots.is_empty():
		return _ownership_error(ERR_CANT_RESOLVE, "component_has_no_upstream_root")
	var work := indegree.duplicate()
	var queue: Array[int] = roots.duplicate()
	var ordered: Array[int] = []
	while not queue.is_empty():
		var cell := queue.pop_front()
		ordered.append(cell)
		var downstream := int(receiver[cell])
		if membership.has(downstream):
			work[downstream] = int(work[downstream]) - 1
			if int(work[downstream]) == 0:
				queue.append(downstream)
				queue.sort()
	if ordered.size() != cells.size():
		return _ownership_error(ERR_CANT_RESOLVE, "component_receiver_cycle")
	return {
		"error": OK,
		"record": {
			"cells": PackedInt32Array(ordered),
			"reach_count": ordered.size(),
			"upstream_mouth_cells": PackedInt32Array(roots),
			"upstream_mouth_count": roots.size(),
			"downstream_outlet_cell": outlets[0],
			"downstream_receiver": int(receiver[outlets[0]]),
			"internal_reach_edges": edges,
		},
	}


func _build_component_record(cells: Array[int], component_id: int) -> Dictionary:
	return _build_component_record_from_records(cells, component_id, _refined_records)


func _build_component_record_from_records(cells: Array[int], component_id: int,
		records: Dictionary) -> Dictionary:
	var topology := _build_component_topology(cells)
	if int(topology.get("error", FAILED)) != OK:
		return topology
	var record := (topology["record"] as Dictionary).duplicate(true)
	var seen_tiles: Dictionary = {}
	var seen_slots: Dictionary = {}
	var fine_tile_ids := PackedInt64Array()
	var fine_slots := PackedInt32Array()
	var fine_member_count := 0
	var ordered := record.get("cells", PackedInt32Array()) as PackedInt32Array
	for raw_cell in ordered:
		var cell := int(raw_cell)
		if not records.has(cell):
			return _ownership_error(ERR_INVALID_DATA, "component_refined_members_missing")
		var reach_record := records[cell] as Dictionary
		var members_value: Variant = reach_record.get("members", null)
		if not (members_value is Array) or (members_value as Array).is_empty():
			return _ownership_error(ERR_INVALID_DATA, "component_refined_members_missing")
		for value: Variant in members_value:
			if not (value is Dictionary):
				return _ownership_error(ERR_INVALID_DATA, "component_member_invalid")
			var member := value as Dictionary
			var tile_id := int(member.get("tile_id", -1))
			var slot := int(member.get("slot", -1))
			if tile_id < 0 or slot < 0 or seen_tiles.has(tile_id) or seen_slots.has(slot):
				return _ownership_error(ERR_INVALID_DATA, "component_member_identity_overlap")
			seen_tiles[tile_id] = true
			seen_slots[slot] = true
			fine_tile_ids.append(tile_id)
			fine_slots.append(slot)
			fine_member_count += 1
	record["component_id"] = component_id
	record["fine_tile_ids"] = fine_tile_ids
	record["fine_slots"] = fine_slots
	record["fine_member_count"] = fine_member_count
	record["topology"] = "directed_single_outlet_confluence"
	record["representation"] = "sparse_2d_river_component"
	record["ownership_changed"] = false
	return {"error": OK, "record": record}


func _normalize_component_junctions(component: Dictionary,
		junctions: Array[Dictionary], records: Dictionary) -> Dictionary:
	var indegree: Dictionary = {}
	var cells := component.get("cells", PackedInt32Array()) as PackedInt32Array
	for raw_cell in cells:
		indegree[int(raw_cell)] = 0
	var edges_value: Variant = component.get("internal_reach_edges", [])
	if edges_value is Array:
		for value: Variant in edges_value:
			if value is Dictionary:
				var to_cell := int((value as Dictionary).get("to_cell", -1))
				indegree[to_cell] = int(indegree.get(to_cell, 0)) + 1
	var required: Dictionary = {}
	for raw_cell in cells:
		var cell := int(raw_cell)
		if int(indegree.get(cell, 0)) > 1:
			required[cell] = true
	if required.is_empty():
		return {"error": OK, "junctions": [], "junction_count": 0, "verified": true}

	var normalized: Array[Dictionary] = []
	var seen: Dictionary = {}
	for value: Variant in junctions:
		if not (value is Dictionary):
			return _ownership_error(ERR_INVALID_PARAMETER, "invalid_component_junction")
		var junction := (value as Dictionary).duplicate(true)
		var cell := int(junction.get("cell", -1))
		if not required.has(cell) or seen.has(cell) or not records.has(cell) \
				or not bool(junction.get("verified", false)):
			return _ownership_error(ERR_INVALID_DATA, "unverified_component_junction")
		var tile_id := int(junction.get("tile_id", -1))
		var slot := int(junction.get("slot", -1))
		var members := (records[cell] as Dictionary).get("members", []) as Array
		var owned := false
		for member_value: Variant in members:
			if member_value is Dictionary:
				var member := member_value as Dictionary
				if int(member.get("tile_id", -2)) == tile_id \
						and int(member.get("slot", -2)) == slot:
					owned = true
					break
		if not owned:
			return _ownership_error(ERR_INVALID_DATA, "junction_tile_not_owned_by_reach")
		seen[cell] = true
		normalized.append(junction)
	if seen.size() != required.size():
		return _ownership_error(ERR_INVALID_DATA, "component_junction_missing")
	return {
		"error": OK, "junctions": normalized,
		"junction_count": normalized.size(), "verified": true,
	}


func _same_cells(a: Dictionary, b: Dictionary) -> bool:
	var a_cells := a.get("cells", PackedInt32Array()) as PackedInt32Array
	var b_cells := b.get("cells", PackedInt32Array()) as PackedInt32Array
	if a_cells.size() != b_cells.size():
		return false
	var left := Array(a_cells)
	var right := Array(b_cells)
	left.sort(); right.sort()
	for i in left.size():
		if int(left[i]) != int(right[i]):
			return false
	return true


func _erase_component(component_id: int) -> void:
	if not _refined_components.has(component_id):
		return
	var record := _refined_components[component_id] as Dictionary
	var cells := record.get("cells", PackedInt32Array()) as PackedInt32Array
	for raw_cell in cells:
		var cell := int(raw_cell)
		if int(_component_by_cell.get(cell, -1)) == component_id:
			_component_by_cell.erase(cell)
	_refined_components.erase(component_id)


func _refined_identity_in_use(tile_id: int, slot: int) -> bool:
	for value: Variant in _refined_records.values():
		if not (value is Dictionary):
			continue
		var record := value as Dictionary
		var members_value: Variant = record.get("members", null)
		if members_value is Array and not (members_value as Array).is_empty():
			for item: Variant in members_value:
				if item is Dictionary:
					var member := item as Dictionary
					if int(member.get("tile_id", -1)) == tile_id \
							or int(member.get("slot", -1)) == slot:
						return true
		else:
			if int(record.get("tile_id", -1)) == tile_id \
					or int(record.get("slot", -1)) == slot:
				return true
	return false


func stats() -> Dictionary:
	var out := super.stats()
	var clusters := 0
	for value: Variant in _refined_records.values():
		if value is Dictionary and int((value as Dictionary).get("member_count", 1)) > 1:
			clusters += 1
	var component_reaches := 0
	var component_members := 0
	var verified_junctions := 0
	var multi_mouth_components := 0
	var max_upstream_mouths := 0
	for value: Variant in _refined_components.values():
		if not (value is Dictionary):
			continue
		var component := value as Dictionary
		component_reaches += int(component.get("reach_count", 0))
		component_members += int(component.get("fine_member_count", 0))
		verified_junctions += int(component.get("junction_count", 0))
		var mouths := int(component.get("upstream_mouth_count", 0))
		max_upstream_mouths = maxi(max_upstream_mouths, mouths)
		if mouths > 1:
			multi_mouth_components += 1
	out["multi_tile_river_clusters"] = clusters
	out["refined_sparse_members"] = refined_sparse_member_count()
	out["river_refined_components"] = _refined_components.size()
	out["component_refined_reaches"] = component_reaches
	out["component_sparse_members"] = component_members
	out["verified_component_junctions"] = verified_junctions
	out["multi_upstream_mouth_components"] = multi_mouth_components
	out["max_component_upstream_mouths"] = max_upstream_mouths
	out["pending_component_promotions"] = _pending_component_promotions.size()
	out["pending_component_demotions"] = _pending_component_demotions.size()
	out["component_ownership_transactional"] = true
	out["component_union_transactional"] = true
	return out
