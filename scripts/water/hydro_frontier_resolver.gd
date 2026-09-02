class_name HydroFrontierResolver
extends RefCounted
## Transitional compact-queue -> world-topology bridge for Phase 3.
##
## GPU emits only source slot, edge direction and outgoing Q. This resolver maps
## the transient source slot back to its stable HydroTileKey, resolves the exact
## cube-sphere neighbor, asks an explicit terrain/structure reachability callback,
## then delegates wake policy to SparseHydroScheduler.
##
## Reachability callable contract:
##   bool(source_key, source_direction, destination_key, flux_m3s, topology_link)
##
## An absent/invalid callback FAILS CLOSED. Topological adjacency is not evidence
## that a cliff, levee, building or dry high boundary can actually be inundated.

var scheduler: SparseHydroScheduler


func _init(p_scheduler: SparseHydroScheduler) -> void:
	scheduler = p_scheduler


func resolve_candidates(candidates: Array[Dictionary],
		reachability: Callable = Callable()) -> Array[Dictionary]:
	var resolved: Array[Dictionary] = []
	if scheduler == null or scheduler.pool == null:
		return resolved

	for candidate in candidates:
		var source_slot := int(candidate.get("slot", -1))
		var direction := int(candidate.get("direction", -1))
		var flux_m3s := float(candidate.get("flux_m3s", 0.0))
		var result := {
			"source_slot": source_slot,
			"direction": direction,
			"flux_m3s": flux_m3s,
			"accepted": false,
			"reachable": false,
			"source_tile_id": -1,
			"destination_tile_id": -1,
			"destination_slot": -1,
			"reason": "",
		}

		if source_slot < 0 or source_slot >= scheduler.pool.capacity:
			result["reason"] = "invalid_source_slot"
			resolved.append(result)
			continue
		var source_id := scheduler.pool.id_for_slot(source_slot)
		if source_id < 0:
			result["reason"] = "stale_source_slot"
			resolved.append(result)
			continue
		if direction < HydroTileTopology.DIR_WEST or direction > HydroTileTopology.DIR_NORTH:
			result["reason"] = "invalid_direction"
			resolved.append(result)
			continue
		if not is_finite(flux_m3s) or flux_m3s <= scheduler.wake_flux_threshold_m3s:
			result["reason"] = "subthreshold_flux"
			resolved.append(result)
			continue

		var source := HydroTileKey.unpack(source_id)
		var link := HydroTileTopology.neighbor(source, direction)
		result["source_tile_id"] = source_id
		if link.is_empty():
			result["reason"] = "topology_unresolved"
			resolved.append(result)
			continue
		var destination := link["key"] as HydroTileKey
		result["destination_tile_id"] = destination.packed()
		result["destination_direction"] = int(link["destination_direction"])
		result["edge_orientation"] = int(link["edge_orientation"])
		result["crossed_face"] = bool(link["crossed_face"])

		var can_enter := false
		if reachability.is_valid():
			var answer: Variant = reachability.call(
				source, direction, destination, flux_m3s, link)
			can_enter = bool(answer)
		result["reachable"] = can_enter
		if not can_enter:
			result["reason"] = "blocked_boundary"
			resolved.append(result)
			continue

		var destination_slot := scheduler.report_resolved_boundary_flux(
			source, destination, flux_m3s, true)
		result["destination_slot"] = destination_slot
		result["accepted"] = destination_slot >= 0
		result["reason"] = "woken" if destination_slot >= 0 else "allocation_or_policy_rejected"
		resolved.append(result)

	return resolved
