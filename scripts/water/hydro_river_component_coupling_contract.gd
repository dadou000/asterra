class_name HydroRiverComponentCouplingContract
extends RefCounted
## Pure CPU validation for switching a logical refined river component from the
## legacy per-reach 1D<->2D mouths to one physically continuous fine component.
##
## Bypassing an internal coarse boundary is conservative only when:
##   1. the upstream coarse reach has no residual 1D length left;
##   2. no residual coarse channel parcel is waiting in that reach;
##   3. the upstream cluster's final sparse member is cardinally adjacent to the
##      receiver cluster's first sparse member; and
##   4. their actual seeded corridor centerlines cross that shared edge continuously.
##
## A true multi-root confluence additionally requires branched junction geometry in
## the receiver tile. The current straight-corridor seeder cannot prove that shape,
## so multi-root components remain on legacy per-reach coupling until a dedicated
## junction builder publishes `fine_junction_verified` metadata.

const LENGTH_REL_TOL := 1.0e-6
const LENGTH_ABS_TOL_M := 1.0e-4
const VOLUME_REL_TOL := 1.0e-9
const VOLUME_ABS_TOL_M3 := 1.0e-7
const EDGE_EPS := 1.0e-8


static func evaluate(store: PlanetHydrologyRiverClusterStore,
		component_id: int, tile_resolution: int = -1,
		cell_size_m: float = -1.0) -> Dictionary:
	if store == null or not store.initialized or component_id < 0:
		return _error(ERR_INVALID_PARAMETER, "invalid_component_contract_request")
	var component := store.refined_component(component_id)
	if component.is_empty():
		return _error(ERR_DOES_NOT_EXIST, "component_not_found")

	var cells := component.get("cells", PackedInt32Array()) as PackedInt32Array
	if cells.size() < 2:
		return _error(ERR_INVALID_DATA, "component_requires_multiple_reaches")
	var upstream_mouth_count := int(component.get("upstream_mouth_count", 0))
	if upstream_mouth_count > 1 and not bool(component.get("fine_junction_verified", false)):
		var junction := _error(ERR_BUSY, "component_fine_junction_not_verified")
		junction["upstream_mouth_count"] = upstream_mouth_count
		junction["requires_branched_junction_seeding"] = true
		return junction

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

		var continuity := {
			"error": OK,
			"source_parameter": -1.0,
			"destination_parameter": -1.0,
			"parameter_delta_cells": 0.0,
		}
		if tile_resolution > 0:
			continuity = _corridor_continuity(from_last, to_first, link,
				tile_resolution, cell_size_m)
			if int(continuity.get("error", FAILED)) != OK:
				continuity["from_cell"] = from_cell
				continuity["to_cell"] = to_cell
				return continuity

		physical_links.append({
			"from_cell": from_cell,
			"to_cell": to_cell,
			"from_tile_id": int(from_last.get("tile_id", -1)),
			"to_tile_id": int(to_first.get("tile_id", -1)),
			"source_direction": int(link.get("source_direction", -1)),
			"destination_direction": int(link.get("destination_direction", -1)),
			"edge_orientation": int(link.get("edge_orientation", 1)),
			"crossed_face": bool(link.get("crossed_face", false)),
			"source_edge_parameter": float(continuity.get("source_parameter", -1.0)),
			"destination_edge_parameter": float(continuity.get("destination_parameter", -1.0)),
			"corridor_parameter_delta_cells": float(continuity.get("parameter_delta_cells", 0.0)),
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
	out["corridor_continuity_verified"] = tile_resolution > 0
	return out


static func _corridor_continuity(from_member: Dictionary, to_member: Dictionary,
		link: Dictionary, resolution: int, cell_size_m: float) -> Dictionary:
	var source_edge := int(link.get("source_direction", -1))
	var destination_edge := int(link.get("destination_direction", -1))
	var source_hit := _line_edge_hit(from_member, source_edge, resolution)
	var destination_hit := _line_edge_hit(to_member, destination_edge, resolution)
	if int(source_hit.get("error", FAILED)) != OK \
			or int(destination_hit.get("error", FAILED)) != OK:
		return _error(ERR_CANT_RESOLVE, "component_internal_corridor_edge_miss")

	# The upstream centerline must travel forward to the shared edge. The receiver
	# centerline must reach that same entry edge when traced backward.
	if float(source_hit.get("t", -INF)) < -EDGE_EPS \
			or float(destination_hit.get("t", INF)) > EDGE_EPS:
		return _error(ERR_CANT_RESOLVE, "component_internal_corridor_wrong_orientation")

	var source_parameter := float(source_hit.get("parameter", -1.0))
	var destination_parameter := float(destination_hit.get("parameter", -1.0))
	var expected_destination := source_parameter if int(link.get("edge_orientation", 1)) >= 0 \
		else 1.0 - source_parameter
	var delta_cells := absf(destination_parameter - expected_destination) * float(resolution)
	var tolerance_cells := 1.5
	if is_finite(cell_size_m) and cell_size_m > 0.0:
		var from_half := maxf(float(from_member.get("half_width_m", 0.0)), 0.0)
		var to_half := maxf(float(to_member.get("half_width_m", 0.0)), 0.0)
		tolerance_cells = maxf(1.0, (from_half + to_half) / cell_size_m + 0.5)
	if delta_cells > tolerance_cells:
		var gap := _error(ERR_CANT_RESOLVE, "component_internal_corridor_gap")
		gap["source_parameter"] = source_parameter
		gap["destination_parameter"] = destination_parameter
		gap["expected_destination_parameter"] = expected_destination
		gap["parameter_delta_cells"] = delta_cells
		gap["tolerance_cells"] = tolerance_cells
		return gap
	return {
		"error": OK,
		"source_parameter": source_parameter,
		"destination_parameter": destination_parameter,
		"parameter_delta_cells": delta_cells,
		"tolerance_cells": tolerance_cells,
	}


static func _line_edge_hit(member: Dictionary, edge: int, resolution: int) -> Dictionary:
	var center_value: Variant = member.get("center_cell", null)
	var direction_value: Variant = member.get("direction_cell", null)
	if not (center_value is Vector2) or not (direction_value is Vector2) or resolution <= 0:
		return {"error": ERR_INVALID_DATA}
	var center := center_value as Vector2
	var direction := direction_value as Vector2
	if direction.length_squared() <= 1.0e-12:
		return {"error": ERR_INVALID_DATA}
	direction = direction.normalized()
	var r := float(resolution)
	var t := INF
	match edge:
		HydroTileTopology.DIR_WEST:
			if absf(direction.x) > EDGE_EPS:
				t = (0.0 - center.x) / direction.x
		HydroTileTopology.DIR_EAST:
			if absf(direction.x) > EDGE_EPS:
				t = (r - center.x) / direction.x
		HydroTileTopology.DIR_SOUTH:
			if absf(direction.y) > EDGE_EPS:
				t = (0.0 - center.y) / direction.y
		HydroTileTopology.DIR_NORTH:
			if absf(direction.y) > EDGE_EPS:
				t = (r - center.y) / direction.y
	if not is_finite(t):
		return {"error": ERR_CANT_RESOLVE}
	var point := center + direction * t
	var parameter := 0.0
	if edge in [HydroTileTopology.DIR_WEST, HydroTileTopology.DIR_EAST]:
		if point.y < -EDGE_EPS or point.y > r + EDGE_EPS:
			return {"error": ERR_CANT_RESOLVE}
		parameter = clampf(point.y / r, 0.0, 1.0)
	else:
		if point.x < -EDGE_EPS or point.x > r + EDGE_EPS:
			return {"error": ERR_CANT_RESOLVE}
		parameter = clampf(point.x / r, 0.0, 1.0)
	return {"error": OK, "t": t, "parameter": parameter}


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
