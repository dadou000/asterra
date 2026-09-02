class_name PlanetHydroDemotionBridge
extends Node
## Transactional bridge from one resident sparse SWE tile back into the persistent
## planetary coarse store.
##
## Sequence:
##   pause sparse runtime
##   -> compact GPU reduction of canonical atlas-A tile volume
##   -> prepare incoming coarse demotion transaction (physical coarse unchanged)
##   -> unpublish/release fine tile
##   -> rebuild sparse connectivity
##   -> commit already-validated incoming coarse parcel
##
## The reduced fine parcel remains classified as coarse *surface* storage. River
## channel classification is intentionally deferred until the persistent 1D
## reach/network representation exists.

signal initialized
signal initialization_failed(error: Error)
signal demotion_started(request_id: int, cell: int, tile_id: int, slot: int)
signal demotion_completed(request_id: int, report: Dictionary)
signal demotion_failed(request_id: int, error: Error, stage: String)
signal released

var store: PlanetHydrologyOwnershipStore
var scheduler: SparseHydroScheduler
var atlas: SparseHydroAtlasGPU
var connectivity: SparseHydroConnectivityGPU
var identity_bridge: SparseHydroIdentityBridge
var runtime: SparseHydrologyRuntime
var volume_diagnostic: SparseHydroTileVolumeDiagnosticsGPU

var minimum_transfer_volume_m3 := 1.0e-7

var _initialized := false
var _busy := false
var _next_request_id := 1
var _request_id := -1
var _volume_request_id := -1
var _transaction_id := -1
var _cell := -1
var _key: HydroTileKey
var _slot := -1
var _physical_lod := 0
var _runtime_was_enabled := true
var _fine_volume_m3 := 0.0


func initialize(p_store: PlanetHydrologyOwnershipStore,
		p_scheduler: SparseHydroScheduler, p_atlas: SparseHydroAtlasGPU,
		p_connectivity: SparseHydroConnectivityGPU,
		p_identity_bridge: SparseHydroIdentityBridge,
		p_runtime: SparseHydrologyRuntime = null) -> Error:
	if _initialized or volume_diagnostic != null:
		return ERR_BUSY
	if p_store == null or not p_store.initialized or p_scheduler == null \
			or p_atlas == null or not p_atlas.initialized_ok() \
			or p_connectivity == null or not p_connectivity.initialized_ok() \
			or p_identity_bridge == null or not p_identity_bridge.is_bound():
		return ERR_UNCONFIGURED
	if p_scheduler.pool == null or p_scheduler.pool.capacity != p_atlas.capacity \
			or p_connectivity.capacity != p_atlas.capacity:
		return ERR_INVALID_PARAMETER

	store = p_store
	scheduler = p_scheduler
	atlas = p_atlas
	connectivity = p_connectivity
	identity_bridge = p_identity_bridge
	runtime = p_runtime

	volume_diagnostic = SparseHydroTileVolumeDiagnosticsGPU.new()
	volume_diagnostic.name = "SparseHydroTileVolumeDiagnosticsGPU"
	add_child(volume_diagnostic)
	volume_diagnostic.initialized.connect(_on_diagnostic_initialized)
	volume_diagnostic.initialization_failed.connect(_on_diagnostic_initialization_failed)
	volume_diagnostic.volume_ready.connect(_on_tile_volume_ready)
	volume_diagnostic.readback_failed.connect(_on_tile_volume_failed)
	var err := volume_diagnostic.initialize_from_atlas(atlas)
	if err != OK:
		volume_diagnostic.queue_free()
		volume_diagnostic = null
		_clear_refs()
		return err
	return OK


func initialized_ok() -> bool:
	return _initialized


func busy() -> bool:
	return _busy


## Same metric mapping contract as PlanetHydroPromotionBridge. This initial reverse
## bridge collapses tiles that originated from one coarse-cell surface promotion.
func tile_key_for_cell(cell: int) -> HydroTileKey:
	if not _initialized or cell < 0 or cell >= store.cell_count():
		return null
	var contract := HydroMetricGrid.atlas_contract(atlas, store.grid.radius)
	var level := int(contract.get("level", 0))
	var mapped := CubeSphere.dir_to_face_uv(store.grid.cell_dir(cell))
	if mapped.size() < 3:
		return null
	var side := 1 << clampi(level, 0, HydroTileKey.MAX_LEVEL)
	var gx := clampf((float(mapped[1]) + 1.0) * 0.5 * float(side),
		0.0, float(side) - 1.0e-9)
	var gy := clampf((float(mapped[2]) + 1.0) * 0.5 * float(side),
		0.0, float(side) - 1.0e-9)
	return HydroTileKey.new(int(mapped[0]), level,
		clampi(int(floor(gx)), 0, side - 1),
		clampi(int(floor(gy)), 0, side - 1))


## Start one fine->coarse collapse. Returns request ID or -1 when rejected.
func demote_cell(cell: int, target_key: HydroTileKey = null) -> int:
	if not _initialized or _busy or cell < 0 or cell >= store.cell_count():
		return -1
	if store.pending_ownership_transaction_count() > 0:
		return -1
	if runtime != null and (not runtime.initialized_ok() or runtime.busy()):
		return -1
	var key := target_key if target_key != null else tile_key_for_cell(cell)
	if key == null or not scheduler.pool.contains(key):
		return -1
	var record := scheduler.pool.record(key)
	if record.is_empty() \
			or int(record.get("state", HydroTilePool.TileState.ALLOCATING)) \
				== HydroTilePool.TileState.ALLOCATING:
		return -1
	var slot := int(record.get("slot", -1))
	if slot < 0 or slot >= atlas.capacity:
		return -1

	_busy = true
	_request_id = _next_request_id
	_next_request_id += 1
	_cell = cell
	_key = key
	_slot = slot
	_physical_lod = int(record.get("physical_lod", 0))
	_fine_volume_m3 = 0.0
	if runtime != null:
		_runtime_was_enabled = runtime.enabled
		runtime.enabled = false

	_volume_request_id = volume_diagnostic.request_volume(slot)
	if _volume_request_id < 0:
		_fail(ERR_BUSY, "volume_request")
		return -1
	demotion_started.emit(_request_id, cell, key.packed(), slot)
	return _request_id


func _on_diagnostic_initialized() -> void:
	_initialized = true
	initialized.emit()


func _on_diagnostic_initialization_failed(error: Error) -> void:
	_initialized = false
	initialization_failed.emit(error)


func _on_tile_volume_ready(request_id: int, slot: int, volume_m3: float) -> void:
	if not _busy or request_id != _volume_request_id or slot != _slot:
		return
	if not is_finite(volume_m3) or volume_m3 < 0.0:
		_fail(ERR_INVALID_DATA, "volume_readback")
		return
	if _key == null or scheduler.pool.slot_for(_key) != _slot:
		_fail(ERR_BUSY, "tile_identity_changed")
		return
	_fine_volume_m3 = maxf(volume_m3, 0.0)

	# A dry tile has no representation parcel to transfer. It may be unpublished
	# directly while the sparse runtime is paused.
	if _fine_volume_m3 <= maxf(minimum_transfer_volume_m3, 0.0):
		if not scheduler.force_release(_key, "planet_demotion_dry"):
			_fail(ERR_CANT_ACQUIRE_RESOURCE, "dry_release")
			return
		var dry_conn := connectivity.sync_pool(scheduler.pool)
		if dry_conn != OK:
			_restore_fine_after_release(dry_conn, "dry_connectivity")
			return
		_complete({
			"cell": _cell,
			"tile_id": _key.packed(),
			"slot": _slot,
			"fine_volume_m3": 0.0,
			"surface_volume_m3": 0.0,
			"channel_volume_m3": 0.0,
			"dry_release": true,
		})
		return

	var prepared := store.prepare_demotion(_cell, _fine_volume_m3, 0.0)
	if int(prepared.get("error", FAILED)) != OK:
		_fail(int(prepared.get("error", ERR_INVALID_DATA)), "coarse_prepare")
		return
	_transaction_id = int(prepared.get("transaction_id", -1))
	if _transaction_id < 0:
		_fail(ERR_INVALID_DATA, "coarse_prepare_identity")
		return

	# CPU sparse ownership leaves fine before coarse receives the parcel. Because the
	# prepared demotion globally blocks coarse stepping/snapshots and the sparse
	# runtime is paused, no physical simulation can observe an ownerless timestep.
	if not scheduler.force_release(_key, "planet_fine_demotion"):
		store.rollback_demotion(_transaction_id)
		_transaction_id = -1
		_fail(ERR_CANT_ACQUIRE_RESOURCE, "fine_release")
		return
	var conn_error := connectivity.sync_pool(scheduler.pool)
	if conn_error != OK:
		_restore_fine_after_release(conn_error, "connectivity")
		return

	var committed := store.commit_demotion(_transaction_id)
	if int(committed.get("error", FAILED)) != OK:
		# This should be unreachable: prepare validated the destination and the pending
		# demotion blocks coarse stepping. Prefer restoring the still-intact raw fine
		# slot bytes rather than accepting ambiguous ownership.
		_restore_fine_after_release(
			int(committed.get("error", ERR_INVALID_DATA)), "coarse_commit")
		return
	_transaction_id = -1
	var report := committed.duplicate(true)
	report["tile_id"] = _key.packed()
	report["slot"] = _slot
	report["fine_volume_m3"] = _fine_volume_m3
	_complete(report)


func _on_tile_volume_failed(request_id: int, slot: int, error: Error) -> void:
	if _busy and request_id == _volume_request_id and slot == _slot:
		_fail(error, "volume_readback")


## Attempt to restore fine authority after CPU release while raw atlas bytes are
## still intact. HydroTilePool is LIFO, so with the runtime paused and no concurrent
## ownership transaction the just-freed slot must be reacquired exactly.
func _restore_fine_after_release(original_error: Error, stage: String) -> void:
	if _key == null:
		_fail(original_error, stage + "_restore_missing_key", false)
		return
	var restored_slot := scheduler.reserve(_key, _physical_lod,
		"planet_demotion_restore")
	if restored_slot == _slot:
		var activated := scheduler.activate_reserved(_key,
			"planet_demotion_restore")
		var restore_conn := connectivity.sync_pool(scheduler.pool)
		if activated == _slot and restore_conn == OK:
			if _transaction_id >= 0:
				store.rollback_demotion(_transaction_id)
				_transaction_id = -1
			_fail(original_error, stage + "_restored_fine", true)
			return

	# Fine publication could not be restored safely. Remove any partially restored
	# record and fall back to the already-validated incoming coarse transaction so
	# mass remains represented somewhere. Keep sparse runtime disabled because its
	# connectivity/identity state needs explicit recovery before further stepping.
	if scheduler.pool.contains(_key):
		scheduler.force_release(_key, "planet_demotion_restore_failed")
	if _transaction_id >= 0:
		var committed := store.commit_demotion(_transaction_id)
		_transaction_id = -1
		if int(committed.get("error", FAILED)) == OK:
			_fail(original_error, stage + "_coarse_fallback", false)
			return
	_fail(ERR_CANT_ACQUIRE_RESOURCE, stage + "_ownership_recovery_failed", false)


func _complete(report: Dictionary) -> void:
	var completed_id := _request_id
	var published := report.duplicate(true)
	published["request_id"] = completed_id
	_finish_request(true)
	demotion_completed.emit(completed_id, published)


func _fail(error: Error, stage: String, restore_runtime: bool = true) -> void:
	var failed_id := _request_id
	if _transaction_id >= 0 and store != null:
		store.rollback_demotion(_transaction_id)
		_transaction_id = -1
	_finish_request(restore_runtime)
	demotion_failed.emit(failed_id, error, stage)


func _finish_request(restore_runtime: bool) -> void:
	if runtime != null and restore_runtime:
		runtime.enabled = _runtime_was_enabled
	_busy = false
	_request_id = -1
	_volume_request_id = -1
	_transaction_id = -1
	_cell = -1
	_key = null
	_slot = -1
	_physical_lod = 0
	_fine_volume_m3 = 0.0


func _clear_refs() -> void:
	store = null
	scheduler = null
	atlas = null
	connectivity = null
	identity_bridge = null
	runtime = null


func release() -> void:
	if _busy:
		# Before fine release, rollback is enough. After fine release the normal
		# request callback owns recovery; release() should not fabricate a parcel from
		# an incomplete GPU readback.
		if _transaction_id >= 0 and store != null:
			store.rollback_demotion(_transaction_id)
		_finish_request(false)
	if volume_diagnostic != null and is_instance_valid(volume_diagnostic):
		volume_diagnostic.release()
		volume_diagnostic.queue_free()
	volume_diagnostic = null
	_initialized = false
	_clear_refs()
	released.emit()


func _exit_tree() -> void:
	release()
