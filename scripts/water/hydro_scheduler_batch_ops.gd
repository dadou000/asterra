class_name HydroSchedulerBatchOps
extends RefCounted
## Prevalidated multi-tile scheduler operations for representation clusters.
##
## All keys/slots/states are validated before the first pool mutation. Signals are
## emitted only after every pool record has changed, so identity publication sees a
## coherent cluster generation rather than an incrementally validated sequence.


static func activate_reserved(scheduler: SparseHydroScheduler,
		keys: Array[HydroTileKey], reason: String = "cluster_activate") -> PackedInt32Array:
	var slots := PackedInt32Array()
	if scheduler == null or scheduler.pool == null or keys.is_empty():
		return slots
	var seen: Dictionary = {}
	var ids := PackedInt32Array()
	for key in keys:
		if key == null or seen.has(key.packed()) or not scheduler.pool.contains(key):
			return PackedInt32Array()
		seen[key.packed()] = true
		var record := scheduler.pool.record(key)
		if int(record.get("state", HydroTilePool.TileState.ACTIVE)) \
				!= HydroTilePool.TileState.ALLOCATING:
			return PackedInt32Array()
		var slot := scheduler.pool.slot_for(key)
		if slot < 0:
			return PackedInt32Array()
		ids.append(key.packed())
		slots.append(slot)
	for key in keys:
		scheduler.pool.set_state(key, HydroTilePool.TileState.ACTIVE, reason)
		scheduler.pool.reset_quiet_time(key)
	for i in keys.size():
		scheduler.tile_woken.emit(ids[i], slots[i], reason)
	return slots


static func force_release(scheduler: SparseHydroScheduler,
		keys: Array[HydroTileKey], reason: String = "cluster_release") -> PackedInt32Array:
	var slots := PackedInt32Array()
	if scheduler == null or scheduler.pool == null or keys.is_empty():
		return slots
	var seen: Dictionary = {}
	var ids := PackedInt32Array()
	for key in keys:
		if key == null or seen.has(key.packed()) or not scheduler.pool.contains(key):
			return PackedInt32Array()
		seen[key.packed()] = true
		var slot := scheduler.pool.slot_for(key)
		if slot < 0:
			return PackedInt32Array()
		ids.append(key.packed())
		slots.append(slot)
	# Every release below is guaranteed by the prevalidation above unless the pool
	# is mutated re-entrantly. No scheduler signal is emitted until all records leave.
	for key in keys:
		if not scheduler.pool.release(key):
			return PackedInt32Array()
	for i in keys.size():
		scheduler.tile_released.emit(ids[i], slots[i], reason)
	return slots
