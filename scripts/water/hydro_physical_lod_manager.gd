class_name HydroPhysicalLODManager
extends Node
## Atomic physical 2:1 HydroLOD ownership transitions.
##
## Coarsen:
##   four published children -> reserve hidden parent -> conservative restriction
##   -> release children -> publish parent -> rebuild connectivity.
##
## Refine:
##   published parent -> reserve/stage four hidden children -> conservative
##   prolongation -> publish children -> release parent -> rebuild connectivity.
##
## Runtime must be paused by the caller for the whole request. Until the dedicated
## cross-LOD flux/reflux layer is available, transitions that would expose a live
## 2:1 boundary are rejected rather than creating a reflective numerical wall.

signal initialized
signal initialization_failed(error: Error)
signal transition_completed(request_id: int, report: Dictionary)
signal transition_failed(request_id: int, error: Error, stage: String,
	recovery: String)
signal released

var maximum_physical_lod := 4
var cross_lod_interfaces_enabled := false

var scheduler: SparseHydroScheduler
var atlas: SparseHydroAtlasGPU
var connectivity: SparseHydroConnectivityGPU
var terrain_bed: HydroTerrainBedGPU
var transfer: HydroLODTransferGPU
var transition_guard := Callable()

var _initialized := false
var _busy := false
var _release_requested := false
var _next_request_id := 1
var _request_id := -1
var _transfer_request_id := -1
var _mode := ""
var _parent: HydroTileKey
var _children: Array[HydroTileKey] = []
var _parent_slot := -1
var _child_slots := PackedInt32Array()
var _parent_record: Dictionary = {}
var _child_records: Array[Dictionary] = []
var _stage_requests: Dictionary = {} # terrain request -> child index
var _staged_children := 0


func initialize(p_scheduler: SparseHydroScheduler, p_atlas: SparseHydroAtlasGPU,
		p_connectivity: SparseHydroConnectivityGPU,
		p_terrain_bed: HydroTerrainBedGPU, atmospheric_source_rid: RID) -> Error:
	if _initialized or transfer != null:
		return ERR_BUSY
	if p_scheduler == null or p_scheduler.pool == null or p_atlas == null \
			or not p_atlas.initialized_ok() or not p_atlas.hydrolod_enabled() \
			or p_connectivity == null or not p_connectivity.initialized_ok() \
			or p_terrain_bed == null or not p_terrain_bed.initialized_ok() \
			or not atmospheric_source_rid.is_valid():
		return ERR_UNCONFIGURED
	scheduler = p_scheduler
	atlas = p_atlas
	connectivity = p_connectivity
	terrain_bed = p_terrain_bed
	terrain_bed.stage_recorded.connect(_on_terrain_stage_recorded)
	terrain_bed.stage_failed.connect(_on_terrain_stage_failed)
	transfer = HydroLODTransferGPU.new()
	transfer.name = "HydroLODTransferGPU"
	add_child(transfer)
	transfer.initialized.connect(func():
		_initialized = true
		initialized.emit())
	transfer.initialization_failed.connect(func(error: Error):
		initialization_failed.emit(error))
	transfer.transfer_recorded.connect(_on_transfer_recorded)
	transfer.transfer_failed.connect(_on_transfer_failed)
	var err := transfer.initialize(atlas, atmospheric_source_rid)
	if err != OK:
		_cleanup_transfer()
		_clear_refs()
	return err


func initialized_ok() -> bool:
	return _initialized


func busy() -> bool:
	return _busy


func set_transition_guard(guard: Callable) -> Error:
	if _busy:
		return ERR_BUSY
	transition_guard = guard
	return OK


func coarsen_eligibility(parent: HydroTileKey) -> Dictionary:
	if not _initialized or _busy:
		return _reject(ERR_BUSY, "lod_manager_busy")
	if parent == null or not HydroLODHierarchy.valid_physical_key(
			atlas.base_tile_level, maximum_physical_lod, parent):
		return _reject(ERR_INVALID_PARAMETER, "invalid_parent_lod")
	var parent_lod := atlas.physical_lod_for_level(parent.level)
	if parent_lod <= 0:
		return _reject(ERR_INVALID_PARAMETER, "parent_not_coarser_than_h0")
	if scheduler.pool.contains(parent):
		return _reject(ERR_ALREADY_EXISTS, "parent_already_resident")
	var resident := HydroLODHierarchy.immediate_children_resident(
		scheduler.pool, parent, true)
	if not bool(resident.get("ready", false)):
		return _reject(ERR_CANT_ACQUIRE_RESOURCE,
			String(resident.get("reason", "children_not_ready")))
	var children := resident.get("children", []) as Array[HydroTileKey]
	var ignored := HydroLODHierarchy.child_ids(parent)
	var conflict := HydroLODHierarchy.representation_conflict(
		scheduler.pool, parent, ignored)
	if bool(conflict.get("conflict", true)):
		return _reject(ERR_ALREADY_EXISTS,
			String(conflict.get("reason", "hierarchy_conflict")))
	if scheduler.pool.free_count() < 1:
		return _reject(ERR_CANT_ACQUIRE_RESOURCE, "no_parent_slot")
	if not _guard_allows("coarsen", parent, children):
		return _reject(ERR_UNAUTHORIZED, "transition_guard_rejected")
	if not cross_lod_interfaces_enabled and not _coarsen_boundary_same_level(parent):
		return _reject(ERR_BUSY, "cross_lod_interface_required")
	return {
		"error": OK,
		"eligible": true,
		"parent": parent,
		"children": children,
		"child_slots": resident.get("child_slots", PackedInt32Array()),
		"physical_lod": parent_lod,
	}


func refine_eligibility(parent: HydroTileKey) -> Dictionary:
	if not _initialized or _busy:
		return _reject(ERR_BUSY, "lod_manager_busy")
	if parent == null or not scheduler.pool.contains(parent):
		return _reject(ERR_DOES_NOT_EXIST, "parent_not_resident")
	var parent_lod := atlas.physical_lod_for_level(parent.level)
	if parent_lod <= 0 or parent.level >= atlas.base_tile_level:
		return _reject(ERR_INVALID_PARAMETER, "parent_already_h0")
	var parent_record := scheduler.pool.record(parent)
	if int(parent_record.get("state", HydroTilePool.TileState.ALLOCATING)) \
			== HydroTilePool.TileState.ALLOCATING:
		return _reject(ERR_BUSY, "parent_not_published")
	var children := HydroLODHierarchy.children(parent)
	if children.size() != 4 or scheduler.pool.free_count() < 4:
		return _reject(ERR_CANT_ACQUIRE_RESOURCE, "child_capacity_insufficient")
	var ignore := PackedInt64Array([parent.packed()])
	for child in children:
		if scheduler.pool.contains(child):
			return _reject(ERR_ALREADY_EXISTS, "child_already_resident")
		var conflict := HydroLODHierarchy.representation_conflict(
			scheduler.pool, child, ignore)
		if bool(conflict.get("conflict", true)):
			return _reject(ERR_ALREADY_EXISTS,
				String(conflict.get("reason", "hierarchy_conflict")))
	if not _guard_allows("refine", parent, children):
		return _reject(ERR_UNAUTHORIZED, "transition_guard_rejected")
	if not cross_lod_interfaces_enabled and not _refine_boundary_same_level(parent):
		return _reject(ERR_BUSY, "cross_lod_interface_required")
	return {
		"error": OK,
		"eligible": true,
		"parent": parent,
		"children": children,
		"physical_lod": parent_lod - 1,
	}


func coarsen(parent: HydroTileKey) -> int:
	var eligibility := coarsen_eligibility(parent)
	if int(eligibility.get("error", FAILED)) != OK:
		return -1
	var children := eligibility.get("children", []) as Array[HydroTileKey]
	var child_slots := eligibility.get("child_slots", PackedInt32Array()) as PackedInt32Array
	var ignored := HydroLODHierarchy.child_ids(parent)
	var parent_slot := scheduler.reserve_for_lod_transition(parent,
		atlas.physical_lod_for_level(parent.level), ignored, "hydrolod_coarsen_parent")
	if parent_slot < 0:
		return -1
	_begin_request("coarsen", parent, children, parent_slot, child_slots)
	for child in children:
		_child_records.append(scheduler.pool.record(child))
	_transfer_request_id = transfer.restrict(_parent_slot, _child_slots)
	if _transfer_request_id < 0:
		scheduler.cancel_reserved(parent, "hydrolod_restrict_submit_failed")
		_fail_precommit(ERR_BUSY, "restrict_submit")
		return -1
	return _request_id


func refine(parent: HydroTileKey) -> int:
	var eligibility := refine_eligibility(parent)
	if int(eligibility.get("error", FAILED)) != OK:
		return -1
	var children := eligibility.get("children", []) as Array[HydroTileKey]
	var parent_record := scheduler.pool.record(parent)
	var parent_slot := int(parent_record.get("slot", -1))
	var child_slots := PackedInt32Array()
	var replacing := PackedInt64Array([parent.packed()])
	var child_lod := atlas.physical_lod_for_level(parent.level + 1)
	for child in children:
		var slot := scheduler.reserve_for_lod_transition(child, child_lod,
			replacing, "hydrolod_refine_child")
		if slot < 0:
			for reserved_child in children:
				if scheduler.pool.contains(reserved_child):
					var rec := scheduler.pool.record(reserved_child)
					if int(rec.get("state", HydroTilePool.TileState.ACTIVE)) \
							== HydroTilePool.TileState.ALLOCATING:
						scheduler.cancel_reserved(reserved_child,
							"hydrolod_refine_reserve_rollback")
			return -1
		child_slots.append(slot)

	_begin_request("refine", parent, children, parent_slot, child_slots)
	_parent_record = parent_record
	for i in _children.size():
		var stage := terrain_bed.stage_reserved_tile(_children[i], _child_slots[i])
		if not bool(stage.get("queued", false)) or int(stage.get("error", FAILED)) != OK:
			_fail_precommit(int(stage.get("error", ERR_INVALID_DATA)),
				"child_terrain_stage_submit")
			return -1
		_stage_requests[int(stage.get("request_id", -1))] = i
	return _request_id


func _begin_request(mode: String, parent: HydroTileKey,
		children: Array[HydroTileKey], parent_slot: int,
		child_slots: PackedInt32Array) -> void:
	_busy = true
	_request_id = _next_request_id
	_next_request_id += 1
	_mode = mode
	_parent = parent
	_children = children.duplicate()
	_parent_slot = parent_slot
	_child_slots = child_slots.duplicate()
	_parent_record = {}
	_child_records = []
	_stage_requests.clear()
	_staged_children = 0
	_transfer_request_id = -1


func _on_terrain_stage_recorded(request_id: int, tile_id: int, slot: int) -> void:
	if not _busy or _mode != "refine" or not _stage_requests.has(request_id):
		return
	var child_index := int(_stage_requests[request_id])
	_stage_requests.erase(request_id)
	if child_index < 0 or child_index >= _children.size() \
			or _children[child_index].packed() != tile_id \
			or _child_slots[child_index] != slot:
		_fail_precommit(ERR_INVALID_DATA, "child_terrain_stage_identity")
		return
	_staged_children += 1
	if _staged_children == _children.size():
		_transfer_request_id = transfer.prolong(_parent_slot, _child_slots)
		if _transfer_request_id < 0:
			_fail_precommit(ERR_BUSY, "prolong_submit")


func _on_terrain_stage_failed(request_id: int, error: Error) -> void:
	if _busy and _mode == "refine" and _stage_requests.has(request_id):
		_fail_precommit(error, "child_terrain_stage")


func _on_transfer_recorded(request_id: int, mode: String, parent_slot: int,
		child_slots: PackedInt32Array) -> void:
	if not _busy or request_id != _transfer_request_id or mode != _mode \
			or parent_slot != _parent_slot or child_slots != _child_slots:
		return
	_transfer_request_id = -1
	if _mode == "coarsen":
		_commit_coarsen()
	else:
		_commit_refine()


func _on_transfer_failed(request_id: int, error: Error) -> void:
	if _busy and request_id == _transfer_request_id:
		_fail_precommit(error, _mode + "_transfer")


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
	var err := connectivity.sync_pool(scheduler.pool)
	if err != OK:
		_rollback_to_children(err, "coarsen_connectivity")
		return
	_finish_success({
		"mode": "coarsen",
		"parent_tile_id": _parent.packed(),
		"parent_slot": _parent_slot,
		"child_tile_ids": HydroLODHierarchy.child_ids(_parent),
		"released_child_slots": released,
		"physical_lod": atlas.physical_lod_for_level(_parent.level),
		"conservative_transfer": true,
	})


func _commit_refine() -> void:
	var activated := HydroSchedulerBatchOps.activate_reserved(scheduler, _children,
		"hydrolod_refine_publish")
	if activated.size() != _children.size():
		_cancel_allocating_children("hydrolod_refine_publish_failed")
		_fail_request(ERR_CANT_ACQUIRE_RESOURCE, "refine_publish_children", "parent_preserved")
		return
	var parent_released := HydroSchedulerBatchOps.force_release(scheduler, [_parent],
		"hydrolod_refine_parent")
	if parent_released.size() != 1:
		HydroSchedulerBatchOps.force_release(scheduler, _children,
			"hydrolod_refine_child_rollback")
		connectivity.sync_pool(scheduler.pool)
		_fail_request(ERR_CANT_ACQUIRE_RESOURCE, "refine_release_parent", "parent_preserved")
		return
	var err := connectivity.sync_pool(scheduler.pool)
	if err != OK:
		_rollback_to_parent(err, "refine_connectivity")
		return
	_finish_success({
		"mode": "refine",
		"parent_tile_id": _parent.packed(),
		"released_parent_slot": parent_released[0],
		"child_tile_ids": HydroLODHierarchy.child_ids(_parent),
		"child_slots": activated,
		"physical_lod": atlas.physical_lod_for_level(_parent.level + 1),
		"conservative_transfer": true,
	})


func _rollback_to_children(original_error: Error, stage: String) -> void:
	# Parent remains allocated/active, so the child slots released immediately before
	# it are still the top of the LIFO free stack. Reserve children in reverse order.
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
	if restored:
		if scheduler.pool.contains(_parent):
			restored = scheduler.force_release(_parent,
				"hydrolod_coarsen_restore_parent")
	if restored:
		restored = connectivity.sync_pool(scheduler.pool) == OK
	if restored:
		_fail_request(original_error, stage, "fine_children_restored")
		return

	# Restoration itself failed. Cancel only unpublished child reservations; the
	# conservative parent state is already a complete authoritative fallback.
	_cancel_allocating_children("hydrolod_coarsen_restore_cancel")
	if scheduler.pool.contains(_parent):
		var rec := scheduler.pool.record(_parent)
		if int(rec.get("state", HydroTilePool.TileState.ALLOCATING)) \
				== HydroTilePool.TileState.ALLOCATING:
			scheduler.activate_reserved(_parent, "hydrolod_coarsen_fallback_parent")
	connectivity.sync_pool(scheduler.pool)
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
		restored = connectivity.sync_pool(scheduler.pool) == OK
	if restored:
		_fail_request(original_error, stage, "coarse_parent_restored")
		return

	# The fine children were already fully prolongated and published. If parent
	# restoration fails, keep fine ownership rather than deleting the only safe copy.
	if scheduler.pool.contains(_parent):
		var rec := scheduler.pool.record(_parent)
		if int(rec.get("state", HydroTilePool.TileState.ALLOCATING)) \
				== HydroTilePool.TileState.ALLOCATING:
			scheduler.cancel_reserved(_parent, "hydrolod_refine_restore_cancel")
	connectivity.sync_pool(scheduler.pool)
	_fail_request(original_error, stage, "fine_children_preserved")


func _fail_precommit(error: Error, stage: String) -> void:
	if _mode == "coarsen":
		if _parent != null and scheduler.pool.contains(_parent):
			var rec := scheduler.pool.record(_parent)
			if int(rec.get("state", HydroTilePool.TileState.ACTIVE)) \
					== HydroTilePool.TileState.ALLOCATING:
				scheduler.cancel_reserved(_parent, "hydrolod_coarsen_precommit_cancel")
		_fail_request(error, stage, "fine_children_preserved")
	else:
		_cancel_allocating_children("hydrolod_refine_precommit_cancel")
		_fail_request(error, stage, "coarse_parent_preserved")


func _cancel_allocating_children(reason: String) -> void:
	for child in _children:
		if not scheduler.pool.contains(child):
			continue
		var rec := scheduler.pool.record(child)
		if int(rec.get("state", HydroTilePool.TileState.ACTIVE)) \
				== HydroTilePool.TileState.ALLOCATING:
			scheduler.cancel_reserved(child, reason)


func _finish_success(report: Dictionary) -> void:
	var completed := _request_id
	var out := report.duplicate(true)
	out["request_id"] = completed
	out["atomic_hierarchy_swap"] = true
	_clear_request()
	transition_completed.emit(completed, out)


func _fail_request(error: Error, stage: String, recovery: String) -> void:
	var failed := _request_id
	_clear_request()
	transition_failed.emit(failed, error, stage, recovery)


func _clear_request() -> void:
	_busy = false
	_request_id = -1
	_transfer_request_id = -1
	_mode = ""
	_parent = null
	_children = []
	_parent_slot = -1
	_child_slots = PackedInt32Array()
	_parent_record = {}
	_child_records = []
	_stage_requests.clear()
	_staged_children = 0
	if _release_requested:
		call_deferred(&"_finish_release")


func _guard_allows(mode: String, parent: HydroTileKey,
		children: Array[HydroTileKey]) -> bool:
	if not transition_guard.is_valid():
		return true
	var value: Variant = transition_guard.call(mode, parent, children)
	if value is bool:
		return bool(value)
	if value is Dictionary:
		var result := value as Dictionary
		return int(result.get("error", OK)) == OK and bool(result.get("allowed", true))
	return false


func _coarsen_boundary_same_level(parent: HydroTileKey) -> bool:
	for direction in 4:
		var link := HydroTileTopology.neighbor(parent, direction)
		if link.is_empty():
			continue
		var neighbor := link.get("key") as HydroTileKey
		if neighbor == null:
			continue
		if scheduler.pool.contains(neighbor):
			continue
		if not HydroLODHierarchy.descendant_records(scheduler.pool, neighbor).is_empty():
			return false
		var covering := HydroLODHierarchy.covering_record(scheduler.pool, neighbor)
		if not covering.is_empty():
			return false
	return true


func _refine_boundary_same_level(parent: HydroTileKey) -> bool:
	var child_level := parent.level + 1
	for direction in 4:
		var link := HydroTileTopology.neighbor(parent, direction)
		if link.is_empty():
			continue
		var neighbor := link.get("key") as HydroTileKey
		if neighbor == null:
			continue
		if scheduler.pool.contains(neighbor):
			return false
		var covering := HydroLODHierarchy.covering_record(scheduler.pool, neighbor)
		if not covering.is_empty():
			return false
		for record in HydroLODHierarchy.descendant_records(scheduler.pool, neighbor):
			var key := record.get("key") as HydroTileKey
			if key == null or key.level != child_level:
				return false
	return true


func _reject(error: Error, reason: String) -> Dictionary:
	return {"error": error, "eligible": false, "reason": reason}


func stats() -> Dictionary:
	return {
		"initialized": _initialized,
		"busy": _busy,
		"maximum_physical_lod": maximum_physical_lod,
		"cross_lod_interfaces_enabled": cross_lod_interfaces_enabled,
		"conservative_restriction": true,
		"terrain_aware_conservative_prolongation": true,
		"atomic_hierarchy_swap": true,
		"rollback_capable": true,
		"active_request_id": _request_id,
		"active_mode": _mode,
	}


func release() -> void:
	if _release_requested:
		return
	_release_requested = true
	if _busy or (transfer != null and transfer.pending()):
		return
	_finish_release()


func _finish_release() -> void:
	if _busy:
		return
	if terrain_bed != null:
		if terrain_bed.stage_recorded.is_connected(_on_terrain_stage_recorded):
			terrain_bed.stage_recorded.disconnect(_on_terrain_stage_recorded)
		if terrain_bed.stage_failed.is_connected(_on_terrain_stage_failed):
			terrain_bed.stage_failed.disconnect(_on_terrain_stage_failed)
	_cleanup_transfer()
	_initialized = false
	_release_requested = false
	transition_guard = Callable()
	_clear_refs()
	released.emit()


func _cleanup_transfer() -> void:
	if transfer != null and is_instance_valid(transfer):
		transfer.release()
		transfer.queue_free()
	transfer = null


func _clear_refs() -> void:
	scheduler = null
	atlas = null
	connectivity = null
	terrain_bed = null


func _exit_tree() -> void:
	release()
