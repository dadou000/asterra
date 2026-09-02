class_name HydroFrontierCoarsePreseed
extends Node
## Transactional coarse-water transfer for a newly reserved frontier tile.
##
## A frontier destination may overlap macro-scale surface water already owned by
## PlanetHydrologyOwnershipStore. Before the destination becomes solver-visible we
## transfer the macro surface depth represented by exactly one fine tile footprint:
##
##   parcel = coarse_surface_depth * fine_tile_area
##
## The parcel is quantized through HydroCoarseSeedGPU.plan_volume(), reserved in the
## coarse store, seeded identically into hidden sparse A+B, and committed only after
## the GPU seed dispatch is recorded. The normal frontier edge handoff is separate
## and runs afterwards. If there is no coarse surface water, request_preseed()
## returns 0 (synchronous no-op).
##
## A committed preseed remains *provisional* until the activation pipeline calls
## finalize_tile(). If a later edge handoff/connectivity step fails, restore_tile()
## returns the exact committed surface/channel categories to coarse storage while
## the hidden/released sparse bytes cease to be authoritative. A restore that is
## temporarily blocked by another pending promotion is retained and retried as soon
## as the ownership store has no unresolved reservations. A lightweight process
## poll closes the same guarantee even when the blocking reservation belongs to a
## different ownership path rather than this coordinator.

signal initialized
signal initialization_failed(error: Error)
signal preseed_started(request_id: int, transaction_id: int, cell: int, tile_id: int, slot: int)
signal preseed_completed(request_id: int, tile_id: int, slot: int, report: Dictionary)
signal preseed_failed(request_id: int, tile_id: int, slot: int, error: Error, stage: String)
signal released

var store: PlanetHydrologyOwnershipStore
var atlas: SparseHydroAtlasGPU
var seeder: HydroCoarseSeedGPU

var _initialized := false
var _next_request_id := 1
var _pending_by_seed_request: Dictionary = {} # seed request -> request dictionary
var _committed_by_tile: Dictionary = {} # tile id -> committed transfer awaiting activation finalization
var _restore_requested: Dictionary = {} # tile id -> true; retry when store reservations drain


func initialize(p_store: PlanetHydrologyOwnershipStore,
		p_atlas: SparseHydroAtlasGPU) -> Error:
	if _initialized or seeder != null:
		return ERR_BUSY
	if p_store == null or not p_store.initialized \
			or p_atlas == null or not p_atlas.initialized_ok():
		return ERR_UNCONFIGURED
	store = p_store
	atlas = p_atlas
	seeder = HydroCoarseSeedGPU.new()
	seeder.name = "HydroCoarseSeedGPU"
	add_child(seeder)
	seeder.initialized.connect(_on_seeder_initialized)
	seeder.initialization_failed.connect(_on_seeder_initialization_failed)
	seeder.seed_recorded.connect(_on_seed_recorded)
	seeder.seed_failed.connect(_on_seed_failed)
	var err := seeder.initialize(atlas)
	if err != OK:
		seeder.queue_free()
		seeder = null
		store = null
		atlas = null
	return err


func initialized_ok() -> bool:
	return _initialized


func pending_count() -> int:
	return _pending_by_seed_request.size()


func provisional_commit_count() -> int:
	return _committed_by_tile.size()


func deferred_restore_count() -> int:
	return _restore_requested.size()


func has_provisional_tile(tile_id: int) -> bool:
	return _committed_by_tile.has(tile_id)


func _process(_delta: float) -> void:
	if _initialized and not _restore_requested.is_empty():
		_retry_requested_restores()


## Returns:
##   >0 asynchronous preseed request ID
##    0 no coarse surface water overlaps this fine footprint
##   -1 rejected/error (no coarse debit is left pending)
func request_preseed(key: HydroTileKey, slot: int,
		local_velocity: Vector2 = Vector2.ZERO) -> int:
	if not _initialized or key == null or slot < 0 or slot >= atlas.capacity \
			or not is_finite(local_velocity.x) or not is_finite(local_velocity.y) \
			or _committed_by_tile.has(key.packed()):
		return -1
	var cell := coarse_cell_for_tile(key)
	if cell < 0:
		return -1
	var state := store.cell_state(cell)
	if state.is_empty():
		return -1
	var surface_depth := maxf(float(state.get("surface_depth_m", 0.0)), 0.0)
	if surface_depth <= 1.0e-9:
		return 0
	var fine_area := atlas.cell_size_m * atlas.cell_size_m \
		* float(atlas.cells_per_tile())
	var requested := minf(surface_depth * fine_area,
		store.available_promotion_volume_m3(cell))
	if requested <= 1.0e-9:
		return 0
	var plan := seeder.plan_volume(requested)
	if int(plan.get("error", FAILED)) != OK:
		return -1
	var represented := float(plan.get("represented_volume_m3", 0.0))
	var depth := float(plan.get("depth_m", 0.0))
	var prepared := store.prepare_promotion(cell, represented)
	if int(prepared.get("error", FAILED)) != OK:
		return -1

	var request_id := _next_request_id
	_next_request_id += 1
	var seed_request := seeder.seed_reserved(slot, depth, local_velocity)
	if seed_request < 0:
		store.rollback_promotion(int(prepared.get("transaction_id", -1)))
		return -1
	_pending_by_seed_request[seed_request] = {
		"request_id": request_id,
		"seed_request_id": seed_request,
		"transaction_id": int(prepared.get("transaction_id", -1)),
		"cell": cell,
		"tile_id": key.packed(),
		"slot": slot,
		"requested_volume_m3": requested,
		"represented_volume_m3": represented,
		"seed_depth_m": depth,
	}
	preseed_started.emit(request_id, int(prepared.get("transaction_id", -1)),
		cell, key.packed(), slot)
	return request_id


func coarse_cell_for_tile(key: HydroTileKey) -> int:
	if not _initialized or key == null or store == null or store.grid == null:
		return -1
	return store.grid.dir_to_index(_tile_center_dir(key))


func suggested_volume_m3(key: HydroTileKey) -> float:
	var cell := coarse_cell_for_tile(key)
	if cell < 0:
		return 0.0
	var state := store.cell_state(cell)
	if state.is_empty():
		return 0.0
	var fine_area := atlas.cell_size_m * atlas.cell_size_m \
		* float(atlas.cells_per_tile())
	return minf(maxf(float(state.get("surface_depth_m", 0.0)), 0.0) * fine_area,
		store.available_promotion_volume_m3(cell))


## Activation/connectivity succeeded: the fine tile is now authoritative and the
## committed transfer no longer needs rollback metadata.
func finalize_tile(tile_id: int) -> bool:
	_restore_requested.erase(tile_id)
	return _committed_by_tile.erase(tile_id)


## Activation/handoff failed after coarse commit. Return the exact categories that
## commit_promotion() removed. If another coarse reservation is still outstanding,
## remember the request rather than dropping it; a later seed completion/failure or
## process poll retries once the store barrier is clear.
func restore_tile(tile_id: int) -> Dictionary:
	var value: Variant = _committed_by_tile.get(tile_id, null)
	if not (value is Dictionary) or store == null:
		_restore_requested.erase(tile_id)
		return {"error": ERR_DOES_NOT_EXIST, "tile_id": tile_id}
	_restore_requested[tile_id] = true
	var committed: Dictionary = value
	var cell := int(committed.get("cell", -1))
	var surface := float(committed.get("reserved_surface_volume_m3", 0.0))
	var channel := float(committed.get("reserved_channel_volume_m3", 0.0))
	var result := store.accept_demotion(cell, surface, channel)
	if int(result.get("error", FAILED)) == OK:
		_committed_by_tile.erase(tile_id)
		_restore_requested.erase(tile_id)
	return result


func _on_seed_recorded(seed_request_id: int, slot: int,
		represented_volume_m3: float) -> void:
	var value: Variant = _pending_by_seed_request.get(seed_request_id, null)
	if not (value is Dictionary):
		return
	var pending: Dictionary = value
	_pending_by_seed_request.erase(seed_request_id)
	var request_id := int(pending.get("request_id", -1))
	var tile_id := int(pending.get("tile_id", -1))
	var expected_slot := int(pending.get("slot", -1))
	var expected_volume := float(pending.get("represented_volume_m3", 0.0))
	var transaction_id := int(pending.get("transaction_id", -1))
	if slot != expected_slot or absf(represented_volume_m3 - expected_volume) \
			> maxf(1.0e-7, absf(expected_volume) * 1.0e-7):
		store.rollback_promotion(transaction_id)
		_retry_requested_restores()
		preseed_failed.emit(request_id, tile_id, expected_slot,
			ERR_INVALID_DATA, "seed_ack")
		return
	var committed := store.commit_promotion(transaction_id)
	if int(committed.get("error", FAILED)) != OK:
		# commit_promotion() keeps a rejected transaction pending. Release that
		# reservation explicitly so the coarse store cannot remain globally blocked.
		if not store.promotion_transaction(transaction_id).is_empty():
			store.rollback_promotion(transaction_id)
		_retry_requested_restores()
		preseed_failed.emit(request_id, tile_id, expected_slot,
			int(committed.get("error", ERR_INVALID_DATA)), "coarse_commit")
		return
	var report := committed.duplicate(true)
	report["request_id"] = request_id
	report["tile_id"] = tile_id
	report["slot"] = expected_slot
	report["represented_volume_m3"] = expected_volume
	report["seed_depth_m"] = float(pending.get("seed_depth_m", 0.0))
	# Keep enough information to reverse the coarse debit until sparse activation
	# and connectivity have both succeeded.
	_committed_by_tile[tile_id] = report.duplicate(true)
	_retry_requested_restores()
	preseed_completed.emit(request_id, tile_id, expected_slot, report)


func _on_seed_failed(seed_request_id: int, error: Error) -> void:
	var value: Variant = _pending_by_seed_request.get(seed_request_id, null)
	if not (value is Dictionary):
		return
	var pending: Dictionary = value
	_pending_by_seed_request.erase(seed_request_id)
	var transaction_id := int(pending.get("transaction_id", -1))
	if transaction_id >= 0 and not store.promotion_transaction(transaction_id).is_empty():
		store.rollback_promotion(transaction_id)
	_retry_requested_restores()
	preseed_failed.emit(int(pending.get("request_id", -1)),
		int(pending.get("tile_id", -1)), int(pending.get("slot", -1)),
		error, "seed")


func _retry_requested_restores() -> void:
	if store == null or store.pending_promotion_count() > 0 \
			or _restore_requested.is_empty():
		return
	var requested := _restore_requested.keys()
	for tile_variant: Variant in requested:
		var tile_id := int(tile_variant)
		if not _committed_by_tile.has(tile_id):
			_restore_requested.erase(tile_id)
			continue
		var result := restore_tile(tile_id)
		var error := int(result.get("error", FAILED))
		if error != OK and error != ERR_BUSY:
			push_error("HydroFrontierCoarsePreseed: deferred restore failed for tile %d (%d)." % [
				tile_id, error])


static func _tile_center_dir(key: HydroTileKey) -> Vector3:
	var side := float(1 << key.level)
	var u := ((float(key.x) + 0.5) / side) * 2.0 - 1.0
	var v := ((float(key.y) + 0.5) / side) * 2.0 - 1.0
	return CubeSphere.face_uv_to_dir(key.face, u, v)


func _on_seeder_initialized() -> void:
	_initialized = true
	initialized.emit()


func _on_seeder_initialization_failed(error: Error) -> void:
	_initialized = false
	initialization_failed.emit(error)


func release() -> void:
	# Roll back unresolved coarse reservations first.
	for value: Variant in _pending_by_seed_request.values():
		if value is Dictionary:
			var transaction_id := int((value as Dictionary).get("transaction_id", -1))
			if transaction_id >= 0 and store != null \
					and not store.promotion_transaction(transaction_id).is_empty():
				store.rollback_promotion(transaction_id)
	_pending_by_seed_request.clear()
	# Any committed-but-unfinalized seed still belongs to an unpublished/failed
	# frontier tile. With all reservations released, every demotion can complete now.
	var committed_ids := _committed_by_tile.keys()
	for tile_variant: Variant in committed_ids:
		var result := restore_tile(int(tile_variant))
		if int(result.get("error", FAILED)) != OK:
			push_error("HydroFrontierCoarsePreseed: release restore failed for tile %d (%d)." % [
				int(tile_variant), int(result.get("error", FAILED))])
	_committed_by_tile.clear()
	_restore_requested.clear()
	if seeder != null and is_instance_valid(seeder):
		seeder.release()
		seeder.queue_free()
	seeder = null
	store = null
	atlas = null
	_initialized = false
	released.emit()


func _exit_tree() -> void:
	release()