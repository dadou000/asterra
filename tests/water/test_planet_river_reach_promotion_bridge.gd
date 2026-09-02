extends Node
## Renderer-mode ownership/conservation gate for PlanetRiverReachPromotionBridge.

const TEST_RES := 2
const TEST_RADIUS_M := 1000.0
const BASELINE_Q := 10.0
const TILE_RES := 8
const TILE_DX := 2.0
const TRANSFER_M3 := 24.0
const ABS_TOL := 3.0e-3
const TIMEOUT_FRAMES := 1800

var _store: PlanetHydrologyRiverPromotionStore
var _scheduler: SparseHydroScheduler
var _atlas: SparseHydroAtlasGPU
var _identity: SparseHydroIdentityBridge
var _connectivity: SparseHydroConnectivityGPU
var _terrain: DryTerrainStage
var _bridge: PlanetRiverReachPromotionBridge
var _diag: SparseHydroVolumeDiagnosticsGPU
var _source := -1
var _coarse_before := 0.0
var _surface_before := 0.0
var _channel_before := 0.0
var _represented := 0.0
var _frames := 0
var _finished := false


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
		if not initialized_ok() or key == null or slot < 0 or slot >= test_atlas.capacity:
			return {"queued": false, "error": ERR_INVALID_PARAMETER}
		var state := PackedFloat32Array()
		state.resize(test_atlas.cells_per_tile() * SparseHydroAtlasGPU.STATE_FLOATS)
		for y in test_atlas.tile_resolution:
			for x in test_atlas.tile_resolution:
				var i := y * test_atlas.tile_resolution + x
				state[i * 4 + 3] = 100.0 + float(x) * 0.01
		var err := test_atlas.stage_slot_state(slot, state)
		if err != OK:
			return {"queued": false, "error": err}
		var request_id := next_request_id
		next_request_id += 1
		call_deferred(&"_publish", request_id, key.packed(), slot)
		return {"queued": true, "error": OK, "request_id": request_id}

	func _publish(request_id: int, tile_id: int, slot: int) -> void:
		stage_recorded.emit(request_id, tile_id, slot)


func _ready() -> void:
	if RenderingServer.get_rendering_device() == null:
		print("PLANET_RIVER_REACH_PROMOTION_BRIDGE: SKIP (RenderingDevice unavailable)")
		get_tree().quit(0)
		return
	var fixture := _make_store()
	if fixture.is_empty():
		return
	_store = fixture["store"] as PlanetHydrologyRiverPromotionStore
	_source = int(fixture["source"])
	_store.surface_storage_m3[_source] = 17.0
	_store.initial_storage_m3 = _store.total_storage_m3()
	_coarse_before = _store.total_storage_m3()
	_surface_before = _store.surface_storage_m3[_source]
	_channel_before = _store.channel_storage_m3[_source]

	_scheduler = SparseHydroScheduler.new(1)
	_scheduler.freeze_wet_tiles = false
	_atlas = SparseHydroAtlasGPU.new()
	add_child(_atlas)
	_atlas.initialized.connect(_on_atlas_initialized)
	_atlas.initialization_failed.connect(func(error: Error):
		_fail("atlas initialization failed (%d)" % int(error)))
	var err := _atlas.initialize(1, TILE_RES, TILE_DX)
	if err != OK:
		_fail("atlas initialize rejected (%d)" % int(err))


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
		_fail("identity bridge bind failed")
		return
	_connectivity = SparseHydroConnectivityGPU.new()
	add_child(_connectivity)
	_connectivity.initialized.connect(_on_connectivity_initialized)
	_connectivity.initialization_failed.connect(func(error: Error):
		_fail("connectivity initialization failed (%d)" % int(error)))
	var err := _connectivity.initialize(1)
	if err != OK:
		_fail("connectivity initialize rejected (%d)" % int(err))


func _on_connectivity_initialized() -> void:
	if _connectivity.sync_pool(_scheduler.pool) != OK:
		_fail("initial connectivity sync failed")
		return
	_terrain = DryTerrainStage.new()
	_terrain.setup(_atlas)
	add_child(_terrain)
	_bridge = PlanetRiverReachPromotionBridge.new()
	add_child(_bridge)
	_bridge.initialized.connect(_on_bridge_initialized)
	_bridge.initialization_failed.connect(func(error: Error):
		_fail("bridge initialization failed (%d)" % int(error)))
	_bridge.promotion_completed.connect(_on_promotion_completed)
	_bridge.promotion_failed.connect(func(_id: int, error: Error, stage: String):
		_fail("promotion failed stage=%s error=%d" % [stage, int(error)]))
	var err := _bridge.initialize(_store, _scheduler, _atlas,
		_connectivity, _identity, _terrain, null)
	if err != OK:
		_fail("bridge initialize rejected (%d)" % int(err))


func _on_bridge_initialized() -> void:
	var request := _bridge.promote_reach(_source, TRANSFER_M3)
	if request < 0:
		_fail("river promotion request rejected")


func _on_promotion_completed(_request_id: int, report: Dictionary) -> void:
	_represented = float(report.get("represented_volume_m3", -1.0))
	if _represented <= 0.0:
		_fail("promotion reported invalid represented volume")
		return
	if String(report.get("seed_strategy", "")) != "terrain_aligned_river_corridor":
		_fail("promotion did not use river corridor strategy")
		return
	if absf(_store.surface_storage_m3[_source] - _surface_before) > ABS_TOL:
		_fail("river promotion changed coarse surface storage")
		return
	if absf(_store.channel_storage_m3[_source] - (_channel_before - _represented)) > ABS_TOL:
		_fail("river promotion did not debit channel storage exactly")
		return
	if _store.pending_ownership_transaction_count() != 0:
		_fail("river promotion left pending ownership")
		return

	_diag = SparseHydroVolumeDiagnosticsGPU.new()
	add_child(_diag)
	_diag.initialized.connect(_on_diag_initialized)
	_diag.initialization_failed.connect(func(error: Error):
		_fail("volume diagnostic initialization failed (%d)" % int(error)))
	_diag.volume_ready.connect(_on_volume_ready)
	_diag.readback_failed.connect(func(_id: int, error: Error):
		_fail("volume readback failed (%d)" % int(error)))
	var err := _diag.initialize(_atlas.state_a_rid(), _atlas.occupancy_rid(),
		1, TILE_RES, TILE_DX)
	if err != OK:
		_fail("volume diagnostic initialize rejected (%d)" % int(err))


func _on_diag_initialized() -> void:
	if _diag.request_volume() < 0:
		_fail("volume request rejected")


func _on_volume_ready(_request_id: int, fine_volume_m3: float) -> void:
	if absf(fine_volume_m3 - _represented) > ABS_TOL:
		_fail("fine GPU volume %.9g differs from channel debit %.9g" % [
			fine_volume_m3, _represented])
		return
	if absf(_store.total_storage_m3() + fine_volume_m3 - _coarse_before) > ABS_TOL:
		_fail("coarse + fine river ownership is not conserved")
		return
	if absf(_store.mass_error_m3()) > ABS_TOL:
		_fail("coarse ownership ledger is not closed after river promotion")
		return
	_finished = true
	print("PLANET_RIVER_REACH_PROMOTION_BRIDGE: PASS represented=", _represented)
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
		_fail("fixture has no usable downstream neighbor")
		return {}
	fields.flow_dir[source] = direction_slot
	fields.elev[source] = 110.0
	fields.elev[destination] = 90.0
	fields.discharge[source] = BASELINE_Q
	fields.stream_order[source] = 3
	fields.river_width[source] = 7.2 * sqrt(BASELINE_Q)
	var store := PlanetHydrologyRiverPromotionStore.new()
	var err := store.initialize(fields)
	if err != OK:
		_fail("store initialization failed (%d)" % int(err))
		return {}
	store.set_climatology_fallback_enabled(false)
	return {"store": store, "source": source}


func _fail(message: String) -> void:
	if _finished:
		return
	_finished = true
	push_error("PLANET_RIVER_REACH_PROMOTION_BRIDGE: " + message)
	_cleanup()
	get_tree().quit(1)


func _cleanup() -> void:
	if _diag != null and is_instance_valid(_diag):
		_diag.release()
	if _bridge != null and is_instance_valid(_bridge):
		_bridge.release()
	if _identity != null and is_instance_valid(_identity):
		_identity.unbind()
	if _connectivity != null and is_instance_valid(_connectivity):
		_connectivity.release()
	if _atlas != null and is_instance_valid(_atlas):
		_atlas.release()
