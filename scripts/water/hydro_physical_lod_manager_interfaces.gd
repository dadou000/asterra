class_name HydroPhysicalLODManagerInterfaces
extends HydroPhysicalLODManager
## Phase-4 manager extension that treats cross-LOD interface publication as part of
## the same atomic hierarchy transaction as connectivity publication.
##
## A transition is eligible only when the resulting external boundary remains 2:1
## balanced. After parent/children ownership swaps, both same-level connectivity and
## the mixed-resolution interface registry must accept the new topology. Any failure
## enters the existing conservative rollback paths before runtime resumes.

var interface_sync := Callable()


func set_interface_sync(provider: Callable) -> Error:
	if busy():
		return ERR_BUSY
	if not provider.is_valid():
		interface_sync = Callable()
		cross_lod_interfaces_enabled = false
		return OK
	var result: Variant = provider.call()
	var error := int(result) if result is int else OK
	if error != OK:
		return error
	interface_sync = provider
	cross_lod_interfaces_enabled = true
	return OK


func coarsen_eligibility(parent: HydroTileKey) -> Dictionary:
	var result := super.coarsen_eligibility(parent)
	if int(result.get("error", FAILED)) != OK:
		return result
	if not _result_boundary_balanced(parent, false):
		return _reject(ERR_BUSY, "hydrolod_two_to_one_balance_required")
	return result


func refine_eligibility(parent: HydroTileKey) -> Dictionary:
	var result := super.refine_eligibility(parent)
	if int(result.get("error", FAILED)) != OK:
		return result
	if not _result_boundary_balanced(parent, true):
		return _reject(ERR_BUSY, "hydrolod_two_to_one_balance_required")
	return result


func _commit_coarsen() -> void:
	var released := HydroSchedulerBatchOps.force_release(scheduler, _children,
		"hydrolod_coarsen_children")
	if released.size() != _children.size():
		_fail_precommit(ERR_CANT_ACQUIRE_RESOURCE, "coarsen_release_children")
		return
	var activated := scheduler.activate_reserved(_parent, "hydrolod_coarsen_publish")
	if activated != _parent_slot:
		_rollback_to_children(ERR_CANT_ACQUIRE_RESOURCE, "coarsen_publish_parent")
		return
	var err := _sync_complete_topology()
	if err != OK:
		_rollback_to_children(err, "coarsen_topology_publish")
		return
	_finish_success({
		"mode": "coarsen",
		"parent_tile_id": _parent.packed(),
		"parent_slot": _parent_slot,
		"child_tile_ids": HydroLODHierarchy.child_ids(_parent),
		"released_child_slots": released,
		"physical_lod": atlas.physical_lod_for_level(_parent.level),
		"conservative_transfer": true,
		"cross_lod_interfaces_published": true,
	})


func _commit_refine() -> void:
	var activated := HydroSchedulerBatchOps.activate_reserved(scheduler, _children,
		"hydrolod_refine_publish")
	if activated.size() != _children.size():
		_cancel_allocating_children("hydrolod_refine_publish_failed")
		_fail_request(ERR_CANT_ACQUIRE_RESOURCE,
			"refine_publish_children", "parent_preserved")
		return
	var parent_released := HydroSchedulerBatchOps.force_release(scheduler, [_parent],
		"hydrolod_refine_parent")
	if parent_released.size() != 1:
		HydroSchedulerBatchOps.force_release(scheduler, _children,
			"hydrolod_refine_child_rollback")
		_sync_complete_topology()
		_fail_request(ERR_CANT_ACQUIRE_RESOURCE,
			"refine_release_parent", "parent_preserved")
		return
	var err := _sync_complete_topology()
	if err != OK:
		_rollback_to_parent(err, "refine_topology_publish")
		return
	_finish_success({
		"mode": "refine",
		"parent_tile_id": _parent.packed(),
		"released_parent_slot": parent_released[0],
		"child_tile_ids": HydroLODHierarchy.child_ids(_parent),
		"child_slots": activated,
		"physical_lod": atlas.physical_lod_for_level(_parent.level + 1),
		"conservative_transfer": true,
		"cross_lod_interfaces_published": true,
	})


func _rollback_to_children(original_error: Error, stage: String) -> void:
	var parent_id := PackedInt64Array([_parent.packed()])
	var restored := true
	for i in range(_children.size() - 1, -1, -1):
		var expected := int(_child_records[i].get("slot", -1))
		var lod := int(_child_records[i].get("physical_lod", 0))
		var slot := scheduler.reserve_for_lod_transition(_children[i], lod,
			parent_id, "hydrolod_coarsen_restore_child")
		if slot != expected:
			restored = false
			break
	if restored:
		var activated := HydroSchedulerBatchOps.activate_reserved(scheduler, _children,
			"hydrolod_coarsen_restore_children")
		restored = activated.size() == _children.size()
	if restored and scheduler.pool.contains(_parent):
		restored = scheduler.force_release(_parent,
			"hydrolod_coarsen_restore_parent")
	if restored:
		restored = _sync_complete_topology() == OK
	if restored:
		_fail_request(original_error, stage, "fine_children_restored")
		return

	_cancel_allocating_children("hydrolod_coarsen_restore_cancel")
	if scheduler.pool.contains(_parent):
		var rec := scheduler.pool.record(_parent)
		if int(rec.get("state", HydroTilePool.TileState.ALLOCATING)) \
				== HydroTilePool.TileState.ALLOCATING:
			scheduler.activate_reserved(_parent, "hydrolod_coarsen_fallback_parent")
	_sync_complete_topology()
	_fail_request(original_error, stage, "coarse_parent_preserved")


func _rollback_to_parent(original_error: Error, stage: String) -> void:
	var child_ids := HydroLODHierarchy.child_ids(_parent)
	var lod := int(_parent_record.get("physical_lod",
		atlas.physical_lod_for_level(_parent.level)))
	var restored_slot := scheduler.reserve_for_lod_transition(_parent, lod,
		child_ids, "hydrolod_refine_restore_parent")
	var expected := int(_parent_record.get("slot", -1))
	var restored := restored_slot == expected
	if restored:
		restored = scheduler.activate_reserved(_parent,
			"hydrolod_refine_restore_parent") == expected
	if restored:
		var released := HydroSchedulerBatchOps.force_release(scheduler, _children,
			"hydrolod_refine_restore_children")
		restored = released.size() == _children.size()
	if restored:
		restored = _sync_complete_topology() == OK
	if restored:
		_fail_request(original_error, stage, "coarse_parent_restored")
		return

	if scheduler.pool.contains(_parent):
		var rec := scheduler.pool.record(_parent)
		if int(rec.get("state", HydroTilePool.TileState.ALLOCATING)) \
				== HydroTilePool.TileState.ALLOCATING:
			scheduler.cancel_reserved(_parent, "hydrolod_refine_restore_cancel")
	_sync_complete_topology()
	_fail_request(original_error, stage, "fine_children_preserved")


func _sync_complete_topology() -> Error:
	var err := connectivity.sync_pool(scheduler.pool)
	if err != OK:
		return err
	if not interface_sync.is_valid():
		return ERR_UNCONFIGURED
	var result: Variant = interface_sync.call()
	return int(result) if result is int else OK


## Evaluate only the parent footprint's external boundary. For a coarsen result the
## parent is live at level L, so neighboring ownership may be L-1..L+1. For a refine
## result the children are live at L+1, allowing neighbors L..L+2. Anything farther
## away in level would create an unsupported >2:1 face.
func _result_boundary_balanced(parent: HydroTileKey, refining: bool) -> bool:
	if parent == null:
		return false
	var min_level := parent.level if refining else parent.level - 1
	var max_level := parent.level + 2 if refining else parent.level + 1
	for direction in 4:
		var link := HydroTileTopology.neighbor(parent, direction)
		if link.is_empty():
			continue
		var region := link.get("key") as HydroTileKey
		var destination_direction := int(link.get("destination_direction", -1))
		if region == null or destination_direction < 0:
			return false

		var covering := HydroLODHierarchy.covering_record(scheduler.pool, region)
		if not covering.is_empty():
			var key := covering.get("key") as HydroTileKey
			if key != null and int(covering.get("state", HydroTilePool.TileState.ALLOCATING)) \
					!= HydroTilePool.TileState.ALLOCATING:
				if key.level < min_level or key.level > max_level:
					return false

		for record in HydroLODHierarchy.descendant_records(scheduler.pool, region):
			if int(record.get("state", HydroTilePool.TileState.ALLOCATING)) \
					== HydroTilePool.TileState.ALLOCATING:
				continue
			var key := record.get("key") as HydroTileKey
			if key == null or not _touches_region_edge(
					key, region, destination_direction):
				continue
			if key.level < min_level or key.level > max_level:
				return false
	return true


func _touches_region_edge(candidate: HydroTileKey, region: HydroTileKey,
		direction: int) -> bool:
	if candidate == null or region == null \
			or not HydroLODHierarchy.is_ancestor(region, candidate):
		return false
	var shift := candidate.level - region.level
	var side := 1 << shift
	var local_x := candidate.x - (region.x << shift)
	var local_y := candidate.y - (region.y << shift)
	match direction:
		HydroTileTopology.DIR_WEST: return local_x == 0
		HydroTileTopology.DIR_EAST: return local_x == side - 1
		HydroTileTopology.DIR_SOUTH: return local_y == 0
		HydroTileTopology.DIR_NORTH: return local_y == side - 1
	return false


func stats() -> Dictionary:
	var out := super.stats()
	out["interface_sync_bound"] = interface_sync.is_valid()
	out["two_to_one_balance_enforced"] = true
	out["interface_publication_transactional"] = true
	return out
