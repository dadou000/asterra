class_name PlanetRiverComponentPromotionBridge
extends Node
## Atomic coarse river graph -> connected sparse 2D component promotion.
##
## Every tile remains hidden while terrain and water are staged. The complete graph
## is batch-published with the runtime paused, then one multi-reach coarse ownership
## transaction commits and the component is registered before simulation resumes.

signal initialized
signal initialization_failed(error: Error)
signal promotion_completed(request_id: int, report: Dictionary)
signal promotion_failed(request_id: int, error: Error, stage: String)
signal released

var store: PlanetHydrologyRiverClusterStore
var scheduler: SparseHydroScheduler
var atlas: SparseHydroAtlasGPU
var connectivity: SparseHydroConnectivityGPU
var identity_bridge: SparseHydroIdentityBridge
var terrain_bed: HydroTerrainBedGPU
var runtime: SparseHydrologyRuntime
var line_seeder: HydroRiverCorridorProlongationGPU
var junction_seeder: HydroRiverJunctionProlongationGPU
var registration_callback := Callable()

var _initialized := false
var _line_ready := false
var _junction_ready := false
var _busy := false
var _next_request_id := 1
var _request_id := -1
var _transaction_id := -1
var _plan: Dictionary = {}
var _flat_members: Array[Dictionary] = []
var _stage_to_flat: Dictionary = {}
var _line_seed_to_flat: Dictionary = {}
var _junction_seed_to_flat: Dictionary = {}
var _staged := 0
var _seeded := 0
var _coarse_committed := false
var _published := false
var _runtime_was_enabled := true


func set_registration_callback(callback: Callable) -> Error:
	if _busy:
		return ERR_BUSY
	registration_callback = callback
	return OK


func initialize(p_store: PlanetHydrologyRiverClusterStore,
		p_scheduler: SparseHydroScheduler, p_atlas: SparseHydroAtlasGPU,
		p_connectivity: SparseHydroConnectivityGPU,
		p_identity_bridge: SparseHydroIdentityBridge,
		p_terrain_bed: HydroTerrainBedGPU,
		p_runtime: SparseHydrologyRuntime) -> Error:
	if _initialized or line_seeder != null or junction_seeder != null:
		return ERR_BUSY
	if p_store == null or not p_store.initialized or p_scheduler == null \
			or p_scheduler.pool == null or p_atlas == null or not p_atlas.initialized_ok() \
			or p_connectivity == null or not p_connectivity.initialized_ok() \
			or p_identity_bridge == null or not p_identity_bridge.is_bound() \
			or p_terrain_bed == null or not p_terrain_bed.initialized_ok() \
			or p_runtime == null or not p_runtime.initialized_ok():
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

	line_seeder = HydroRiverCorridorProlongationGPU.new()
	line_seeder.name = "HydroRiverComponentLineProlongationGPU"
	add_child(line_seeder)
	line_seeder.initialized.connect(func():
		_line_ready = true
		_try_finish_init())
	line_seeder.initialization_failed.connect(_on_seeder_initialization_failed)
	line_seeder.seed_recorded.connect(_on_line_seed_recorded)
	line_seeder.seed_failed.connect(_on_line_seed_failed)

	junction_seeder = HydroRiverJunctionProlongationGPU.new()
	junction_seeder.name = "HydroRiverComponentJunctionProlongationGPU"
	add_child(junction_seeder)
	junction_seeder.initialized.connect(func():
		_junction_ready = true
		_try_finish_init())
	junction_seeder.initialization_failed.connect(_on_seeder_initialization_failed)
	junction_seeder.seed_recorded.connect(_on_junction_seed_recorded)
	junction_seeder.seed_failed.connect(_on_junction_seed_failed)

	var line_err := line_seeder.initialize(atlas)
	if line_err != OK:
		_cleanup_initialization()
		return line_err
	var junction_err := junction_seeder.initialize(atlas)
	if junction_err != OK:
		_cleanup_initialization()
		return junction_err
	return OK


func initialized_ok() -> bool:
	return _initialized


func busy() -> bool:
	return _busy


func plan_component(cells: PackedInt32Array) -> Dictionary:
	if not _initialized:
		return {"error": ERR_UNCONFIGURED}
	return HydroRiverComponentPlanner.plan(store, atlas, cells)


func planned_member_count(cells: PackedInt32Array) -> int:
	var planned := plan_component(cells)
	return int(planned.get("member_count", 0)) if int(planned.get("error", FAILED)) == OK else 0


func promote_component(cells: PackedInt32Array) -> int:
	if not _initialized or _busy or runtime.busy() \
			or store.pending_ownership_transaction_count() > 0:
		return -1
	var planned := HydroRiverComponentPlanner.plan(store, atlas, cells)
	if int(planned.get("error", FAILED)) != OK:
		return -1
	var member_count := int(planned.get("member_count", 0))
	if member_count <= 0 or scheduler.pool.free_count() < member_count:
		return -1

	var reach_plans_value: Variant = planned.get("reach_plans", null)
	if not (reach_plans_value is Array):
		return -1
	var reach_plans: Array = reach_plans_value
	var reach_volumes: Dictionary = {}
	var flat: Array[Dictionary] = []
	for reach_value: Variant in reach_plans:
		if not (reach_value is Dictionary):
			return -1
		var reach := (reach_value as Dictionary).duplicate(true)
		var cell := int(reach.get("cell", -1))
		var available := store.available_channel_promotion_volume_m3(cell)
		var members_value: Variant = reach.get("members", null)
		if available <= 0.0 or not (members_value is Array) or (members_value as Array).is_empty():
			return -1
		var input_members: Array = members_value
		var each_requested := available / float(input_members.size())
		var represented_total := 0.0
		var normalized: Array[Dictionary] = []
		for i in input_members.size():
			var member := (input_members[i] as Dictionary).duplicate(true)
			var volume_plan: Dictionary
			if bool(member.get("junction", false)):
				volume_plan = junction_seeder.plan_volume(each_requested)
			else:
				volume_plan = line_seeder.plan_volume(each_requested)
			if int(volume_plan.get("error", FAILED)) != OK:
				return -1
			var represented := float(volume_plan.get("represented_volume_m3", 0.0))
			if represented <= 0.0:
				return -1
			member["represented_volume_m3"] = represented
			member["cell"] = cell
			member["reach_member_index"] = i
			represented_total += represented
			normalized.append(member)
			flat.append(member)
		reach["members"] = normalized
		reach["represented_volume_m3"] = represented_total
		reach_volumes[cell] = represented_total
		# Replace the plan entry in-place for final report construction.
		for index in reach_plans.size():
			if int((reach_plans[index] as Dictionary).get("cell", -1)) == cell:
				reach_plans[index] = reach
				break

	var prepared := store.prepare_component_channel_promotion(reach_volumes)
	if int(prepared.get("error", FAILED)) != OK:
		return -1
	var tx := int(prepared.get("transaction_id", -1))
	if tx < 0:
		return -1

	# Reserve all sparse identities before making the request visible as busy.
	for i in flat.size():
		var key := flat[i].get("key") as HydroTileKey
		if key == null or scheduler.pool.contains(key):
			_cancel_reserved(flat)
			store.rollback_component_channel_promotion(tx)
			return -1
		var slot := scheduler.reserve(key, 0, "planet_river_component_promotion")
		if slot < 0:
			_cancel_reserved(flat)
			store.rollback_component_channel_promotion(tx)
			return -1
		flat[i]["slot"] = slot
		var cell := int(flat[i]["cell"])
		var member_index := int(flat[i]["reach_member_index"])
		for r in reach_plans.size():
			if int((reach_plans[r] as Dictionary).get("cell", -1)) == cell:
				var reach := reach_plans[r] as Dictionary
				var members := reach["members"] as Array
				(members[member_index] as Dictionary)["slot"] = slot
				reach["members"] = members
				reach_plans[r] = reach
				break

	planned["reach_plans"] = reach_plans
	_busy = true
	_request_id = _next_request_id
	_next_request_id += 1
	_transaction_id = tx
	_plan = planned
	_flat_members = flat
	_stage_to_flat.clear()
	_line_seed_to_flat.clear()
	_junction_seed_to_flat.clear()
	_staged = 0
	_seeded = 0
	_coarse_committed = false
	_published = false
	_runtime_was_enabled = runtime.enabled
	runtime.enabled = false

	for i in _flat_members.size():
		var member := _flat_members[i]
		var stage := terrain_bed.stage_reserved_tile(member["key"] as HydroTileKey,
			int(member["slot"]))
		if not bool(stage.get("queued", false)) or int(stage.get("error", FAILED)) != OK:
			_fail(int(stage.get("error", ERR_INVALID_DATA)), "terrain_stage_submit")
			return -1
		_stage_to_flat[int(stage.get("request_id", -1))] = i
	return _request_id


func _on_terrain_stage_recorded(request_id: int, tile_id: int, slot: int) -> void:
	if not _busy or not _stage_to_flat.has(request_id):
		return
	var index := int(_stage_to_flat[request_id])
	_stage_to_flat.erase(request_id)
	var member := _flat_members[index]
	if int(member.get("tile_id", -1)) != tile_id or int(member.get("slot", -1)) != slot:
		_fail(ERR_INVALID_DATA, "terrain_identity")
		return
	_staged += 1
	if _staged == _flat_members.size():
		_seed_all_members()


func _on_terrain_stage_failed(request_id: int, error: Error) -> void:
	if _busy and _stage_to_flat.has(request_id):
		_fail(error, "terrain_stage")


func _seed_all_members() -> void:
	for i in _flat_members.size():
		var member := _flat_members[i]
		var seed_id := -1
		if bool(member.get("junction", false)):
			var segments_value: Variant = member.get("junction_segments", null)
			if not (segments_value is Array):
				_fail(ERR_INVALID_DATA, "junction_segments")
				return
			var segments: Array[Dictionary] = []
			for value: Variant in segments_value:
				if value is Dictionary:
					segments.append((value as Dictionary).duplicate(true))
			seed_id = junction_seeder.seed_reserved(int(member["slot"]),
				float(member["represented_volume_m3"]), segments)
			if seed_id >= 0:
				_junction_seed_to_flat[seed_id] = i
		else:
			seed_id = line_seeder.seed_reserved(int(member["slot"]),
				float(member["represented_volume_m3"]),
				member["center_cell"] as Vector2,
				member["direction_cell"] as Vector2,
				float(member["half_width_m"]), member["local_velocity"] as Vector2)
			if seed_id >= 0:
				_line_seed_to_flat[seed_id] = i
		if seed_id < 0:
			_fail(ERR_BUSY, "component_seed_submit")
			return


func _on_line_seed_recorded(request_id: int, slot: int, represented_volume_m3: float) -> void:
	if _busy and _line_seed_to_flat.has(request_id):
		var index := int(_line_seed_to_flat[request_id])
		_line_seed_to_flat.erase(request_id)
		_accept_seed(index, slot, represented_volume_m3)


func _on_junction_seed_recorded(request_id: int, slot: int,
		represented_volume_m3: float) -> void:
	if _busy and _junction_seed_to_flat.has(request_id):
		var index := int(_junction_seed_to_flat[request_id])
		_junction_seed_to_flat.erase(request_id)
		_accept_seed(index, slot, represented_volume_m3)


func _accept_seed(index: int, slot: int, represented_volume_m3: float) -> void:
	if index < 0 or index >= _flat_members.size():
		_fail(ERR_INVALID_DATA, "seed_index")
		return
	var member := _flat_members[index]
	var expected := float(member.get("represented_volume_m3", 0.0))
	var tolerance := maxf(1.0e-6, expected * 1.0e-6)
	if int(member.get("slot", -1)) != slot or absf(expected - represented_volume_m3) > tolerance:
		_fail(ERR_INVALID_DATA, "seed_identity")
		return
	_seeded += 1
	if _seeded == _flat_members.size():
		_publish_component()


func _on_line_seed_failed(request_id: int, error: Error) -> void:
	if _busy and _line_seed_to_flat.has(request_id):
		_fail(error, "line_seed")


func _on_junction_seed_failed(request_id: int, error: Error) -> void:
	if _busy and _junction_seed_to_flat.has(request_id):
		_fail(error, "junction_seed")


func _publish_component() -> void:
	var keys: Array[HydroTileKey] = []
	for member in _flat_members:
		keys.append(member["key"] as HydroTileKey)
	var slots := HydroSchedulerBatchOps.activate_reserved(scheduler, keys,
		"river_component_publish")
	if slots.size() != keys.size():
		_fail(ERR_CANT_ACQUIRE_RESOURCE, "component_publish")
		return
	_published = true
	for i in slots.size():
		if slots[i] != int(_flat_members[i]["slot"]):
			_fail(ERR_INVALID_DATA, "component_slot_changed")
			return
	var conn_error := connectivity.sync_pool(scheduler.pool)
	if conn_error != OK:
		_fail(conn_error, "component_connectivity")
		return

	var committed := store.commit_component_channel_promotion(_transaction_id)
	if int(committed.get("error", FAILED)) != OK:
		_fail(int(committed.get("error", ERR_INVALID_DATA)), "coarse_component_commit")
		return
	_transaction_id = -1
	_coarse_committed = true

	var report := _published_report()
	if registration_callback.is_valid():
		var value: Variant = registration_callback.call(report)
		var error := int(value) if value is int else ERR_INVALID_DATA
		if error != OK:
			_fail(error, "component_registration")
			return
	_complete(report)


func _published_report() -> Dictionary:
	var reach_reports: Array[Dictionary] = []
	var slot_by_tile: Dictionary = {}
	for member in _flat_members:
		slot_by_tile[int(member["tile_id"])] = int(member["slot"])
	var reach_plans := _plan.get("reach_plans", []) as Array
	for value: Variant in reach_plans:
		var reach := (value as Dictionary).duplicate(true)
		var members_value: Variant = reach.get("members", [])
		var published_members: Array[Dictionary] = []
		if members_value is Array:
			for item: Variant in members_value:
				var member := (item as Dictionary).duplicate(true)
				member.erase("key")
				member.erase("cell")
				member.erase("reach_member_index")
				published_members.append(member)
		reach["members"] = published_members
		reach["member_count"] = published_members.size()
		reach["representation"] = "sparse_2d_river_cluster"
		reach_reports.append(reach)

	var junctions: Array[Dictionary] = []
	var junction_values := _plan.get("junctions", []) as Array
	for value: Variant in junction_values:
		var junction := (value as Dictionary).duplicate(true)
		var tile_id := int(junction.get("tile_id", -1))
		junction["slot"] = int(slot_by_tile.get(tile_id, -1))
		junction["verified"] = int(junction["slot"]) >= 0
		junctions.append(junction)
	return {
		"cells": _plan.get("cells", PackedInt32Array()),
		"reach_reports": reach_reports,
		"reach_count": reach_reports.size(),
		"member_count": _flat_members.size(),
		"junctions": junctions,
		"junction_count": junctions.size(),
		"upstream_mouth_cells": _plan.get("upstream_mouth_cells", PackedInt32Array()),
		"upstream_mouth_count": int(_plan.get("upstream_mouth_count", 0)),
		"downstream_outlet_cell": int(_plan.get("downstream_outlet_cell", -1)),
		"fine_junction_verified": true,
		"physical_topology_verified": true,
		"published_atomically": true,
		"representation": "sparse_2d_river_component",
	}


func _complete(report: Dictionary) -> void:
	var completed := _request_id
	var out := report.duplicate(true)
	out["request_id"] = completed
	_restore_runtime()
	_clear_request()
	promotion_completed.emit(completed, out)


func _fail(error: Error, stage: String) -> void:
	var failed := _request_id
	if not _coarse_committed and _transaction_id >= 0:
		store.rollback_component_channel_promotion(_transaction_id)
		_transaction_id = -1
	if not _coarse_committed:
		_unpublish_or_cancel_members()
		_restore_runtime()
	else:
		# Once coarse ownership committed, an unknown registration/publication state
		# must fail closed. Continuing either owner could duplicate or lose water.
		runtime.enabled = false
	PersistentHydrologySystem.enabled = false
	_clear_request()
	promotion_failed.emit(failed, error, stage)


func _unpublish_or_cancel_members() -> void:
	if _published:
		var keys: Array[HydroTileKey] = []
		for member in _flat_members:
			var key := member.get("key") as HydroTileKey
			if key != null and scheduler.pool.contains(key):
				keys.append(key)
		if not keys.is_empty():
			HydroSchedulerBatchOps.force_release(scheduler, keys,
				"river_component_promotion_rollback")
			connectivity.sync_pool(scheduler.pool)
	else:
		_cancel_reserved(_flat_members)


func _cancel_reserved(members: Array[Dictionary]) -> void:
	for member in members:
		var key := member.get("key") as HydroTileKey
		if key != null and scheduler.pool.contains(key):
			var rec := scheduler.pool.record(key)
			if int(rec.get("state", HydroTilePool.TileState.ACTIVE)) \
					== HydroTilePool.TileState.ALLOCATING:
				scheduler.cancel_reserved(key, "river_component_rollback")


func _try_finish_init() -> void:
	if _line_ready and _junction_ready and not _initialized:
		_initialized = true
		initialized.emit()


func _on_seeder_initialization_failed(error: Error) -> void:
	if not _initialized:
		initialization_failed.emit(error)


func _restore_runtime() -> void:
	if runtime != null:
		runtime.enabled = _runtime_was_enabled
		if runtime.enabled:
			runtime.advance_time(0.0)


func _clear_request() -> void:
	_busy = false
	_request_id = -1
	_transaction_id = -1
	_plan = {}
	_flat_members = []
	_stage_to_flat.clear()
	_line_seed_to_flat.clear()
	_junction_seed_to_flat.clear()
	_staged = 0
	_seeded = 0
	_coarse_committed = false
	_published = false


func _cleanup_initialization() -> void:
	_disconnect_inputs()
	if line_seeder != null and is_instance_valid(line_seeder):
		line_seeder.release(); line_seeder.queue_free()
	if junction_seeder != null and is_instance_valid(junction_seeder):
		junction_seeder.release(); junction_seeder.queue_free()
	line_seeder = null
	junction_seeder = null
	_clear_refs()


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
	registration_callback = Callable()


func release() -> void:
	if _busy:
		_fail(ERR_CANT_ACQUIRE_RESOURCE, "release")
	_disconnect_inputs()
	if line_seeder != null and is_instance_valid(line_seeder):
		line_seeder.release(); line_seeder.queue_free()
	if junction_seeder != null and is_instance_valid(junction_seeder):
		junction_seeder.release(); junction_seeder.queue_free()
	line_seeder = null
	junction_seeder = null
	_initialized = false
	_line_ready = false
	_junction_ready = false
	_clear_refs()
	released.emit()


func _exit_tree() -> void:
	release()
