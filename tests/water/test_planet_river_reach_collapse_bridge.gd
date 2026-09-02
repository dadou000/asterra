extends Node
## Renderer-mode end-to-end gate for symmetric 1D -> 2D -> 1D river ownership.

const TEST_RES := 2
const TEST_RADIUS_M := 1000.0
const BASELINE_Q := 10.0
const TILE_RES := 8
const TILE_DX := 2.0
const TRANSFER_M3 := 24.0
const PENDING_CONFLUENCE_M3 := 3.0
const ABS_TOL := 4.0e-3
const TIMEOUT_FRAMES := 2400

var _store: PlanetHydrologyRiverCoupledStore
var _scheduler: SparseHydroScheduler
var _atlas: SparseHydroAtlasGPU
var _identity: SparseHydroIdentityBridge
var _connectivity: SparseHydroConnectivityGPU
var _terrain: DryTerrainStage
var _runtime: RiverCouplingRuntimeStub
var _coupling: HydroRiverReachCouplingCollapse
var _promotion: PlanetRiverReachPromotionBridge
var _collapse: PlanetRiverReachCollapseBridgeProduction
var _volume_diag: SparseHydroVolumeDiagnosticsGPU
var _source := -1
var _coarse_before := 0.0
var _channel_before := 0.0
var _surface_before := 0.0
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
		print("PLANET_RIVER_REACH_COLLAPSE_BRIDGE: SKIP (RenderingDevice unavailable)")
		get_tree().quit(0)
		return
	var fixture := _make_store()
	if fixture.is_empty():
		return
	_store = fixture["store"] as PlanetHydrologyRiverCoupledStore
	_source = int(fixture["source"])
	_store.surface_storage_m3[_source] = 11.0
	_store.initial_storage_m3 = _store.total_storage_m3()
	_coarse_before = _store.total_storage_m3()
	_channel_before = _store.channel_storage_m3[_source]
	_surface_before = _store.surface_storage_m3[_source]

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
	if _connectivity.initialize(1) != OK:
		_fail("connectivity initialize rejected")


func _on_connectivity_initialized() -> void:
	if _connectivity.sync_pool(_scheduler.pool) != OK:
		_fail("initial connectivity sync failed")
		return
	_runtime = RiverCouplingRuntimeStub.new()
	_runtime.scheduler = _scheduler
	_runtime.atlas = _atlas
	_runtime.connectivity = _connectivity
	_runtime.identity_bridge = _identity
	_runtime.enabled = true
	add_child(_runtime)

	_coupling = HydroRiverReachCouplingCollapse.new()
	add_child(_coupling)
	_coupling.initialized.connect(_on_coupling_initialized)
	_coupling.initialization_failed.connect(func(error: Error):
		_fail("coupling initialization failed (%d)" % int(error)))
	if _coupling.initialize(_store, _runtime) != OK:
		_fail("coupling initialize rejected")


func _on_coupling_initialized() -> void:
	_terrain = DryTerrainStage.new()
	_terrain.setup(_atlas)
	add_child(_terrain)
	_promotion = PlanetRiverReachPromotionBridge.new()
	add_child(_promotion)
	_promotion.initialized.connect(_on_promotion_initialized)
	_promotion.initialization_failed.connect(func(error: Error):
		_fail("promotion initialization failed (%d)" % int(error)))
	_promotion.promotion_completed.connect(_on_promotion_completed)
	_promotion.promotion_failed.connect(func(_id: int, error: Error, stage: String):
		_fail("promotion failed stage=%s error=%d" % [stage, int(error)]))
	var err := _promotion.initialize(_store, _scheduler, _atlas,
		_connectivity, _identity, _terrain, _runtime)
	if err != OK:
		_fail("promotion initialize rejected (%d)" % int(err))


func _on_promotion_initialized() -> void:
	if _promotion.promote_reach(_source, TRANSFER_M3) < 0:
		_fail("river promotion request rejected")


func _on_promotion_completed(_request_id: int, report: Dictionary) -> void:
	_represented = float(report.get("represented_volume_m3", -1.0))
	if _represented <= 0.0:
		_fail("promotion returned invalid represented volume")
		return
	var register_error := _coupling.register_promoted_reach(report)
	if register_error != OK:
		_fail("continuous coupling registration failed (%d)" % int(register_error))
		return
	if not _store.is_refined_reach(_source):
		_fail("coarse store did not register refinement hole")
		return

	# Model a confluence parcel that has already routed into the refined macro node
	# but has not yet crossed the GPU mouth. Moving it from residual channel storage
	# to the pending queue is an internal coarse ownership transfer.
	if _store.channel_storage_m3[_source] <= PENDING_CONFLUENCE_M3:
		_fail("fixture has insufficient residual channel storage")
		return
	_store.channel_storage_m3[_source] -= PENDING_CONFLUENCE_M3
	_store.refined_pending_inflow_m3[_source] += PENDING_CONFLUENCE_M3

	var rec := _coupling.registered_reach(_source)
	var key := HydroTileKey.unpack(int(rec.get("tile_id", -1)))
	if key == null:
		_fail("registered reach has invalid tile identity")
		return
	_scheduler.pool.update_activity(key, 0.50, 0.001, 0.001, 1.0e-6, 25.0)
	_scheduler.pool.set_state(key, HydroTilePool.TileState.SETTLING, "collapse_test_quiet")

	_collapse = PlanetRiverReachCollapseBridgeProduction.new()
	add_child(_collapse)
	_collapse.initialized.connect(_on_collapse_initialized)
	_collapse.initialization_failed.connect(func(error: Error):
		_fail("collapse initialization failed (%d)" % int(error)))
	_collapse.collapse_completed.connect(_on_collapse_completed)
	_collapse.collapse_failed.connect(func(_id: int, error: Error, stage: String):
		_fail("collapse failed stage=%s error=%d" % [stage, int(error)]))
	var err := _collapse.initialize(_store, _scheduler, _atlas,
		_connectivity, _runtime, _coupling)
	if err != OK:
		_fail("collapse initialize rejected (%d)" % int(err))


func _on_collapse_initialized() -> void:
	if not _collapse.eligible(_source):
		_fail("quiet registered river was not collapse-eligible")
		return
	if _collapse.collapse_reach(_source) < 0:
		_fail("river collapse request rejected")


func _on_collapse_completed(_request_id: int, report: Dictionary) -> void:
	if _store.is_refined_reach(_source):
		_fail("refinement hole survived successful collapse")
		return
	if not _coupling.registered_reach(_source).is_empty() \
			or _coupling.suspended_count() != 0:
		_fail("continuous coupling retained collapsed reach")
		return
	if _scheduler.pool.allocated_count() != 0:
		_fail("fine tile remained allocated after collapse")
		return
	if absf(float(report.get("fine_volume_m3", 0.0)) - _represented) > ABS_TOL:
		_fail("collapse fine-volume measurement differs from promoted parcel")
		return
	if absf(float(report.get("pending_returned_m3", 0.0)) - PENDING_CONFLUENCE_M3) > ABS_TOL:
		_fail("pending confluence parcel was not returned to 1D")
		return
	if float(report.get("downstream_velocity_mps", 0.0)) <= 0.0:
		_fail("fine downstream momentum was not reconstructed")
		return
	if absf(_store.channel_storage_m3[_source] - _channel_before) > ABS_TOL:
		_fail("1D channel storage was not restored after round trip")
		return
	if absf(_store.surface_storage_m3[_source] - _surface_before) > ABS_TOL:
		_fail("river round trip changed surface storage")
		return
	if absf(_store.total_storage_m3() - _coarse_before) > ABS_TOL:
		_fail("coarse storage did not return to initial total")
		return
	if absf(_store.cumulative_promoted_to_fine_m3 \
			- _store.cumulative_demoted_from_fine_m3) > ABS_TOL:
		_fail("promotion/demotion ownership ledgers did not close")
		return
	if absf(_store.mass_error_m3()) > ABS_TOL:
		_fail("coarse mass ledger did not close")
		return

	_volume_diag = SparseHydroVolumeDiagnosticsGPU.new()
	add_child(_volume_diag)
	_volume_diag.initialized.connect(_on_volume_diag_initialized)
	_volume_diag.initialization_failed.connect(func(error: Error):
		_fail("global sparse volume diagnostic initialization failed (%d)" % int(error)))
	_volume_diag.volume_ready.connect(_on_final_sparse_volume)
	_volume_diag.readback_failed.connect(func(_id: int, error: Error):
		_fail("global sparse volume readback failed (%d)" % int(error)))
	var err := _volume_diag.initialize(_atlas.state_a_rid(), _atlas.occupancy_rid(),
		1, TILE_RES, TILE_DX)
	if err != OK:
		_fail("global sparse volume diagnostic rejected (%d)" % int(err))


func _on_volume_diag_initialized() -> void:
	if _volume_diag.request_volume() < 0:
		_fail("final sparse volume request rejected")


func _on_final_sparse_volume(_request_id: int, volume_m3: float) -> void:
	if absf(volume_m3) > ABS_TOL:
		_fail("authoritative fine volume is not zero after river collapse")
		return
	_finished = true
	print("PLANET_RIVER_REACH_COLLAPSE_BRIDGE: PASS represented=", _represented,
		" pending_returned=", PENDING_CONFLUENCE_M3,
		" final_fine=", volume_m3)
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
	var store := PlanetHydrologyRiverCoupledStore.new()
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
	push_error("PLANET_RIVER_REACH_COLLAPSE_BRIDGE: " + message)
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
