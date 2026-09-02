class_name PlanetHydrologyRiverClusterStore
extends PlanetHydrologyRiverCoupledStore
## Coarse 1D river store extended with ordered multi-tile fine refinement records.
##
## A cluster is still ONE coarse refinement hole / ownership parcel. The members
## array only describes how that parcel is spatially represented in sparse SWE.
## This preserves all inherited pending-inflow, routing and mass-ledger semantics.


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


func stats() -> Dictionary:
	var out := super.stats()
	var clusters := 0
	var members := 0
	for value: Variant in _refined_records.values():
		var record := value as Dictionary
		var count := int(record.get("member_count", 1))
		if count > 1:
			clusters += 1
		members += maxi(count, 1)
	out["multi_tile_river_clusters"] = clusters
	out["refined_sparse_members"] = members
	return out
