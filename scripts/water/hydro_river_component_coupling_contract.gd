class_name HydroRiverComponentCouplingContract
extends RefCounted
## Pure CPU validation for switching a logical refined river component from the
## legacy per-reach 1D<->2D mouths to one physically continuous fine component.
##
## The component registry itself is metadata-only. Bypassing an internal coarse
## boundary is conservative only when:
##   1. the upstream coarse reach has no residual 1D length left;
##   2. no residual coarse channel parcel is waiting in that reach; and
##   3. the upstream cluster's final sparse member is cardinally adjacent to the
##      receiver cluster's first sparse member.
##
## If any condition is false, callers must keep the existing per-reach coupling.

const LENGTH_REL_TOL := 1.0e-6
const LENGTH_ABS_TOL_M := 1.0e-4
const VOLUME_REL_TOL := 1.0e-9
const VOLUME_ABS_TOL_M3 := 1.0e-7


static func evaluate(store: PlanetHydrologyRiverClusterStore,
		component_id: int) -> Dictionary:
	if store == null or not store.initialized or component_id < 0:
		return _error(ERR_INVALID_PARAMETER, "invalid_component_contract_request")
	var component := store.refined_component(component_id)
	if component.is_empty():
		return _error(ERR_DOES_NOT_EXIST, "component_not_found")

	var cells := component.get("cells", PackedInt32Array()) as PackedInt32Array
	if cells.size() < 2:
		return _error(ERR_INVALID_DATA, "component_requires_multiple_reaches")
	for raw_cell in cells:
		var cell := int(raw_cell)
		if not store.is_refined_reach(cell):
			var missing := _error(ERR_INVALID_DATA, "component_member_not_refined")
			missing["cell"] = cell
			return missing
		if store.refined_cluster_members(cell).is_empty():
			var absent := _error(ERR_INVALID_DATA, "component_member_cluster_missing")
			absent["cell"] = cell
			return absent

	var physical_links: Array[Dictionary] = []
	var edges_value: Variant = component.get("internal_reach_edges", [])
	if not (edges_value is Array):
		return _error(ERR_INVALID_DATA, "component_internal_edges_missing")
	for edge_value: Variant in edges_value:
		if not (edge_value is Dictionary):
			return _error(ERR_INVALID_DATA, "component_internal_edge_invalid")
		var edge := edge_value as Dictionary
		var from_cell := int(edge.get("from_cell", -1))
		var to_cell := int(edge.get("to_cell", -1))
		if from_cell < 0 or to_cell < 0 or not store.is_refined_reach(from_cell) \
				or not store.is_refined_reach(to_cell):
			var bad_edge := _error(ERR_INVALID_DATA, "component_internal_edge_unresolved")
			bad_edge["from_cell"] = from_cell
			bad_edge["to_cell"] = to_cell
			return bad_edge

		var from_record := store.refined_reach_record(from_cell)
		var reach_length := maxf(float(from_record.get("reach_length_m", 0.0)), 0.0)
		var residual_length := maxf(float(from_record.get("residual_length_m", 0.0)), 0.0)
		var length_tolerance := maxf(LENGTH_ABS_TOL_M, reach_length * LENGTH_REL_TOL)
		if residual_length > length_tolerance:
			var residual := _error(ERR_BUSY, "component_internal_reach_has_residual_1d")
			residual["from_cell"] = from_cell
			residual["to_cell"] = to_cell
			residual["residual_length_m"] = residual_length
			residual["length_tolerance_m"] = length_tolerance
			return residual

		var represented := maxf(float(from_record.get("represented_volume_m3", 0.0)), 0.0)
		var residual_volume := maxf(store.channel_storage_m3[from_cell], 0.0)
		var volume_tolerance := maxf(VOLUME_ABS_TOL_M3, represented * VOLUME_REL_TOL)
		if residual_volume > volume_tolerance:
			var queued := _error(ERR_BUSY, "component_internal_reach_has_residual_water")
			queued["from_cell"] = from_cell
			queued["to_cell"] = to_cell
			queued["residual_channel_storage_m3"] = residual_volume
			queued["volume_tolerance_m3"] = volume_tolerance
			return queued

		var from_members := store.refined_cluster_members(from_cell)
		var to_members := store.refined_cluster_members(to_cell)
		var from_last := from_members[from_members.size() - 1]
		var to_first := to_members[0]
		var source_key := HydroTileKey.unpack(int(from_last.get("tile_id", -1)))
		var destination_key := HydroTileKey.unpack(int(to_first.get("tile_id", -1)))
		var link := _cardinal_link(source_key, destination_key)
		if link.is_empty():
			var gap := _error(ERR_CANT_RESOLVE, "component_internal_fine_gap")
			gap["from_cell"] = from_cell
			gap["to_cell"] = to_cell
			gap["from_tile_id"] = int(from_last.get("tile_id", -1))
			gap["to_tile_id"] = int(to_first.get("tile_id", -1))
			return gap
		physical_links.append({
			"from_cell": from_cell,
			"to_cell": to_cell,
			"from_tile_id": int(from_last.get("tile_id", -1)),
			"to_tile_id": int(to_first.get("tile_id", -1)),
			"source_direction": int(link.get("source_direction", -1)),
			"destination_direction": int(link.get("destination_direction", -1)),
			"edge_orientation": int(link.get("edge_orientation", 1)),
			"crossed_face": bool(link.get("crossed_face", false)),
		})

	var out := component.duplicate(true)
	out["error"] = OK
	out["ready"] = true
	out["physical_internal_links"] = physical_links
	out["physical_internal_link_count"] = physical_links.size()
	out["internal_coarse_mouths_bypassed"] = physical_links.size()
	out["ownership_changed"] = false
	out["requires_full_internal_reaches"] = true
	out["requires_cardinal_fine_links"] = true
	return out


static func _cardinal_link(source: HydroTileKey, destination: HydroTileKey) -> Dictionary:
	if source == null or destination == null:
		return {}
	for direction in 4:
		var link := HydroTileTopology.neighbor(source, direction)
		if link.is_empty():
			continue
		var key := link.get("key") as HydroTileKey
		if key != null and key.equals(destination):
			return link
	return {}


static func _error(error: Error, reason: String) -> Dictionary:
	return {
		"error": error,
		"ready": false,
		"reason": reason,
		"ownership_changed": false,
	}
