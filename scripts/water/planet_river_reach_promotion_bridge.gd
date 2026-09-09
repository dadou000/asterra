class_name PlanetRiverReachPromotionBridge
extends Node
## Transactional bridge from one persistent 1D macro river reach to one newly
## allocated sparse 2D SWE river tile.
##
## Unlike flood promotion, this path reserves channel water only and reconstructs a
## narrow terrain-following corridor aligned with the generated downstream graph.
## The destination stays ALLOCATING/occupancy=0 through terrain staging and exact
## GPU corridor prolongation. The channel debit occurs only after GPU acknowledgement.

signal initialized
signal initialization_failed(error: Error)
signal promotion_started(request_id: int, transaction_id: int, cell: int, tile_id: int, slot: int)
signal promotion_completed(request_id: int, report: Dictionary)
signal promotion_failed(request_id: int, error: Error, stage: String)
signal released

var store: PlanetHydrologyRiverPromotionStore
var scheduler: SparseHydroScheduler
var atlas: SparseHydroAtlasGPU
var connectivity: SparseHydroConnectivityGPU
var identity_bridge: SparseHydroIdentityBridge
var terrain_bed: HydroTerrainBedGPU
var runtime: SparseHydrologyRuntime
var seeder: HydroRiverCorridorProlongationGPU

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
var _represented_volume_m3 := 0.0
var _corridor_center_cell := Vector2.ZERO
var _corridor_direction_cell := Vector2.RIGHT
var _half_width_m := 1.0
var _local_velocity := Vector2.ZERO
var _runtime_was_enabled := true


func initialize(p_store: PlanetHydrologyRiverPromotionStore,
		p_scheduler: SparseHydroScheduler, p_atlas: SparseHydroAtlasGPU,
		p_connectivity: SparseHydroConnectivityGPU,
		p_identity_bridge: SparseHydroIdentityBridge,
		p_terrain_bed: HydroTerrainBedGPU,
		p_runtime: SparseHydrologyRuntime = null) -> Error:
	if _initialized or seeder != null:
		return ERR_BUSY
	if p_store == null or not p_store.initialized or p_store.river_reaches == null \
			or p_scheduler == null or p_atlas == null or not p_atlas.initialized_ok() \
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

	seeder = HydroRiverCorridorProlongationGPU.new()
	seeder.name = "HydroRiverCorridorProlongationGPU"
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


func suggested_channel_volume_m3(cell: int) -> float:
	if not _initialized:
		return 0.0
	var tile_span := float(atlas.tile_resolution) * atlas.cell_size_m
	return store.suggested_channel_tile_volume_m3(cell, tile_span)


## Promote one local segment of a generated 1D river reach. requested_volume_m3 < 0
## selects the reach cross-section volume over one fine-tile span.
func promote_reach(cell: int, requested_volume_m3: float = -1.0,
		target_key: HydroTileKey = null) -> int:
	if not _initialized or _busy or cell < 0 or cell >= store.cell_count() \
			or store.river_reaches == null or not store.river_reaches.is_reach_cell(cell):
		return -1
	if runtime != null and (not runtime.initialized_ok() or runtime.busy()):
		return -1
	var key := target_key if target_key != null else tile_key_for_cell(cell)
	if key == null or scheduler.pool.contains(key):
		return -1
	var geometry := _corridor_geometry(cell, key)
	if geometry.is_empty():
		return -1

	var requested := requested_volume_m3
	if requested < 0.0:
		requested = suggested_channel_volume_m3(cell)
	if not is_finite(requested) or requested <= 0.0:
		return -1
	var plan := seeder.plan_volume(requested)
	if int(plan.get("error", FAILED)) != OK:
		return -1
	var represented := float(plan.get("represented_volume_m3", 0.0))
	var prepared := store.prepare_channel_promotion(cell, represented)
	if int(prepared.get("error", FAILED)) != OK:
		return -1

	var slot := scheduler.reserve(key, 0, "planet_river_reach_promotion")
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
	_represented_volume_m3 = represented
	_corridor_center_cell = geometry["center_cell"]
	_corridor_direction_cell = geometry["direction_cell"]
	_half_width_m = float(geometry["half_width_m"])
	_local_velocity = geometry["local_velocity"]
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


func _corridor_geometry(cell: int, key: HydroTileKey) -> Dictionary:
	var r := store.receiver[cell]
	if r < 0 or r >= store.cell_count() or r == cell:
		return {}
	var source_dir := store.grid.cell_dir(cell).normalized()
	var receiver_dir := store.grid.cell_dir(r).normalized()
	var flow_tangent := receiver_dir - source_dir * receiver_dir.dot(source_dir)
	if flow_tangent.length_squared() <= 1.0e-12:
		return {}
	flow_tangent = flow_tangent.normalized()

	var mapped := CubeSphere.dir_to_face_uv(source_dir)
	if mapped.size() < 3 or int(mapped[0]) != key.face:
		return {}
	var side := 1 << key.level
	var gx := clampf((float(mapped[1]) + 1.0) * 0.5 * float(side),
		0.0, float(side) - 1.0e-9)
	var gy := clampf((float(mapped[2]) + 1.0) * 0.5 * float(side),
		0.0, float(side) - 1.0e-9)
	var center := Vector2(
		(gx - float(key.x)) * float(atlas.tile_resolution),
		(gy - float(key.y)) * float(atlas.tile_resolution))

	var reach := store.river_reaches.reach_state(cell, store.channel_storage_m3[cell])
	var depth := maxf(float(reach.get("depth_m", 0.0)), 0.0)
	var cross_area := store.river_reaches.cross_section_area(cell, depth)
	var q_capacity := store.river_reaches.discharge_for_depth(cell, depth)
	var speed := q_capacity / maxf(cross_area, 1.0e-9)
	var velocity_world := flow_tangent * maxf(speed, 0.0)
	var local_velocity := _world_velocity_to_face_uv(source_dir, velocity_world)
	var local_direction := _world_velocity_to_face_uv(source_dir, flow_tangent)
	if local_direction.length_squared() <= 1.0e-12:
		return {}
	local_direction = local_direction.normalized()

	var generated_width := maxf(float(store.fields.river_width[cell]), 0.0)
	var half_width := maxf(generated_width * 0.5, atlas.cell_size_m * 0.75)
	return {
		"center_cell": center,
		"direction_cell": local_direction,
		"local_velocity": local_velocity,
		"half_width_m": half_width,
		"hydraulic_speed_mps": speed,
		"receiver": r,
	}


func _world_velocity_to_face_uv(direction: Vector3, velocity_world: Vector3) -> Vector2:
	if velocity_world.length_squared() <= 1.0e-20:
		return Vector2.ZERO
	var d := direction.normalized()
	var mapped := CubeSphere.dir_to_face_uv(d)
	if mapped.size() < 3:
		return Vector2.ZERO
	var face := int(mapped[0])
	var u := float(mapped[1])
	var v := float(mapped[2])
	const EPS := 1.0e-4
	var du := (CubeSphere.face_uv_to_dir(face, clampf(u + EPS, -1.0, 1.0), v)
		- CubeSphere.face_uv_to_dir(face, clampf(u - EPS, -1.0, 1.0), v)).normalized()
	var dv := (CubeSphere.face_uv_to_dir(face, u, clampf(v + EPS, -1.0, 1.0))
		- CubeSphere.face_uv_to_dir(face, u, clampf(v - EPS, -1.0, 1.0))).normalized()
	var tangent_velocity := velocity_world - d * velocity_world.dot(d)
	return Vector2(tangent_velocity.dot(du), tangent_velocity.dot(dv))


func _on_terrain_stage_recorded(request_id: int, tile_id: int, slot: int) -> void:
	if not _busy or request_id != _terrain_request_id or _key == null \
			or tile_id != _key.packed() or slot != _slot:
		return
	_seed_request_id = seeder.seed_reserved(_slot, _represented_volume_m3,
		_corridor_center_cell, _corridor_direction_cell, _half_width_m, _local_velocity)
	if _seed_request_id < 0:
		_fail(ERR_BUSY, "corridor_seed_submit")


func _on_terrain_stage_failed(request_id: int, error: Error) -> void:
	if _busy and request_id == _terrain_request_id:
		_fail(error, "terrain_stage")


func _on_seed_recorded(request_id: int, slot: int,
		represented_volume_m3: float) -> void:
	if not _busy or request_id != _seed_request_id or slot != _slot:
		return
	var tolerance := maxf(1.0e-7, absf(_represented_volume_m3) * 1.0e-7)
	if absf(represented_volume_m3 - _represented_volume_m3) > tolerance:
		_fail(ERR_INVALID_DATA, "corridor_volume_ack")
		return

	var committed := store.commit_promotion(_transaction_id)
	if int(committed.get("error", FAILED)) != OK:
		_fail(int(committed.get("error", ERR_INVALID_DATA)), "coarse_channel_commit")
		return

	var activated_slot := scheduler.activate_reserved(_key, "planet_river_corridor_seeded")
	if activated_slot != _slot:
		_restore_after_committed_failure(committed, "activate")
		return
	var conn_error := connectivity.sync_pool(scheduler.pool)
	if conn_error != OK:
		scheduler.force_release(_key, "planet_river_connectivity_failed")
		_restore_after_committed_failure(committed, "connectivity")
		return

	var reach := store.river_reaches.reach_state(_cell, store.channel_storage_m3[_cell])
	var report := committed.duplicate(true)
	report["request_id"] = _request_id
	report["tile_id"] = _key.packed()
	report["slot"] = _slot
	report["seed_strategy"] = "terrain_aligned_river_corridor"
	report["represented_volume_m3"] = _represented_volume_m3
	report["corridor_center_cell"] = _corridor_center_cell
	report["corridor_direction_cell"] = _corridor_direction_cell
	report["corridor_half_width_m"] = _half_width_m
	report["local_velocity_mps"] = _local_velocity
	report["remaining_reach_storage_m3"] = store.channel_storage_m3[_cell]
	report["remaining_reach_state"] = reach
	var completed_id := _request_id
	_finish_request()
	promotion_completed.emit(completed_id, report)


func _on_seed_failed(request_id: int, error: Error) -> void:
	if _busy and request_id == _seed_request_id:
		_fail(error, "corridor_seed")


func _restore_after_committed_failure(committed: Dictionary, stage: String) -> void:
	var channel := float(committed.get("reserved_channel_volume_m3", 0.0))
	var restore := store.accept_demotion(_cell, 0.0, channel)
	var error := ERR_CANT_ACQUIRE_RESOURCE
	if int(restore.get("error", FAILED)) != OK:
		error = int(restore.get("error", ERR_INVALID_DATA))
	_fail(error, stage + "_after_commit", false)


func _fail(error: Error, stage: String, rollback_coarse: bool = true) -> void:
	var failed_id := _request_id
	if _key != null and scheduler != null:
		scheduler.cancel_reserved(_key, "planet_river_promotion_" + stage)
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
	_represented_volume_m3 = 0.0
	_corridor_center_cell = Vector2.ZERO
	_corridor_direction_cell = Vector2.RIGHT
	_half_width_m = 1.0
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
