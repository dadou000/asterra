class_name PlanetRiverReachClusterPromotionBridge
extends Node
## Transactional one-reach -> contiguous sparse river-cluster promotion.
##
## One coarse ownership transaction covers every fine member. All members remain
## ALLOCATING/occupancy=0 through terrain staging and corridor seeding. The coarse
## channel debit commits once, then the full chain is published and connectivity is
## rebuilt once so internal fine<->fine edges appear atomically.

signal initialized
signal initialization_failed(error: Error)
signal promotion_completed(request_id: int, report: Dictionary)
signal promotion_failed(request_id: int, error: Error, stage: String)
signal released

var default_cluster_tiles := 3
var max_cluster_tiles := 8

var store: PlanetHydrologyRiverClusterStore
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
var _members: Array[Dictionary] = []
var _stage_to_index: Dictionary = {}
var _seed_to_index: Dictionary = {}
var _staged := 0
var _seeded := 0
var _represented_total_m3 := 0.0
var _runtime_was_enabled := true


func initialize(p_store: PlanetHydrologyRiverClusterStore,
		p_scheduler: SparseHydroScheduler, p_atlas: SparseHydroAtlasGPU,
		p_connectivity: SparseHydroConnectivityGPU,
		p_identity_bridge: SparseHydroIdentityBridge,
		p_terrain_bed: HydroTerrainBedGPU,
		p_runtime: SparseHydrologyRuntime = null) -> Error:
	if _initialized or seeder != null:
		return ERR_BUSY
	if p_store == null or not p_store.initialized or p_scheduler == null \
			or p_scheduler.pool == null or p_atlas == null or not p_atlas.initialized_ok() \
			or p_connectivity == null or not p_connectivity.initialized_ok() \
			or p_identity_bridge == null or not p_identity_bridge.is_bound() \
			or p_terrain_bed == null or not p_terrain_bed.initialized_ok():
		return ERR_UNCONFIGURED
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
	seeder.name = "HydroRiverClusterCorridorProlongationGPU"
	add_child(seeder)
	seeder.initialized.connect(func():
		_initialized = true
		initialized.emit())
	seeder.initialization_failed.connect(func(error: Error):
		initialization_failed.emit(error))
	seeder.seed_recorded.connect(_on_seed_recorded)
	seeder.seed_failed.connect(_on_seed_failed)
	var err := seeder.initialize(atlas)
	if err != OK:
		_disconnect_inputs()
		seeder.queue_free()
		seeder = null
		_clear_refs()
		return err
	return OK


func initialized_ok() -> bool:
	return _initialized


func busy() -> bool:
	return _busy


func suggested_cluster_volume_m3(cell: int, tile_count: int = -1) -> float:
	if not _initialized or cell < 0 or cell >= store.cell_count():
		return 0.0
	var count := clampi(default_cluster_tiles if tile_count < 0 else tile_count,
		1, maxi(max_cluster_tiles, 1))
	var span := float(count * atlas.tile_resolution) * atlas.cell_size_m
	return store.suggested_channel_tile_volume_m3(cell, span)


func promote_cluster(cell: int, tile_count: int = -1,
		requested_volume_m3: float = -1.0) -> int:
	if not _initialized or _busy or cell < 0 or cell >= store.cell_count() \
			or store.is_refined_reach(cell):
		return -1
	if runtime != null and (not runtime.initialized_ok() or runtime.busy()):
		return -1
	var count := clampi(default_cluster_tiles if tile_count < 0 else tile_count,
		1, maxi(max_cluster_tiles, 1))
	var cluster := HydroRiverClusterPlanner.plan(store, atlas, cell, count)
	if int(cluster.get("error", FAILED)) != OK:
		return -1
	var planned: Array = cluster.get("members", [])
	if planned.is_empty():
		return -1

	var requested := requested_volume_m3
	if requested < 0.0:
		requested = store.suggested_channel_tile_volume_m3(cell,
			float(cluster.get("represented_span_m", 0.0)))
	if not is_finite(requested) or requested <= 0.0:
		return -1

	# Quantize every member independently before reserving coarse water, then reserve
	# exactly the sum that the GPU members can represent.
	var each_requested := requested / float(planned.size())
	var members: Array[Dictionary] = []
	var represented_total := 0.0
	for i in planned.size():
		var plan := seeder.plan_volume(each_requested)
		if int(plan.get("error", FAILED)) != OK:
			return -1
		var member := (planned[i] as Dictionary).duplicate(true)
		member["represented_volume_m3"] = float(plan["represented_volume_m3"])
		represented_total += float(member["represented_volume_m3"])
		members.append(member)
	if represented_total <= 0.0:
		return -1
	var prepared := store.prepare_channel_promotion(cell, represented_total)
	if int(prepared.get("error", FAILED)) != OK:
		return -1

	# Reserve the entire chain before any GPU mutation. Failure is therefore a clean
	# rollback with no published sparse ownership.
	for i in members.size():
		var key := members[i].get("key") as HydroTileKey
		if key == null or scheduler.pool.contains(key):
			_cancel_reserved_members(members)
			store.rollback_promotion(int(prepared["transaction_id"]))
			return -1
		var slot := scheduler.reserve(key, 0, "planet_river_cluster_promotion")
		if slot < 0:
			_cancel_reserved_members(members)
			store.rollback_promotion(int(prepared["transaction_id"]))
			return -1
		members[i]["slot"] = slot

	_busy = true
	_request_id = _next_request_id
	_next_request_id += 1
	_transaction_id = int(prepared["transaction_id"])
	_cell = cell
	_members = members
	_represented_total_m3 = represented_total
	_stage_to_index.clear()
	_seed_to_index.clear()
	_staged = 0
	_seeded = 0
	if runtime != null:
		_runtime_was_enabled = runtime.enabled
		runtime.enabled = false

	for i in _members.size():
		var member := _members[i]
		var stage := terrain_bed.stage_reserved_tile(member["key"] as HydroTileKey,
			int(member["slot"]))
		if not bool(stage.get("queued", false)) or int(stage.get("error", FAILED)) != OK:
			_fail(int(stage.get("error", ERR_INVALID_DATA)), "terrain_stage_submit")
			return -1
		_stage_to_index[int(stage.get("request_id", -1))] = i
	return _request_id


func _on_terrain_stage_recorded(request_id: int, tile_id: int, slot: int) -> void:
	if not _busy or not _stage_to_index.has(request_id):
		return
	var index := int(_stage_to_index[request_id])
	_stage_to_index.erase(request_id)
	var member := _members[index]
	if int(member.get("tile_id", -1)) != tile_id or int(member.get("slot", -1)) != slot:
		_fail(ERR_INVALID_DATA, "terrain_identity")
		return
	_staged += 1
	if _staged != _members.size():
		return
	# All beds exist before any member receives water.
	for i in _members.size():
		var m := _members[i]
		var seed_id := seeder.seed_reserved(int(m["slot"]),
			float(m["represented_volume_m3"]), m["center_cell"] as Vector2,
			m["direction_cell"] as Vector2, float(m["half_width_m"]),
			m["local_velocity"] as Vector2)
		if seed_id < 0:
			_fail(ERR_BUSY, "cluster_seed_submit")
			return
		_seed_to_index[seed_id] = i


func _on_terrain_stage_failed(request_id: int, error: Error) -> void:
	if _busy and _stage_to_index.has(request_id):
		_fail(error, "terrain_stage")


func _on_seed_recorded(request_id: int, slot: int, represented_volume_m3: float) -> void:
	if not _busy or not _seed_to_index.has(request_id):
		return
	var index := int(_seed_to_index[request_id])
	_seed_to_index.erase(request_id)
	var member := _members[index]
	var tol := maxf(1.0e-6, float(member["represented_volume_m3"]) * 1.0e-6)
	if int(member["slot"]) != slot \
			or absf(float(member["represented_volume_m3"]) - represented_volume_m3) > tol:
		_fail(ERR_INVALID_DATA, "seed_identity")
		return
	_seeded += 1
	if _seeded == _members.size():
		_commit_and_publish()


func _on_seed_failed(request_id: int, error: Error) -> void:
	if _busy and _seed_to_index.has(request_id):
		_fail(error, "cluster_seed")


func _commit_and_publish() -> void:
	var committed := store.commit_promotion(_transaction_id)
	if int(committed.get("error", FAILED)) != OK:
		_fail(int(committed.get("error", ERR_INVALID_DATA)), "coarse_commit")
		return
	_transaction_id = -1
	var published_members: Array[Dictionary] = []
	for i in _members.size():
		var member := _members[i]
		var key := member["key"] as HydroTileKey
		var slot := scheduler.activate_reserved(key, "river_cluster_publish")
		if slot != int(member["slot"]):
			# Coarse debit already committed and GPU water exists. Preserve the entire
			# generation fail-closed; never cancel a subset and lose its parcel.
			_fail(ERR_CANT_ACQUIRE_RESOURCE, "cluster_publish", false)
			return
		var out := member.duplicate(true)
		out.erase("key")
		published_members.append(out)
	var conn_error := connectivity.sync_pool(scheduler.pool)
	if conn_error != OK:
		_fail(conn_error, "cluster_connectivity", false)
		return
	var report := {
		"cell": _cell,
		"members": published_members,
		"member_count": published_members.size(),
		"tile_id": int(published_members[0]["tile_id"]),
		"slot": int(published_members[0]["slot"]),
		"represented_volume_m3": _represented_total_m3,
		"represented_span_m": float(published_members.size() * atlas.tile_resolution) \
			* atlas.cell_size_m,
		"representation": "sparse_2d_river_cluster",
	}
	_complete(report)


func _complete(report: Dictionary) -> void:
	var completed := _request_id
	var published := report.duplicate(true)
	published["request_id"] = completed
	_restore_runtime()
	_clear_request()
	promotion_completed.emit(completed, published)


func _fail(error: Error, stage: String, rollback_coarse: bool = true) -> void:
	var failed := _request_id
	if rollback_coarse and _transaction_id >= 0:
		store.rollback_promotion(_transaction_id)
		_cancel_reserved_members(_members)
		_restore_runtime()
	else:
		# Ownership already left coarse or a subset may be published. Freeze sparse
		# advancement rather than silently duplicating/removing water.
		if runtime != null:
			runtime.enabled = false
	_clear_request()
	promotion_failed.emit(failed, error, stage)


func _cancel_reserved_members(members: Array[Dictionary]) -> void:
	for member in members:
		var key := member.get("key") as HydroTileKey
		if key != null and scheduler.pool.contains(key):
			var rec := scheduler.pool.record(key)
			if int(rec.get("state", HydroTilePool.TileState.ACTIVE)) \
					== HydroTilePool.TileState.ALLOCATING:
				scheduler.cancel_reserved(key, "river_cluster_rollback")


func _restore_runtime() -> void:
	if runtime != null:
		runtime.enabled = _runtime_was_enabled
		if runtime.enabled:
			runtime.advance_time(0.0)


func _clear_request() -> void:
	_busy = false
	_request_id = -1
	_transaction_id = -1
	_cell = -1
	_members = []
	_stage_to_index.clear()
	_seed_to_index.clear()
	_staged = 0
	_seeded = 0
	_represented_total_m3 = 0.0


func _disconnect_inputs() -> void:
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
		_fail(ERR_CANT_ACQUIRE_RESOURCE, "release", _transaction_id >= 0)
	_disconnect_inputs()
	if seeder != null and is_instance_valid(seeder):
		seeder.release()
		seeder.queue_free()
	seeder = null
	_initialized = false
	_clear_refs()
	released.emit()


func _exit_tree() -> void:
	release()
