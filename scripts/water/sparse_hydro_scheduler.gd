class_name SparseHydroScheduler
extends RefCounted
## CPU-side policy layer for sparse hydrology activity and physical representation.
##
## Frontier expansion uses a two-stage lifecycle:
##   reserve() -> ALLOCATING (CPU slot owned, GPU occupancy still unpublished)
##   activate_reserved() -> ACTIVE (tile_woken publishes GPU identity)
##
## Phase 4 additionally enforces quadtree ownership: an ordinary allocation cannot
## overlap an already-resident ancestor/descendant. Atomic LOD transitions use the
## dedicated reserve_for_lod_transition() escape hatch, which names the exact owners
## it is replacing while still rejecting every unrelated overlap.

signal tile_reserved(tile_id: int, slot: int, reason: String)
signal tile_woken(tile_id: int, slot: int, reason: String)
signal tile_settling(tile_id: int)
signal tile_slept(tile_id: int, released_slot: int)
signal tile_released(tile_id: int, released_slot: int, reason: String)
signal tile_frozen(tile_id: int)
signal allocation_failed(tile_id: int, reason: String)

const DIR_WEST := HydroTileTopology.DIR_WEST
const DIR_EAST := HydroTileTopology.DIR_EAST
const DIR_SOUTH := HydroTileTopology.DIR_SOUTH
const DIR_NORTH := HydroTileTopology.DIR_NORTH

var pool: HydroTilePool
var wake_flux_threshold_m3s := 0.01
var active_velocity_threshold_mps := 0.015
var active_disturbance_threshold := 1.0e-4
var dry_depth_threshold_m := 0.002
var settle_time_s := 2.0
var sleep_time_s := 6.0
var freeze_wet_tiles := true


func _init(capacity: int = 1024) -> void:
	pool = HydroTilePool.new(capacity)


## Reserve transient ownership without publishing the tile as active. Existing
## exact tiles retain their current representation and simply return their slot.
func reserve(key: HydroTileKey, physical_lod: int = 0,
		reason: String = "reserve") -> int:
	return _reserve_internal(key, physical_lod, reason, PackedInt64Array())


## Hidden allocation used only by an atomic parent<->children transfer. The caller
## supplies the stable IDs that will be removed at commit. No other overlapping
## owner is allowed, preventing a transition from masking an unrelated descendant.
func reserve_for_lod_transition(key: HydroTileKey, physical_lod: int,
		replacing_ids: PackedInt64Array,
		reason: String = "hydrolod_transition") -> int:
	return _reserve_internal(key, physical_lod, reason, replacing_ids)


func _reserve_internal(key: HydroTileKey, physical_lod: int, reason: String,
		ignored_overlap_ids: PackedInt64Array) -> int:
	if key == null:
		return -1
	var existing := pool.slot_for(key)
	if existing >= 0:
		return existing
	var conflict := HydroLODHierarchy.representation_conflict(
		pool, key, ignored_overlap_ids)
	if bool(conflict.get("conflict", true)):
		allocation_failed.emit(key.packed(), String(conflict.get("reason", reason)))
		return -1
	var slot := pool.allocate(key, physical_lod)
	if slot < 0:
		allocation_failed.emit(key.packed(), reason)
		return -1
	pool.set_state(key, HydroTilePool.TileState.ALLOCATING, reason)
	pool.reset_quiet_time(key)
	tile_reserved.emit(key.packed(), slot, reason)
	return slot


## Promote an already reserved tile after its GPU state has been initialized.
func activate_reserved(key: HydroTileKey, reason: String = "activate") -> int:
	if key == null or not pool.contains(key):
		return -1
	var slot := pool.slot_for(key)
	if slot < 0:
		return -1
	pool.set_state(key, HydroTilePool.TileState.ACTIVE, reason)
	pool.reset_quiet_time(key)
	tile_woken.emit(key.packed(), slot, reason)
	return slot


func wake(key: HydroTileKey, physical_lod: int = 0, reason: String = "frontier") -> int:
	if key == null:
		return -1
	var existing := pool.slot_for(key)
	if existing >= 0:
		var record := pool.record(key)
		if int(record.get("state", HydroTilePool.TileState.ACTIVE)) \
				== HydroTilePool.TileState.ALLOCATING:
			return activate_reserved(key, reason)
		pool.set_state(key, HydroTilePool.TileState.ACTIVE, reason)
		pool.reset_quiet_time(key)
		return existing
	var slot := reserve(key, physical_lod, reason)
	if slot < 0:
		return -1
	return activate_reserved(key, reason)


func report_activity(key: HydroTileKey, max_depth_m: float,
		max_velocity_mps: float, max_outgoing_flux_m3s: float,
		disturbance_energy: float, quiet_dt_s: float) -> void:
	if key == null or not pool.contains(key):
		return
	var record_before := pool.record(key)
	if int(record_before.get("state", HydroTilePool.TileState.ALLOCATING)) \
			== HydroTilePool.TileState.ALLOCATING:
		return
	var active := max_velocity_mps > active_velocity_threshold_mps \
		or max_outgoing_flux_m3s > wake_flux_threshold_m3s \
		or disturbance_energy > active_disturbance_threshold
	if active:
		pool.update_activity(key, max_depth_m, max_velocity_mps,
			max_outgoing_flux_m3s, disturbance_energy, 0.0)
		pool.reset_quiet_time(key)
		pool.set_state(key, HydroTilePool.TileState.ACTIVE, "activity")
		return

	pool.update_activity(key, max_depth_m, max_velocity_mps,
		max_outgoing_flux_m3s, disturbance_energy, maxf(quiet_dt_s, 0.0))
	var record := pool.record(key)
	var quiet := float(record.get("quiet_time_s", 0.0))
	var state := int(record.get("state", HydroTilePool.TileState.ACTIVE))
	if quiet >= settle_time_s and state == HydroTilePool.TileState.ACTIVE:
		pool.set_state(key, HydroTilePool.TileState.SETTLING, "quiet")
		tile_settling.emit(key.packed())
		state = HydroTilePool.TileState.SETTLING
	if quiet < sleep_time_s:
		return

	if max_depth_m <= dry_depth_threshold_m:
		var released := pool.slot_for(key)
		var id := key.packed()
		pool.set_state(key, HydroTilePool.TileState.SLEEPING_DRY, "dry_sleep")
		pool.release(key)
		tile_slept.emit(id, released)
		tile_released.emit(id, released, "dry_sleep")
	elif freeze_wet_tiles and state == HydroTilePool.TileState.SETTLING:
		pool.set_state(key, HydroTilePool.TileState.FROZEN_WATER, "quiet_wet")
		tile_frozen.emit(key.packed())


func report_boundary_flux(key: HydroTileKey, direction: int, flux_m3s: float,
		reachable: bool, neighbor_lod: int = -1) -> int:
	if key == null:
		return -1
	var link := HydroTileTopology.neighbor(key, direction)
	if link.is_empty():
		return -1
	return report_resolved_boundary_flux(key, link["key"], flux_m3s,
		reachable, neighbor_lod)


func report_resolved_boundary_flux(source: HydroTileKey, destination: HydroTileKey,
		flux_m3s: float, reachable: bool, neighbor_lod: int = -1) -> int:
	var slot := reserve_resolved_boundary_flux(source, destination, flux_m3s,
		reachable, neighbor_lod)
	if slot < 0:
		return -1
	var record := pool.record(destination)
	if int(record.get("state", HydroTilePool.TileState.ACTIVE)) \
			== HydroTilePool.TileState.ALLOCATING:
		return activate_reserved(destination, "boundary_flux")
	return slot


func reserve_resolved_boundary_flux(source: HydroTileKey,
		destination: HydroTileKey, flux_m3s: float, reachable: bool,
		neighbor_lod: int = -1) -> int:
	if source == null or destination == null or not reachable \
			or flux_m3s <= wake_flux_threshold_m3s:
		return -1
	var source_record := pool.record(source)
	if source_record.is_empty():
		return -1
	var inherited_lod := int(source_record.get("physical_lod", 0))
	var physical_lod := inherited_lod if neighbor_lod < 0 else maxi(neighbor_lod, 0)
	return reserve(destination, physical_lod, "boundary_flux_handoff")


func cancel_reserved(key: HydroTileKey, reason: String = "handoff_cancelled") -> bool:
	if key == null or not pool.contains(key):
		return false
	var record := pool.record(key)
	if int(record.get("state", HydroTilePool.TileState.ACTIVE)) \
			!= HydroTilePool.TileState.ALLOCATING:
		return false
	var id := key.packed()
	var slot := pool.slot_for(key)
	if not pool.release(key):
		return false
	tile_released.emit(id, slot, reason)
	return true


func thaw(key: HydroTileKey, reason: String = "disturbance") -> int:
	if key == null:
		return -1
	if not pool.contains(key):
		return wake(key, 0, reason)
	var record := pool.record(key)
	if int(record.get("state", HydroTilePool.TileState.ACTIVE)) \
			== HydroTilePool.TileState.ALLOCATING:
		return activate_reserved(key, reason)
	pool.set_state(key, HydroTilePool.TileState.ACTIVE, reason)
	pool.reset_quiet_time(key)
	return pool.slot_for(key)


func force_release(key: HydroTileKey, reason: String = "forced") -> bool:
	if key == null or not pool.contains(key):
		return false
	var id := key.packed()
	var slot := pool.slot_for(key)
	if not pool.release(key):
		return false
	tile_released.emit(id, slot, reason)
	return true


func stats() -> Dictionary:
	var result := pool.stats()
	result["wake_flux_threshold_m3s"] = wake_flux_threshold_m3s
	result["active_velocity_threshold_mps"] = active_velocity_threshold_mps
	result["active_disturbance_threshold"] = active_disturbance_threshold
	result["dry_depth_threshold_m"] = dry_depth_threshold_m
	result["settle_time_s"] = settle_time_s
	result["sleep_time_s"] = sleep_time_s
	result["hierarchy_overlap_guard"] = true
	return result
