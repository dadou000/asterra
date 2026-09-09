extends Node
## Renderer-mode gate for one coarse reach <-> three contiguous sparse river tiles.

const TEST_RES := 2
const TEST_RADIUS_M := 1000.0
const BASELINE_Q := 10.0
const TILE_RES := 8
const TILE_DX := 2.0
const CLUSTER_TILES := 3
const TRANSFER_M3 := 36.0
const ABS_TOL := 7.0e-3
const TIMEOUT_FRAMES := 3000

var _store: PlanetHydrologyRiverClusterStore
var _scheduler: SparseHydroScheduler
var _atlas: SparseHydroAtlasGPU
var _identity: SparseHydroIdentityBridge
var _connectivity: SparseHydroConnectivityGPU
var _runtime: TestRuntime
var _terrain: DryTerrainStage
var _promotion: PlanetRiverReachClusterPromotionBridge
var _coupling: HydroRiverReachClusterCoupling
var _collapse: PlanetRiverReachClusterCollapseBridge
var _volume_diag: SparseHydroVolumeDiagnosticsGPU
var _source := -1
var _coarse_before := 0.0
var _channel_before := 0.0
var _represented := 0.0
var _frames := 0
var _finished := false


class TestRuntime:
	extends SparseHydrologyRuntime
	func initialized_ok() -> bool:
		return scheduler != null and atlas != null and connectivity != null
	func busy() -> bool:
		return false
	func advance_time(_dt_s: float) -> Error:
		return OK


class DryTerrainStage:
	extends HydroTerrainBedGPU
	var test_atlas: SparseHydroAtlasGPU
	var next_request_id := 1
	func setup(atlas: SparseHydroAtlasGPU) -> void:
		test_atlas = atlas
	func initialized_ok() -> bool:
		return test_atlas != null and test_atlas.initialized_ok()
	func stage_reserved_tile(key: HydroTileKey, slot: int,
			delta_override: PackedFloat32Array = PackedFloat32Array()) -> Dictionary:
		if key == null or not initialized_ok() or slot < 0 or slot >= test_atlas.capacity:
			return {"queued": false, "error": ERR_INVALID_PARAMETER}
		var state := PackedFloat32Array()
		state.resize(test_atlas.cells_per_tile() * 4)
		for y in test_atlas.tile_resolution:
			for x in test_atlas.tile_resolution:
				var i := y * test_atlas.tile_resolution + x
				state[i * 4 + 3] = 100.0 + 0.01 * float(x)
		var err := test_atlas.stage_slot_state(slot, state)
		if err != OK:
			return {"queued": false, "error": err}
		var id := next_request_id
		next_request_id += 1
		call_deferred(&"_publish", id, key.packed(), slot)
		return {"queued": true, "error": OK, "request_id": id}
	func _publish(request_id: int, tile_id: int, slot: int) -> void:
		stage_recorded.emit(request_id, tile_id, slot)


func _ready() -> void:
	if RenderingServer.get_rendering_device() == null:
		print("PLANET_RIVER_CLUSTER_ROUNDTRIP: SKIP (RenderingDevice unavailable)")
		get_tree().quit(0)
		return
	var fixture := _make_store()
	if fixture.is_empty():
		return
	_store = fixture["store"] as PlanetHydrologyRiverClusterStore
	_source = int(fixture["source"])
	_coarse_before = _store.total_storage_m3()
	_channel_before = _store.channel_storage_m3[_source]

	_scheduler = SparseHydroScheduler.new(6)
	_scheduler.freeze_wet_tiles = false
	_atlas = SparseHydroAtlasGPU.new()
	add_child(_atlas)
	_atlas.initialized.connect(_on_atlas_initialized)
	_atlas.initialization_failed.connect(func(error: Error):
		_fail("atlas initialization failed (%d)" % int(error)))
	if _atlas.initialize(6, TILE_RES, TILE_DX) != OK:
		_fail("atlas initialize rejected")


func _process(_delta: float) -> void:
	if _finished:
		return
	_frames += 1
	if _frames > TIMEOUT_FRAMES:
		_fail("timed out")


func _on_atlas_initialized() -> void:
	_identity = SparseHydroIdentityBridge.new()
	add_child(_identity)
	if _identity.bind(_scheduler, _atlas) != OK:
		_fail("identity bind failed")
		return
	_connectivity = SparseHydroConnectivityGPU.new()
	add_child(_connectivity)
	_connectivity.initialized.connect(_on_connectivity_initialized)
	_connectivity.initialization_failed.connect(func(error: Error):
		_fail("connectivity initialization failed (%d)" % int(error)))
	if _connectivity.initialize(6) != OK:
		_fail("connectivity initialize rejected")


func _on_connectivity_initialized() -> void:
	if _connectivity.sync_pool(_scheduler.pool) != OK:
		_fail("initial connectivity sync failed")
		return
	_runtime = TestRuntime.new()
	_runtime.scheduler = _scheduler
	_runtime.atlas = _atlas
	_runtime.connectivity = _connectivity
	_runtime.identity_bridge = _identity
	_runtime.enabled = true
	add_child(_runtime)
	_coupling = HydroRiverReachClusterCoupling.new()
	add_child(_coupling)
	_coupling.initialized.connect(_on_coupling_initialized)
	_coupling.initialization_failed.connect(func(error: Error):
		_fail("cluster coupling initialization failed (%d)" % int(error)))
	if _coupling.initialize(_store, _runtime) != OK:
		_fail("cluster coupling initialize rejected")


func _on_coupling_initialized() -> void:
	_terrain = DryTerrainStage.new()
	_terrain.setup(_atlas)
	add_child(_terrain)
	_promotion = PlanetRiverReachClusterPromotionBridge.new()
	_promotion.default_cluster_tiles = CLUSTER_TILES
	add_child(_promotion)
	_promotion.initialized.connect(_on_promotion_initialized)
	_promotion.initialization_failed.connect(func(error: Error):
		_fail("cluster promotion initialization failed (%d)" % int(error)))
	_promotion.promotion_completed.connect(_on_promotion_completed)
	_promotion.promotion_failed.connect(func(_id: int, error: Error, stage: String):
		_fail("cluster promotion failed stage=%s error=%d" % [stage, int(error)]))
	if _promotion.initialize(_store, _scheduler, _atlas, _connectivity,
			_identity, _terrain, _runtime) != OK:
		_fail("cluster promotion initialize rejected")


func _on_promotion_initialized() -> void:
	if _promotion.promote_cluster(_source, CLUSTER_TILES, TRANSFER_M3) < 0:
		_fail("cluster promotion request rejected")


func _on_promotion_completed(_request_id: int, report: Dictionary) -> void:
	_represented = float(report.get("represented_volume_m3", -1.0))
	var members_value: Variant = report.get("members", null)
	if _represented <= 0.0 or not (members_value is Array):
		_fail("cluster promotion report invalid")
		return
	var members: Array = members_value
	if members.size() != CLUSTER_TILES:
		_fail("planner did not produce requested three-member cluster")
		return
	if _coupling.register_promoted_cluster(report) != OK:
		_fail("cluster coupling registration failed")
		return
	if _store.refined_reach_count() != 1 or _store.refined_cluster_size(_source) != CLUSTER_TILES:
		_fail("coarse store did not record one three-member refinement")
		return
	if _scheduler.pool.allocated_count() != CLUSTER_TILES:
		_fail("cluster member allocation count is wrong")
		return
	if not _verify_internal_connectivity(members):
		return
	if absf(_store.total_storage_m3() + _represented - _coarse_before) > ABS_TOL:
		_fail("coarse+fine total did not close after cluster promotion")
		return

	# Mark every member quiet. Atomic collapse must refuse to treat only one member
	# as the whole reach and must remove all three together.
	for value: Variant in members:
		var member := value as Dictionary
		var key := HydroTileKey.unpack(int(member.get("tile_id", -1)))
		if key == null:
			_fail("invalid cluster member key")
			return
		_scheduler.pool.update_activity(key, 0.4, 0.001, 0.001, 1.0e-6, 25.0)
		_scheduler.pool.set_state(key, HydroTilePool.TileState.SETTLING, "cluster_quiet")

	_collapse = PlanetRiverReachClusterCollapseBridge.new()
	add_child(_collapse)
	_collapse.initialized.connect(_on_collapse_initialized)
	_collapse.initialization_failed.connect(func(error: Error):
		_fail("cluster collapse initialization failed (%d)" % int(error)))
	_collapse.collapse_completed.connect(_on_collapse_completed)
	_collapse.collapse_failed.connect(func(_id: int, error: Error, stage: String):
		_fail("cluster collapse failed stage=%s error=%d" % [stage, int(error)]))
	if _collapse.initialize(_store, _scheduler, _atlas, _connectivity,
			_runtime, _coupling) != OK:
		_fail("cluster collapse initialize rejected")


func _verify_internal_connectivity(members: Array) -> bool:
	var arrays := SparseHydroConnectivityGPU.build_arrays(_scheduler.pool)
	var slots := arrays["neighbor_slots"] as PackedInt32Array
	for i in members.size() - 1:
		var a := members[i] as Dictionary
		var b := members[i + 1] as Dictionary
		var link := a.get("downstream_link", {}) as Dictionary
		var direction := int(link.get("source_direction", -1))
		var a_slot := int(a.get("slot", -1))
		var b_slot := int(b.get("slot", -1))
		if direction < 0 or slots[a_slot * 4 + direction] != b_slot:
			_fail("cluster internal sparse connectivity missing at member %d" % i)
			return false
	return true


func _on_collapse_initialized() -> void:
	if not _collapse.eligible(_source):
		_fail("quiet cluster is not collapse eligible")
		return
	if _collapse.collapse_cluster(_source) < 0:
		_fail("cluster collapse request rejected")


func _on_collapse_completed(_request_id: int, report: Dictionary) -> void:
	if int(report.get("member_count", 0)) != CLUSTER_TILES:
		_fail("collapse did not account for every cluster member")
		return
	if absf(float(report.get("fine_volume_m3", 0.0)) - _represented) > ABS_TOL:
		_fail("aggregate collapsed fine volume differs from promoted cluster parcel")
		return
	if _store.is_refined_reach(_source) or _coupling.registered_count() != 0:
		_fail("cluster refinement metadata survived collapse")
		return
	if _scheduler.pool.allocated_count() != 0:
		_fail("one or more cluster members survived atomic collapse")
		return
	if absf(_store.channel_storage_m3[_source] - _channel_before) > ABS_TOL:
		_fail("cluster round trip did not restore channel storage")
		return
	if absf(_store.total_storage_m3() - _coarse_before) > ABS_TOL:
		_fail("cluster round trip changed coarse total")
		return
	if absf(_store.cumulative_promoted_to_fine_m3 \
			- _store.cumulative_demoted_from_fine_m3) > ABS_TOL:
		_fail("cluster promotion/demotion ledger did not close")
		return

	_volume_diag = SparseHydroVolumeDiagnosticsGPU.new()
	add_child(_volume_diag)
	_volume_diag.initialized.connect(_on_diag_initialized)
	_volume_diag.initialization_failed.connect(func(error: Error):
		_fail("final volume diagnostic initialization failed (%d)" % int(error)))
	_volume_diag.volume_ready.connect(_on_final_volume)
	_volume_diag.readback_failed.connect(func(_id: int, error: Error):
		_fail("final sparse volume readback failed (%d)" % int(error)))
	if _volume_diag.initialize(_atlas.state_a_rid(), _atlas.occupancy_rid(),
		_atlas.capacity, TILE_RES, TILE_DX) != OK:
		_fail("final sparse volume diagnostic rejected")


func _on_diag_initialized() -> void:
	if _volume_diag.request_volume() < 0:
		_fail("final sparse volume request rejected")


func _on_final_volume(_request_id: int, volume_m3: float) -> void:
	if absf(volume_m3) > ABS_TOL:
		_fail("authoritative sparse volume is nonzero after atomic cluster collapse")
		return
	_finished = true
	print("PLANET_RIVER_CLUSTER_ROUNDTRIP: PASS members=", CLUSTER_TILES,
		" represented=", _represented, " final_fine=", volume_m3)
	_cleanup()
	get_tree().quit(0)


func _make_store() -> Dictionary:
	var cfg := GenConfig.new()
	cfg.face_res = TEST_RES
	cfg.planet_radius = TEST_RADIUS_M
	var grid := PlanetGrid.new(TEST_RES, TEST_RADIUS_M)
	var fields := PlanetFields.new(cfg, grid)
	fields.elev.fill(100.0)
	fields.base_elev.fill(100.0)
	fields.flow_dir.fill(255)
	fields.lake_level.fill(-1.0e9)
	fields.soil_depth.fill(0.40)
	fields.soil_sand.fill(0.45)
	fields.soil_silt.fill(0.35)
	fields.soil_clay.fill(0.20)
	fields.soil_organic.fill(0.05)
	fields.soil_moisture.fill(0.0)
	fields.aquifer.fill(0.35)
	fields.floodplain.fill(0.20)
	fields.relief.fill(20.0)
	fields.discharge.fill(0.0)
	fields.stream_order.fill(1)
	fields.river_width.fill(0.0)
	var source := 0
	var direction_slot := 0
	var destination := int(grid.nbr[source * 8 + direction_slot])
	if destination == source or destination < 0 or destination >= grid.cell_count:
		_fail("fixture has no downstream neighbor")
		return {}
	fields.flow_dir[source] = direction_slot
	fields.elev[source] = 110.0
	fields.elev[destination] = 90.0
	fields.discharge[source] = BASELINE_Q
	fields.stream_order[source] = 3
	fields.river_width[source] = 7.2 * sqrt(BASELINE_Q)
	var store := PlanetHydrologyRiverClusterStore.new()
	var err := store.initialize(fields)
	if err != OK:
		_fail("cluster store initialization failed (%d)" % int(err))
		return {}
	store.set_climatology_fallback_enabled(false)
	store.initial_storage_m3 = store.total_storage_m3()
	return {"store": store, "source": source}


func _fail(message: String) -> void:
	if _finished:
		return
	_finished = true
	push_error("PLANET_RIVER_CLUSTER_ROUNDTRIP: " + message)
	_cleanup()
	get_tree().quit(1)


func _cleanup() -> void:
	if _volume_diag != null and is_instance_valid(_volume_diag):
		_volume_diag.release()
	if _collapse != null and is_instance_valid(_collapse):
		_collapse.release()
	if _promotion != null and is_instance_valid(_promotion):
		_promotion.release()
	if _coupling != null and is_instance_valid(_coupling):
		_coupling.release()
	if _identity != null and is_instance_valid(_identity):
		_identity.unbind()
	if _connectivity != null and is_instance_valid(_connectivity):
		_connectivity.release()
	if _atlas != null and is_instance_valid(_atlas):
		_atlas.release()
