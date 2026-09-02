class_name PlanetHydrologyRiverClusterStore
extends PlanetHydrologyRiverCoupledStore
## Coarse 1D river store extended with ordered multi-tile fine refinement records.
##
## A cluster is still ONE coarse refinement hole / ownership parcel. The members
## array only describes how that parcel is spatially represented in sparse SWE.
## This preserves all inherited pending-inflow, routing and mass-ledger semantics.
##
## Cross-reach components are deliberately metadata-only at this layer. They bind
## two or more already-refined coarse reaches into one directed fine component
## without moving any water or altering the individual ownership holes. The later
## multi-mouth GPU coupling bridge can therefore consume one atomic, validated
## topology contract instead of inferring confluences from partially-mutated state.

var _refined_components: Dictionary = {} # component_id -> Dictionary
var _component_by_cell: Dictionary = {} # coarse cell -> component_id
var _next_component_id := 1


func initialize(p_fields: PlanetFields) -> Error:
	var err := super.initialize(p_fields)
	if err != OK:
		return err
	_refined_components.clear()
	_component_by_cell.clear()
	_next_component_id = 1
	return OK


func register_refined_cluster(cell: int, members: Array[Dictionary],
		represented_volume_m3: float) -> Dictionary:
	if not initialized or river_reaches == null or not river_reaches.is_reach_cell(cell):
		return _ownership_error(ERR_INVALID_PARAMETER, "not_a_river_reach")
	if is_refined_reach(cell):
		return _ownership_error(ERR_ALREADY_EXISTS, "reach_already_refined")
	if members.is_empty() or not is_finite(represented_volume_m3) \
			or represented_volume_m3 <= 0.0:
		return _ownership_error(ERR_INVALID_PARAMETER, "invalid_cluster")

	var seen_tiles: Dictionary = {}
	var seen_slots: Dictionary = {}
	var normalized: Array[Dictionary] = []
	for i in members.size():
		var member := members[i]
		var tile_id := int(member.get("tile_id", -1))
		var slot := int(member.get("slot", -1))
		if tile_id < 0 or slot < 0 or seen_tiles.has(tile_id) or seen_slots.has(slot):
			return _ownership_error(ERR_INVALID_PARAMETER, "invalid_cluster_member")
		if _refined_identity_in_use(tile_id, slot):
			return _ownership_error(ERR_ALREADY_EXISTS, "cluster_member_identity_in_use")
		seen_tiles[tile_id] = true
		seen_slots[slot] = true
		var out := member.duplicate(true)
		out["index"] = i
		normalized.append(out)

	var storage_before_promotion := channel_storage_m3[cell] + represented_volume_m3
	var depth := river_reaches.depth_from_storage(cell,
		minf(storage_before_promotion, river_reaches.bankfull_storage_for_cell(cell)))
	var area := river_reaches.cross_section_area(cell, depth)
	var represented_length := represented_volume_m3 / maxf(area, 1.0e-9)
	represented_length = clampf(represented_length, 0.0,
		maxf(river_reaches.reach_length_m[cell], 0.0))
	var residual_length := maxf(river_reaches.reach_length_m[cell] - represented_length, 0.0)
	var first := normalized[0]
	var record := {
		"cell": cell,
		# Legacy primary identity remains the upstream member for old diagnostics.
		"tile_id": int(first["tile_id"]),
		"slot": int(first["slot"]),
		"represented_volume_m3": represented_volume_m3,
		"represented_length_m": represented_length,
		"residual_length_m": residual_length,
		"reach_length_m": river_reaches.reach_length_m[cell],
		"receiver": receiver[cell],
		"members": normalized,
		"member_count": normalized.size(),
		"representation": "sparse_2d_river_cluster",
	}
	_refined_records[cell] = record
	refined_mask[cell] = 1
	refined_pending_inflow_m3[cell] = 0.0
	refined_inflow_rate_m3s[cell] = 0.0
	_refined_step_inflow_m3[cell] = 0.0
	return {"error": OK, "record": record.duplicate(true)}


## A component is a topology contract spanning multiple already-refined reaches.
## The operation is atomic: every requested/existing component member is expanded,
## validated and normalized before any registry mutation occurs.
##
## Existing components referenced by any requested cell are unioned automatically.
## The resulting directed graph must be acyclic and have exactly one downstream
## outlet to coarse 1D. Multiple upstream roots are allowed (the confluence case).
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
		var cell := int(key)
		if not is_refined_reach(cell):
			return _ownership_error(ERR_INVALID_DATA, "component_member_lost_refinement")
		normalized_cells.append(cell)
	normalized_cells.sort()
	if normalized_cells.size() < 2:
		return _ownership_error(ERR_INVALID_PARAMETER, "component_requires_multiple_reaches")

	# Include component IDs reached through expanded cells as a final consistency
	# pass. A valid registry maps each cell to at most one component.
	for cell in normalized_cells:
		var existing_id := int(_component_by_cell.get(cell, -1))
		if existing_id >= 0 and not source_component_ids.has(existing_id):
			if not _refined_components.has(existing_id):
				return _ownership_error(ERR_INVALID_DATA, "component_registry_corrupt")
			source_component_ids.append(existing_id)
	source_component_ids.sort()

	var provisional_id := source_component_ids[0] if not source_component_ids.is_empty() \
		else _next_component_id
	var built := _build_component_record(normalized_cells, provisional_id)
	if int(built.get("error", FAILED)) != OK:
		return built
	var record := built.get("record", {}) as Dictionary

	if source_component_ids.size() == 1:
		var old := _refined_components[source_component_ids[0]] as Dictionary
		if _same_cells(old, record):
			return {
				"error": OK,
				"component_id": source_component_ids[0],
				"component": old.duplicate(true),
				"changed": false,
				"merged_component_ids": PackedInt32Array(source_component_ids),
			}

	var component_id := provisional_id
	if source_component_ids.is_empty():
		_next_component_id += 1

	# Commit point. No ownership or water volumes are changed here.
	for old_id in source_component_ids:
		_erase_component(old_id)
	record["component_id"] = component_id
	record["merged_component_ids"] = PackedInt32Array(source_component_ids)
	_refined_components[component_id] = record
	var component_cells := record.get("cells", PackedInt32Array()) as PackedInt32Array
	for cell in component_cells:
		_component_by_cell[int(cell)] = component_id

	return {
		"error": OK,
		"component_id": component_id,
		"component": record.duplicate(true),
		"changed": true,
		"merged_component_ids": PackedInt32Array(source_component_ids),
	}


## Remove only cross-reach union metadata. Individual refined reaches and every
## coarse/fine water parcel remain untouched.
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
	if component_id < 0 or not _refined_components.has(component_id):
		return {}
	return (_refined_components[component_id] as Dictionary).duplicate(true)


func refined_component(component_id: int) -> Dictionary:
	if not _refined_components.has(component_id):
		return {}
	return (_refined_components[component_id] as Dictionary).duplicate(true)


## A reach cannot disappear from underneath an active component contract. The
## component-level collapse bridge must dissolve the component first, after it has
## conservatively decided how to return all member parcels.
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
	if value is Array:
		var out: Array[Dictionary] = []
		for item: Variant in value:
			if item is Dictionary:
				out.append((item as Dictionary).duplicate(true))
		return out
	# Legacy one-tile reach is a one-member cluster for callers that understand both.
	return [{
		"index": 0,
		"tile_id": int(record.get("tile_id", -1)),
		"slot": int(record.get("slot", -1)),
	}]


func refined_cluster_size(cell: int) -> int:
	return refined_cluster_members(cell).size()


## O(number of refined reaches), used by low-cadence capacity policy. Do not route
## this through stats(), whose parent currently aggregates some values over all
## planetary coarse cells.
func refined_sparse_member_count() -> int:
	var members := 0
	for value: Variant in _refined_records.values():
		if not (value is Dictionary):
			continue
		members += maxi(int((value as Dictionary).get("member_count", 1)), 1)
	return members


func _build_component_record(cells: Array[int], component_id: int) -> Dictionary:
	var membership: Dictionary = {}
	var indegree: Dictionary = {}
	for cell in cells:
		membership[cell] = true
		indegree[cell] = 0

	var internal_edges: Array[Dictionary] = []
	var outlet_cells: Array[int] = []
	for cell in cells:
		var downstream := int(receiver[cell])
		if membership.has(downstream):
			internal_edges.append({"from_cell": cell, "to_cell": downstream})
			indegree[downstream] = int(indegree.get(downstream, 0)) + 1
		else:
			outlet_cells.append(cell)
	if outlet_cells.size() != 1:
		return _ownership_error(ERR_CANT_RESOLVE, "component_requires_single_downstream_outlet")

	var roots: Array[int] = []
	for cell in cells:
		if int(indegree.get(cell, 0)) == 0:
			roots.append(cell)
	roots.sort()
	if roots.is_empty():
		return _ownership_error(ERR_CANT_RESOLVE, "component_has_no_upstream_root")

	# Kahn ordering proves the directed receiver graph is acyclic. With one external
	# outlet and one receiver per reach, visiting every node also proves that every
	# branch drains into the same connected component.
	var work_indegree := indegree.duplicate()
	var queue: Array[int] = roots.duplicate()
	var topological: Array[int] = []
	while not queue.is_empty():
		var cell := queue.pop_front()
		topological.append(cell)
		var downstream := int(receiver[cell])
		if membership.has(downstream):
			var next_degree := int(work_indegree.get(downstream, 0)) - 1
			work_indegree[downstream] = next_degree
			if next_degree == 0:
				queue.append(downstream)
				queue.sort()
	if topological.size() != cells.size():
		return _ownership_error(ERR_CANT_RESOLVE, "component_receiver_cycle")

	var seen_tiles: Dictionary = {}
	var seen_slots: Dictionary = {}
	var fine_member_count := 0
	var fine_tile_ids := PackedInt64Array()
	var fine_slots := PackedInt32Array()
	for cell in topological:
		var members := refined_cluster_members(cell)
		if members.is_empty():
			return _ownership_error(ERR_INVALID_DATA, "component_refined_members_missing")
		for member in members:
			var tile_id := int(member.get("tile_id", -1))
			var slot := int(member.get("slot", -1))
			if tile_id < 0 or slot < 0 or seen_tiles.has(tile_id) or seen_slots.has(slot):
				return _ownership_error(ERR_INVALID_DATA, "component_member_identity_overlap")
			seen_tiles[tile_id] = true
			seen_slots[slot] = true
			fine_tile_ids.append(tile_id)
			fine_slots.append(slot)
			fine_member_count += 1

	var packed_cells := PackedInt32Array()
	for cell in topological:
		packed_cells.append(cell)
	var packed_roots := PackedInt32Array()
	for cell in roots:
		packed_roots.append(cell)

	return {
		"error": OK,
		"record": {
			"component_id": component_id,
			"cells": packed_cells,
			"reach_count": packed_cells.size(),
			"upstream_mouth_cells": packed_roots,
			"upstream_mouth_count": packed_roots.size(),
			"downstream_outlet_cell": outlet_cells[0],
			"downstream_receiver": int(receiver[outlet_cells[0]]),
			"internal_reach_edges": internal_edges,
			"fine_tile_ids": fine_tile_ids,
			"fine_slots": fine_slots,
			"fine_member_count": fine_member_count,
			"topology": "directed_single_outlet_confluence",
			"representation": "sparse_2d_river_component",
			"ownership_changed": false,
		},
	}


func _same_cells(a: Dictionary, b: Dictionary) -> bool:
	var a_cells := a.get("cells", PackedInt32Array()) as PackedInt32Array
	var b_cells := b.get("cells", PackedInt32Array()) as PackedInt32Array
	if a_cells.size() != b_cells.size():
		return false
	var left := Array(a_cells)
	var right := Array(b_cells)
	left.sort()
	right.sort()
	for i in left.size():
		if int(left[i]) != int(right[i]):
			return false
	return true


func _erase_component(component_id: int) -> void:
	if not _refined_components.has(component_id):
		return
	var record := _refined_components[component_id] as Dictionary
	var cells := record.get("cells", PackedInt32Array()) as PackedInt32Array
	for cell in cells:
		if int(_component_by_cell.get(int(cell), -1)) == component_id:
			_component_by_cell.erase(int(cell))
	_refined_components.erase(component_id)


func _refined_identity_in_use(tile_id: int, slot: int) -> bool:
	for value: Variant in _refined_records.values():
		if not (value is Dictionary):
			continue
		var record := value as Dictionary
		var members_value: Variant = record.get("members", null)
		if members_value is Array and not (members_value as Array).is_empty():
			for item: Variant in members_value:
				if not (item is Dictionary):
					continue
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
		var record := value as Dictionary
		if int(record.get("member_count", 1)) > 1:
			clusters += 1
	var component_reaches := 0
	var component_members := 0
	var multi_mouth_components := 0
	var max_upstream_mouths := 0
	for value: Variant in _refined_components.values():
		if not (value is Dictionary):
			continue
		var component := value as Dictionary
		component_reaches += int(component.get("reach_count", 0))
		component_members += int(component.get("fine_member_count", 0))
		var mouth_count := int(component.get("upstream_mouth_count", 0))
		max_upstream_mouths = maxi(max_upstream_mouths, mouth_count)
		if mouth_count > 1:
			multi_mouth_components += 1
	out["multi_tile_river_clusters"] = clusters
	out["refined_sparse_members"] = refined_sparse_member_count()
	out["river_refined_components"] = _refined_components.size()
	out["component_refined_reaches"] = component_reaches
	out["component_sparse_members"] = component_members
	out["multi_upstream_mouth_components"] = multi_mouth_components
	out["max_component_upstream_mouths"] = max_upstream_mouths
	out["component_union_transactional"] = true
	out["component_union_changes_ownership"] = false
	return out
