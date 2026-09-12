class_name SparseHydroIdentityBridge
extends Node
## Mirrors SparseHydroScheduler slot lifecycle into SparseHydroAtlasGPU identity.
##
## The scheduler remains the stable authority for tile<->slot ownership. This
## bridge ensures GPU occupancy and (face, level, x, y) metadata follow every wake
## and release, including force-release paths. Water state initialization itself
## is a separate concern handled by reconstruction/prolongation passes.

signal bound
signal unbound
signal mirror_request_failed(tile_id: int, slot: int, error: Error)

var scheduler: SparseHydroScheduler
var atlas: SparseHydroAtlasGPU
var _bound := false


func bind(p_scheduler: SparseHydroScheduler, p_atlas: SparseHydroAtlasGPU) -> Error:
	if _bound:
		return ERR_ALREADY_IN_USE
	if p_scheduler == null or p_atlas == null or not p_atlas.initialized_ok():
		return ERR_INVALID_PARAMETER
	if p_scheduler.pool == null or p_scheduler.pool.capacity != p_atlas.capacity:
		return ERR_INVALID_PARAMETER

	scheduler = p_scheduler
	atlas = p_atlas
	var err := atlas.sync_pool(scheduler.pool)
	if err != OK:
		scheduler = null
		atlas = null
		return err

	scheduler.tile_woken.connect(_on_tile_woken)
	scheduler.tile_released.connect(_on_tile_released)
	_bound = true
	bound.emit()
	return OK


func is_bound() -> bool:
	return _bound


func sync_all() -> Error:
	if not _bound or scheduler == null or atlas == null:
		return ERR_UNCONFIGURED
	return atlas.sync_pool(scheduler.pool)


func unbind() -> void:
	if not _bound:
		return
	if scheduler != null:
		if scheduler.tile_woken.is_connected(_on_tile_woken):
			scheduler.tile_woken.disconnect(_on_tile_woken)
		if scheduler.tile_released.is_connected(_on_tile_released):
			scheduler.tile_released.disconnect(_on_tile_released)
	_bound = false
	scheduler = null
	atlas = null
	unbound.emit()


func _on_tile_woken(tile_id: int, slot: int, _reason: String) -> void:
	if not _bound or atlas == null:
		return
	var key := HydroTileKey.unpack(tile_id)
	var err := atlas.bind_slot_key(slot, key)
	if err != OK:
		mirror_request_failed.emit(tile_id, slot, err)


func _on_tile_released(tile_id: int, slot: int, _reason: String) -> void:
	if not _bound or atlas == null:
		return
	var err := atlas.unbind_slot(slot)
	if err != OK:
		mirror_request_failed.emit(tile_id, slot, err)


func _exit_tree() -> void:
	unbind()
