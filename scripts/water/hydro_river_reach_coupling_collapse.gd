class_name HydroRiverReachCouplingCollapse
extends HydroRiverReachCouplingProduction
## Adds a transactional suspension boundary for fine->1D river collapse.
##
## Suspending a reach removes it from future mouth exchange while keeping both the
## sparse tile and PlanetHydrologyRiverCoupledStore refinement record authoritative.
## The collapse bridge may then GPU-reduce the tile and either:
##   resume_suspended_reach()   -- fine remains authoritative after rollback
##   finalize_suspended_reach() -- fine is gone; pending coarse inflow returns to 1D

var _suspended_records: Dictionary = {} # cell -> coupling record


func suspend_reach(cell: int) -> Dictionary:
	if not initialized_ok() or pending() or _release_requested:
		return {"error": ERR_BUSY, "reason": "coupling_not_quiescent"}
	if _suspended_records.has(cell):
		return {"error": ERR_ALREADY_EXISTS, "reason": "reach_already_suspended"}
	if not _records.has(cell) or store == null or not store.is_refined_reach(cell):
		return {"error": ERR_DOES_NOT_EXIST, "reason": "reach_not_registered"}
	var record := (_records[cell] as Dictionary).duplicate(true)
	_records.erase(cell)
	_suspended_records[cell] = record
	return {
		"error": OK,
		"cell": cell,
		"record": record.duplicate(true),
		"pending_coarse_inflow_m3": store.refined_inflow_available_m3(cell),
	}


func resume_suspended_reach(cell: int) -> Error:
	if pending() or _release_requested:
		return ERR_BUSY
	if not _suspended_records.has(cell):
		return ERR_DOES_NOT_EXIST
	if store == null or not store.is_refined_reach(cell) or runtime == null \
			or runtime.scheduler == null or runtime.scheduler.pool == null:
		return ERR_UNCONFIGURED
	var record := _suspended_records[cell] as Dictionary
	var key := HydroTileKey.unpack(int(record.get("tile_id", -1)))
	var slot := int(record.get("slot", -1))
	if key == null or not runtime.scheduler.pool.contains(key) \
			or runtime.scheduler.pool.slot_for(key) != slot:
		return ERR_CANT_ACQUIRE_RESOURCE
	_records[cell] = record.duplicate(true)
	_suspended_records.erase(cell)
	return OK


## Finalize only after authoritative fine ownership has left the sparse atlas.
## Pending donor/confluence water never left coarse ownership, so unregistering the
## refinement simply folds that pending parcel back into channel_storage_m3.
func finalize_suspended_reach(cell: int,
		return_pending_to_channel: bool = true) -> Dictionary:
	if pending():
		return {"error": ERR_BUSY, "reason": "exchange_pending"}
	if not _suspended_records.has(cell):
		return {"error": ERR_DOES_NOT_EXIST, "reason": "reach_not_suspended"}
	if store == null or not store.is_refined_reach(cell):
		return {"error": ERR_UNCONFIGURED, "reason": "coarse_refinement_missing"}
	var result := store.unregister_refined_reach(cell, return_pending_to_channel)
	if int(result.get("error", FAILED)) != OK:
		return result
	_suspended_records.erase(cell)
	return result


func suspended_reach(cell: int) -> Dictionary:
	return (_suspended_records[cell] as Dictionary).duplicate(true) \
		if _suspended_records.has(cell) else {}


func suspended_count() -> int:
	return _suspended_records.size()


func stats() -> Dictionary:
	var out := super.stats()
	out["suspended_reaches"] = _suspended_records.size()
	return out


func _finish_release() -> void:
	# Do not mutate coarse ownership for suspended records during generation teardown;
	# the collapse owner (or world transition) decides which representation survives.
	_suspended_records.clear()
	super._finish_release()
