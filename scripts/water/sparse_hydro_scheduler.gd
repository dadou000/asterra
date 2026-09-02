class_name SparseHydroScheduler
extends RefCounted
## Phase 3 CPU-side policy layer for sparse hydrology activity.
##
## GPU kernels produce compact activity/boundary summaries; this scheduler owns
## representation policy only: which stable tile IDs deserve transient slots,
## when exact cube-sphere neighbors wake, and when quiet tiles recycle/freeze.

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


func wake(key: HydroTileKey, physical_lod: int = 0, reason: String = "frontier") -> int:
	if key == null:
		return -1
	var existing := pool.slot_for(key)
	if existing >= 0:
		pool.set_state(key, HydroTilePool.TileState.ACTIVE, reason)
		pool.reset_quiet_time(key)
		return existing
	var slot := pool.allocate(key, physical_lod)
	if slot < 0:
		allocation_failed.emit(key.packed(), reason)
		return -1
	pool.set_state(key, HydroTilePool.TileState.ACTIVE, reason)
	pool.reset_quiet_time(key)
	tile_woken.emit(key.packed(), slot, reason)
	return slot


## Called with compact per-tile reductions from the GPU. quiet_dt_s is elapsed
## physical simulation time represented by this report, not wall-clock time.
func report_activity(key: HydroTileKey, max_depth_m: float,
		max_velocity_mps: float, max_outgoing_flux_m3s: float,
		disturbance_energy: float, quiet_dt_s: float) -> void:
	if key == null or not pool.contains(key):
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


## Resolve the destination from Asterra's exact cube-sphere topology, including
## cross-face seams. Reachability remains explicit: topology says *which* tile is
## adjacent; terrain/banks/structures decide whether flux can actually enter it.
func report_boundary_flux(key: HydroTileKey, direction: int, flux_m3s: float,
		reachable: bool, neighbor_lod: int = -1) -> int:
	if key == null:
		return -1
	var link := HydroTileTopology.neighbor(key, direction)
	if link.is_empty():
		return -1
	return report_resolved_boundary_flux(key, link["key"], flux_m3s,
		reachable, neighbor_lod)


## Explicit destination variant used by the compact GPU frontier resolver. It is
## also the future entry point for coarse/fine topology where the destination may
## not be the same quadtree level as the source.
func report_resolved_boundary_flux(source: HydroTileKey, destination: HydroTileKey,
		flux_m3s: float, reachable: bool, neighbor_lod: int = -1) -> int:
	if source == null or destination == null or not reachable \
			or flux_m3s <= wake_flux_threshold_m3s:
		return -1
	var source_record := pool.record(source)
	if source_record.is_empty():
		return -1
	var inherited_lod := int(source_record.get("physical_lod", 0))
	var physical_lod := inherited_lod if neighbor_lod < 0 else maxi(neighbor_lod, 0)
	return wake(destination, physical_lod, "boundary_flux")


func thaw(key: HydroTileKey, reason: String = "disturbance") -> int:
	if key == null:
		return -1
	if not pool.contains(key):
		# No previous transient record exists, so the caller must decide the desired
		# physical representation. Default is finest local level 0, never key.level.
		return wake(key, 0, reason)
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
	return result
