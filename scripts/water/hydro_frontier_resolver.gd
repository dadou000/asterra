class_name HydroFrontierResolver
extends RefCounted
## Compact GPU frontier queue -> stable world-topology bridge.
##
## GPU snapshots source slot + (face, level, x, y) + edge direction + outgoing Q.
## The resolver verifies that the transient slot still belongs to that exact stable
## tile, resolves Asterra's cube-sphere neighbor and asks an explicit
## terrain/structure reachability callback before policy can allocate anything.
##
## Reachability callable contract:
##   bool(source_key, source_direction, destination_key, flux_m3s, topology_link)
##
## boundary_ownership_provider is an optional representation hook. It is called as:
##   bool(source_key, source_direction, topology_link)
## When true, another conservative representation already owns that physical edge
## (for example a 2:1 HydroLOD interface), so frontier allocation is suppressed.
##
## defer_activation=true reserves a destination slot in ALLOCATING state. That is
## the production frontier-handoff path: initialize/seed state first, then call
## scheduler.activate_reserved() only after the GPU handoff has been recorded.

var scheduler: SparseHydroScheduler
var boundary_ownership_provider := Callable()


func _init(p_scheduler: SparseHydroScheduler) -> void:
	scheduler = p_scheduler


func set_boundary_ownership_provider(provider: Callable) -> void:
	boundary_ownership_provider = provider


func resolve_candidates(candidates: Array[Dictionary],
		reachability: Callable = Callable(), defer_activation: bool = false) -> Array[Dictionary]:
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
			"reserved": false,
			"needs_handoff": false,
			"reachable": false,
			"source_tile_id": -1,
			"destination_tile_id": -1,
			"destination_slot": -1,
			"reason": "",
		}
		if candidate.has("source_surface_m"):
			result["source_surface_m"] = float(candidate["source_surface_m"])
		if candidate.has("predictive_wetting"):
			result["predictive_wetting"] = bool(candidate["predictive_wetting"])

		if source_slot < 0 or source_slot >= scheduler.pool.capacity:
			result["reason"] = "invalid_source_slot"
			resolved.append(result)
			continue
		var source_id := scheduler.pool.id_for_slot(source_slot)
		if source_id < 0:
			result["reason"] = "stale_source_slot"
			resolved.append(result)
			continue

		if _candidate_has_identity(candidate):
			var snapshot := HydroTileKey.new(
				int(candidate["face"]), int(candidate["level"]),
				int(candidate["x"]), int(candidate["y"]))
			result["snapshot_tile_id"] = snapshot.packed()
			if snapshot.packed() != source_id:
				result["source_tile_id"] = source_id
				result["reason"] = "stale_source_identity"
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

		# Representation ownership is checked before reachability or allocation. A
		# claimed edge is already physically connected by another conservative path.
		if boundary_ownership_provider.is_valid():
			var claimed: Variant = boundary_ownership_provider.call(source, direction, link)
			if bool(claimed):
				result["reachable"] = true
				result["reason"] = "represented_boundary"
				resolved.append(result)
				continue

		var policy_link := link.duplicate(true)
		if candidate.has("source_surface_m"):
			policy_link["source_surface_m"] = float(candidate["source_surface_m"])
		if candidate.has("predictive_wetting"):
			policy_link["predictive_wetting"] = bool(candidate["predictive_wetting"])

		var can_enter := false
		if reachability.is_valid():
			var answer: Variant = reachability.call(
				source, direction, destination, flux_m3s, policy_link)
			can_enter = bool(answer)
		result["reachable"] = can_enter
		if not can_enter:
			result["reason"] = "blocked_boundary"
			resolved.append(result)
			continue

		var destination_slot: int
		if defer_activation:
			destination_slot = scheduler.reserve_resolved_boundary_flux(
				source, destination, flux_m3s, true)
		else:
			destination_slot = scheduler.report_resolved_boundary_flux(
				source, destination, flux_m3s, true)
		result["destination_slot"] = destination_slot
		result["accepted"] = destination_slot >= 0
		if destination_slot < 0:
			result["reason"] = "allocation_or_policy_rejected"
		elif defer_activation:
			var record := scheduler.pool.record(destination)
			var is_allocating := int(record.get("state", HydroTilePool.TileState.ACTIVE)) \
				== HydroTilePool.TileState.ALLOCATING
			result["reserved"] = is_allocating
			result["needs_handoff"] = is_allocating
			result["reason"] = "reserved" if is_allocating else "already_resident"
		else:
			result["reason"] = "woken"
		resolved.append(result)

	return resolved


func _candidate_has_identity(candidate: Dictionary) -> bool:
	return candidate.has("face") and candidate.has("level") \
		and candidate.has("x") and candidate.has("y")
