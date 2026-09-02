class_name PlanetHydroPromotionBridge
extends Node
## Transactional bridge from the persistent coarse planetary store to one newly
## allocated sparse SWE tile.
##
## The sparse runtime is paused only while this short ownership handoff is in
## flight. The destination remains ALLOCATING/occupancy=0 through terrain staging
## and terrain-aware exact-volume prolongation. Coarse storage is debited only after
## the GPU seed dispatch is recorded; only then is the tile activated and published.
##
## This bridge promotes into a *new* tile. Adding water to an already-live sparse
## tile belongs at an idle solver/source boundary and is intentionally rejected.

signal initialized
signal initialization_failed(error: Error)
signal promotion_started(request_id: int, transaction_id: int, cell: int, tile_id: int, slot: int)
signal promotion_completed(request_id: int, report: Dictionary)
signal promotion_failed(request_id: int, error: Error, stage: String)
signal released

var store: PlanetHydrologyOwnershipStore
var scheduler: SparseHydroScheduler
var atlas: SparseHydroAtlasGPU
var connectivity: SparseHydroConnectivityGPU
var identity_bridge: SparseHydroIdentityBridge
var terrain_bed: HydroTerrainBedGPU
var runtime: SparseHydrologyRuntime
var seeder: HydroCoarseProlongationGPU

var _initialized := false
var _busy := false
var _next_request_id := 1
var _request_id := -1
var _transaction_id := -1
var _cell := -1
var _key: HydroTileKey
var _slot := -1
var _terrain_request_id := -1
var _seed_request_id := -1
var _seed_equivalent_depth_m := 0.0
var _represented_volume_m3 := 0.0
var _local_velocity := Vector2.ZERO
var _runtime_was_enabled := true


func initialize(p_store: PlanetHydrologyOwnershipStore,
		p_scheduler: SparseHydroScheduler, p_atlas: SparseHydroAtlasGPU,
		p_connectivity: SparseHydroConnectivityGPU,
		p_identity_bridge: SparseHydroIdentityBridge,
		p_terrain_bed: HydroTerrainBedGPU,
		p_runtime: SparseHydrologyRuntime = null) -> Error:
	if _initialized or seeder != null:
		return ERR_BUSY
	if p_store == null or not p_store.initialized or p_scheduler == null \
			or p_atlas == null or not p_atlas.initialized_ok() \
			or p_connectivity == null or not p_connectivity.initialized_ok() \
			or p_identity_bridge == null or not p_identity_bridge.is_bound() \
			or p_terrain_bed == null or not p_terrain_bed.initialized_ok():
		return ERR_UNCONFIGURED
	if p_scheduler.pool == null or p_scheduler.pool.capacity != p_atlas.capacity:
		return ERR_INVALID_PARAMETER

	store = p_store
	scheduler = p_scheduler
	atlas = p_atlas
	connectivity = p_connectivity
	identity_bridge = p_identity_bridge
	terrain_bed = p_terrain_bed
	runtime = p_runtime
	terrain_bed.stage_recorded.connect(_on_terrain_stage_recorded)
	terrain_bed.stage_failed.connect(_on_terrain_stage_failed)

	seeder = HydroCoarseProlongationGPU.new()
	seeder.name = "HydroCoarseProlongationGPU"
	add_child(seeder)
	seeder.initialized.connect(_on_seeder_initialized)
	seeder.initialization_failed.connect(_on_seeder_initialization_failed)
	seeder.seed_recorded.connect(_on_seed_recorded)
	seeder.seed_failed.connect(_on_seed_failed)
	var err := seeder.initialize(atlas)
	if err != OK:
		_disconnect_terrain()
		seeder.queue_free()
		seeder = null
		_clear_refs()
		return err
	return OK


func initialized_ok() -> bool:
	return _initialized


func busy() -> bool:
	return _busy


## Resolve the fine tile containing a coarse PlanetGrid cell centre using the
## exact physical level chosen for the atlas metric contract.
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


## Conservative flood-oriented default: transfer only the coarse surface depth
## over one fine tile footprint, never the entire much larger macro cell volume.
## Channel-only promotions must choose an explicit parcel volume/policy.
func suggested_surface_volume_m3(cell: int) -> float:
	if not _initialized or cell < 0 or cell >= store.cell_count():
		return 0.0
	var state := store.cell_state(cell)
	if state.is_empty():
		return 0.0
	var surface_depth := maxf(float(state.get("surface_depth_m", 0.0)), 0.0)
	var fine_area := atlas.cell_size_m * atlas.cell_size_m * float(atlas.cells_per_tile())
	return minf(surface_depth * fine_area, store.available_promotion_volume_m3(cell))


## Start one coarse -> fine ownership transaction. The physical parcel is quantized
## directly to a non-increasing FP32 target, then distributed after terrain staging
## as a level free surface rather than uniform depth.
func promote_cell(cell: int, requested_volume_m3: float,
		local_velocity: Vector2 = Vector2.ZERO,
		target_key: HydroTileKey = null) -> int:
	if not _initialized or _busy or not is_finite(requested_volume_m3) \
			or requested_volume_m3 <= 0.0:
		return -1
	if runtime != null and (not runtime.initialized_ok() or runtime.busy()):
		return -1
	if not is_finite(local_velocity.x) or not is_finite(local_velocity.y):
		return -1
	var key := target_key if target_key != null else tile_key_for_cell(cell)
	if key == null or scheduler.pool.contains(key):
		return -1

	var plan := seeder.plan_volume(requested_volume_m3)
	if int(plan.get("error", FAILED)) != OK:
		return -1
	var represented := float(plan.get("represented_volume_m3", 0.0))
	var equivalent_depth := float(plan.get("equivalent_uniform_depth_m", 0.0))
	var prepared := store.prepare_promotion(cell, represented)
	if int(prepared.get("error", FAILED)) != OK:
		return -1

	var slot := scheduler.reserve(key, 0, "planet_coarse_promotion")
	if slot < 0:
		store.rollback_promotion(int(prepared["transaction_id"]))
		return -1

	_busy = true
	_request_id = _next_request_id
	_next_request_id += 1
	_transaction_id = int(prepared["transaction_id"])
	_cell = cell
	_key = key
	_slot = slot
	_seed_equivalent_depth_m = equivalent_depth
	_represented_volume_m3 = represented
	_local_velocity = local_velocity
	if runtime != null:
		_runtime_was_enabled = runtime.enabled
		runtime.enabled = false

	var stage := terrain_bed.stage_reserved_tile(key, slot)
	if not bool(stage.get("queued", false)) or int(stage.get("error", FAILED)) != OK:
		_fail(int(stage.get("error", ERR_INVALID_DATA)), "terrain_stage_submit")
		return -1
	_terrain_request_id = int(stage.get("request_id", -1))
	promotion_started.emit(_request_id, _transaction_id, cell, key.packed(), slot)
	return _request_id


func _on_terrain_stage_recorded(request_id: int, tile_id: int, slot: int) -> void:
	if not _busy or request_id != _terrain_request_id or _key == null \
			or tile_id != _key.packed() or slot != _slot:
		return
	_seed_request_id = seeder.seed_reserved(
		_slot, _represented_volume_m3, _local_velocity)
	if _seed_request_id < 0:
		_fail(ERR_BUSY, "prolongation_submit")


func _on_terrain_stage_failed(request_id: int, error: Error) -> void:
	if _busy and request_id == _terrain_request_id:
		_fail(error, "terrain_stage")


func _on_seed_recorded(request_id: int, slot: int,
		represented_volume_m3: float) -> void:
	if not _busy or request_id != _seed_request_id or slot != _slot:
		return
	var tolerance := maxf(1.0e-7, absf(_represented_volume_m3) * 1.0e-7)
	if absf(represented_volume_m3 - _represented_volume_m3) > tolerance:
		_fail(ERR_INVALID_DATA, "prolongation_volume_ack")
		return

	# The hidden fine slot now owns the parcel. Commit the coarse debit before
	# publishing occupancy; commit is deterministic because coarse stepping has
	# been blocked by the pending reservation.
	var committed := store.commit_promotion(_transaction_id)
	if int(committed.get("error", FAILED)) != OK:
		_fail(int(committed.get("error", ERR_INVALID_DATA)), "coarse_commit")
		return

	var activated_slot := scheduler.activate_reserved(_key, "planet_coarse_prolongated")
	if activated_slot != _slot:
		_restore_after_committed_failure(committed, "activate")
		return
	var conn_error := connectivity.sync_pool(scheduler.pool)
	if conn_error != OK:
		scheduler.force_release(_key, "planet_promotion_connectivity_failed")
		_restore_after_committed_failure(committed, "connectivity")
		return

	var report := committed.duplicate(true)
	report["request_id"] = _request_id
	report["tile_id"] = _key.packed()
	report["slot"] = _slot
	# Kept for compatibility/debug comparison; this is no longer the actual depth
	# written to every cell. Terrain-aware h varies while the free surface is level.
	report["seed_depth_m"] = _seed_equivalent_depth_m
	report["seed_strategy"] = "level_free_surface"
	report["represented_volume_m3"] = _represented_volume_m3
	var completed_id := _request_id
	_finish_request()
	promotion_completed.emit(completed_id, report)


func _on_seed_failed(request_id: int, error: Error) -> void:
	if _busy and request_id == _seed_request_id:
		_fail(error, "prolongation")


func _restore_after_committed_failure(committed: Dictionary, stage: String) -> void:
	var surface := float(committed.get("reserved_surface_volume_m3", 0.0))
	var channel := float(committed.get("reserved_channel_volume_m3", 0.0))
	var restore := store.accept_demotion(_cell, surface, channel)
	var error := ERR_CANT_ACQUIRE_RESOURCE
	if int(restore.get("error", FAILED)) != OK:
		error = int(restore.get("error", ERR_INVALID_DATA))
	_fail(error, stage + "_after_commit", false)


func _fail(error: Error, stage: String, rollback_coarse: bool = true) -> void:
	var failed_id := _request_id
	if _key != null and scheduler != null:
		scheduler.cancel_reserved(_key, "planet_promotion_" + stage)
	if rollback_coarse and store != null and _transaction_id >= 0:
		store.rollback_promotion(_transaction_id)
	_finish_request()
	promotion_failed.emit(failed_id, error, stage)


func _finish_request() -> void:
	if runtime != null:
		runtime.enabled = _runtime_was_enabled
	_busy = false
	_request_id = -1
	_transaction_id = -1
	_cell = -1
	_key = null
	_slot = -1
	_terrain_request_id = -1
	_seed_request_id = -1
	_seed_equivalent_depth_m = 0.0
	_represented_volume_m3 = 0.0
	_local_velocity = Vector2.ZERO


func _on_seeder_initialized() -> void:
	_initialized = true
	initialized.emit()


func _on_seeder_initialization_failed(error: Error) -> void:
	_initialized = false
	initialization_failed.emit(error)


func _disconnect_terrain() -> void:
	if terrain_bed != null:
		if terrain_bed.stage_recorded.is_connected(_on_terrain_stage_recorded):
			terrain_bed.stage_recorded.disconnect(_on_terrain_stage_recorded)
		if terrain_bed.stage_failed.is_connected(_on_terrain_stage_failed):
			terrain_bed.stage_failed.disconnect(_on_terrain_stage_failed)


func _clear_refs() -> void:
	store = null
	scheduler = null
	atlas = null
	connectivity = null
	identity_bridge = null
	terrain_bed = null
	runtime = null


func release() -> void:
	if _busy:
		_fail(ERR_CANT_ACQUIRE_RESOURCE, "bridge_release")
	_disconnect_terrain()
	if seeder != null and is_instance_valid(seeder):
		seeder.release()
		seeder.queue_free()
	seeder = null
	_initialized = false
	_clear_refs()
	released.emit()


func _exit_tree() -> void:
	release()
