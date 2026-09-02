class_name SparseHydroScheduler
extends RefCounted
## Phase 3 CPU-side policy layer for sparse hydrology activity.
##
## GPU kernels will eventually produce the activity/boundary summaries consumed
## here. This scheduler owns representation policy only: which stable tile IDs
## deserve transient slots, when neighbors wake, and when quiet dry tiles recycle.

signal tile_woken(tile_id: int, slot: int, reason: String)
signal tile_settling(tile_id: int)
signal tile_slept(tile_id: int, released_slot: int)
signal tile_frozen(tile_id: int)
signal allocation_failed(tile_id: int, reason: String)

const DIR_WEST := 0
const DIR_EAST := 1
const DIR_SOUTH := 2
const DIR_NORTH := 3

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


## Called with compact per-tile reductions from the GPU. quiet_dt_s is the elapsed
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
		pool.set_state(key, HydroTilePool.TileState.SLEEPING_DRY, "dry_sleep")
		pool.release(key)
		tile_slept.emit(key.packed(), released)
	elif freeze_wet_tiles and state == HydroTilePool.TileState.SETTLING:
		pool.set_state(key, HydroTilePool.TileState.FROZEN_WATER, "quiet_wet")
		tile_frozen.emit(key.packed())


## Wake a same-face neighbor only when an outgoing boundary flux is both large
## enough and topologically reachable. Cube-face seam routing is intentionally
## delegated to the future planetary neighbor mapper rather than guessed here.
func report_boundary_flux(key: HydroTileKey, direction: int, flux_m3s: float,
		reachable: bool, neighbor_lod: int = -1) -> int:
	if key == null or not reachable or flux_m3s <= wake_flux_threshold_m3s:
		return -1
	var delta := _direction_delta(direction)
	if delta == Vector2i.ZERO:
		return -1
	var neighbor := key.same_face_neighbor(delta.x, delta.y)
	if neighbor == null:
		return -1
	var lod := key.level if neighbor_lod < 0 else neighbor_lod
	return wake(neighbor, lod, "boundary_flux")


func thaw(key: HydroTileKey, reason: String = "disturbance") -> int:
	if key == null or not pool.contains(key):
		return wake(key, key.level if key != null else 0, reason)
	pool.set_state(key, HydroTilePool.TileState.ACTIVE, reason)
	pool.reset_quiet_time(key)
	return pool.slot_for(key)


func force_release(key: HydroTileKey) -> bool:
	return key != null and pool.release(key)


func stats() -> Dictionary:
	var result := pool.stats()
	result["wake_flux_threshold_m3s"] = wake_flux_threshold_m3s
	result["active_velocity_threshold_mps"] = active_velocity_threshold_mps
	result["active_disturbance_threshold"] = active_disturbance_threshold
	result["dry_depth_threshold_m"] = dry_depth_threshold_m
	result["settle_time_s"] = settle_time_s
	result["sleep_time_s"] = sleep_time_s
	return result


func _direction_delta(direction: int) -> Vector2i:
	match direction:
		DIR_WEST: return Vector2i(-1, 0)
		DIR_EAST: return Vector2i(1, 0)
		DIR_SOUTH: return Vector2i(0, -1)
		DIR_NORTH: return Vector2i(0, 1)
	return Vector2i.ZERO
