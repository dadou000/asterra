extends Node
## Renderer-mode automatic-policy gate using real river promotion/coupling/collapse.

const TEST_RES := 2
const TEST_RADIUS_M := 1000.0
const BASELINE_Q := 10.0
const TILE_RES := 8
const TILE_DX := 2.0
const ABS_TOL := 5.0e-3
const TIMEOUT_FRAMES := 3000

var _store: PlanetHydrologyRiverCoupledStore
var _scheduler: SparseHydroScheduler
var _atlas: SparseHydroAtlasGPU
var _identity: SparseHydroIdentityBridge
var _connectivity: SparseHydroConnectivityGPU
var _runtime: RiverCouplingRuntimeStub
var _coupling: HydroRiverReachCouplingCollapse
var _terrain: DryTerrainStage
var _promotion: PlanetRiverReachPromotionBridge
var _collapse: PlanetRiverReachCollapseBridgeProduction
var _water: PolicyWaterFacade
var _persistent: PolicyPersistentFacade
var _policy: HydroAutomaticChannelRefinement
var _diag: SparseHydroVolumeDiagnosticsGPU
var _source := -1
var _coarse_before := 0.0
var _surface_before := 0.0
var _channel_before := 0.0
var _frames := 0
var _finished := false
var _promotion_seen := false
var _collapse_seen := false


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


class PolicyPersistentFacade:
	extends Node
	signal store_rebuilt
	var test_store: PlanetHydrologyRiverCoupledStore
	func available() -> bool: return test_store != null and test_store.initialized
	func store() -> PlanetHydrologyRiverCoupledStore: return test_store


class PolicyWaterFacade:
	extends Node
	signal river_reach_promotion_completed(report: Dictionary)
	signal river_reach_promotion_failed(error: Error, stage: String)
	signal river_reach_collapse_completed(report: Dictionary)
	signal river_reach_collapse_failed(error: Error, stage: String)

	var automatic_channel_promotion_enabled := true
	var automatic_channel_demotion_enabled := true
	var store: PlanetHydrologyRiverCoupledStore
	var promotion: PlanetRiverReachPromotionBridge
	var coupling: HydroRiverReachCouplingCollapse
	var collapse: PlanetRiverReachCollapseBridgeProduction

	func setup(p_store: PlanetHydrologyRiverCoupledStore,
			p_promotion: PlanetRiverReachPromotionBridge,
			p_coupling: HydroRiverReachCouplingCollapse,
			p_collapse: PlanetRiverReachCollapseBridgeProduction) -> void:
		store = p_store
		promotion = p_promotion
		coupling = p_coupling
		collapse = p_collapse
		promotion.promotion_completed.connect(_on_promotion_completed)
		promotion.promotion_failed.connect(func(_id: int, error: Error, stage: String):
			river_reach_promotion_failed.emit(error, stage))
		collapse.collapse_completed.connect(_on_collapse_completed)
		collapse.collapse_failed.connect(func(_id: int, error: Error, stage: String):
			river_reach_collapse_failed.emit(error, stage))

	func river_reach_promotion_bridge_available() -> bool:
		return promotion != null and promotion.initialized_ok()
	func river_reach_coupling_available() -> bool:
		return coupling != null and coupling.initialized_ok()
	func river_reach_collapse_bridge_available() -> bool:
		return collapse != null and collapse.initialized_ok()
	func river_reach_coupling() -> HydroRiverReachCoupling:
		return coupling
	func river_reach_collapse_eligible(cell: int) -> bool:
		return collapse != null and collapse.eligible(cell)
	func channel_reach_candidates(max_count: int, q_ratio: float,
			bank_ratio: float) -> Array[Dictionary]:
		return store.channel_reach_candidates(max_count, q_ratio, bank_ratio)
	func suggested_river_reach_promotion_volume_m3(cell: int) -> float:
		return promotion.suggested_channel_volume_m3(cell)
	func promote_coarse_river_reach(cell: int, volume_m3: float) -> int:
		return promotion.promote_reach(cell, volume_m3)
	func collapse_fine_river_reach(cell: int, ignore_quiet: bool = false) -> int:
		return collapse.collapse_reach(cell, ignore_quiet)

	func _on_promotion_completed(request_id: int, report: Dictionary) -> void:
		var published := report.duplicate(true)
		published["request_id"] = request_id
		var err := coupling.register_promoted_reach(published)
		if err != OK:
			river_reach_promotion_failed.emit(err, "coupling_registration")
			return
		river_reach_promotion_completed.emit(published)

	func _on_collapse_completed(request_id: int, report: Dictionary) -> void:
		var published := report.duplicate(true)
		published["request_id"] = request_id
		river_reach_collapse_completed.emit(published)


func _ready() -> void:
	if RenderingServer.get_rendering_device() == null:
		print("HYDRO_AUTOMATIC_CHANNEL_REFINEMENT_GPU: SKIP (RenderingDevice unavailable)")
		get_tree().quit(0)
		return
	var fixture := _make_store()
	if fixture.is_empty():
		return
	_store = fixture["store"] as PlanetHydrologyRiverCoupledStore
	_source = int(fixture["source"])
	_coarse_before = _store.total_storage_m3()
	_surface_before = _store.surface_storage_m3[_source]
	_channel_before = _store.channel_storage_m3[_source]
	# Make this reach an explicit high-Q promotion candidate without adding/removing
	# water; the policy acts on anomaly state while the bridge still transfers an
	# exact parcel from existing channel storage.
	_store.channel_discharge_m3s[_source] = BASELINE_Q * 2.5

	_scheduler = SparseHydroScheduler.new(1)
	_scheduler.freeze_wet_tiles = false
	_atlas = SparseHydroAtlasGPU.new()
	add_child(_atlas)
	_atlas.initialized.connect(_on_atlas_initialized)
	_atlas.initialization_failed.connect(func(error: Error):
		_fail("atlas initialization failed (%d)" % int(error)))
	if _atlas.initialize(1, TILE_RES, TILE_DX) != OK:
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
	if _promotion.initialize(_store, _scheduler, _atlas,
		_connectivity, _identity, _terrain, _runtime) != OK:
		_fail("promotion initialize rejected")


func _on_promotion_initialized() -> void:
	_collapse = PlanetRiverReachCollapseBridgeProduction.new()
	add_child(_collapse)
	_collapse.initialized.connect(_on_collapse_initialized)
	_collapse.initialization_failed.connect(func(error: Error):
		_fail("collapse initialization failed (%d)" % int(error)))
	if _collapse.initialize(_store, _scheduler, _atlas,
		_connectivity, _runtime, _coupling) != OK:
		_fail("collapse initialize rejected")


func _on_collapse_initialized() -> void:
	_persistent = PolicyPersistentFacade.new()
	_persistent.test_store = _store
	add_child(_persistent)
	_water = PolicyWaterFacade.new()
	_water.setup(_store, _promotion, _coupling, _collapse)
	add_child(_water)
	_water.river_reach_promotion_completed.connect(_on_policy_promotion_completed)
	_water.river_reach_collapse_completed.connect(_on_policy_collapse_completed)
	_water.river_reach_promotion_failed.connect(func(error: Error, stage: String):
		_fail("policy promotion failed stage=%s error=%d" % [stage, int(error)]))
	_water.river_reach_collapse_failed.connect(func(error: Error, stage: String):
		_fail("policy collapse failed stage=%s error=%d" % [stage, int(error)]))
	_policy = HydroAutomaticChannelRefinement.new()
	_policy.scan_interval_s = 999.0
	_policy.max_auto_refined_reaches = 1
	if _policy.bind_facades_for_test(_water, _persistent) != OK:
		_fail("policy test facade bind failed")
		return
	add_child(_policy)
	_policy.scan_once()


func _on_policy_promotion_completed(report: Dictionary) -> void:
	if _promotion_seen:
		return
	_promotion_seen = true
	if int(report.get("cell", -1)) != _source:
		_fail("automatic policy promoted wrong reach")
		return
	if not _store.is_refined_reach(_source):
		_fail("automatic promotion did not register refined reach")
		return
	# Bring the anomaly below the exit band and mark the fine tile quiet. The policy
	# must now use its own ownership registry and the real collapse eligibility gate.
	_store.channel_discharge_m3s[_source] = BASELINE_Q
	var rec := _coupling.registered_reach(_source)
	var key := HydroTileKey.unpack(int(rec.get("tile_id", -1)))
	if key == null:
		_fail("automatic refined reach has invalid tile identity")
		return
	_scheduler.pool.update_activity(key, 0.40, 0.001, 0.001, 1.0e-6, 25.0)
	_scheduler.pool.set_state(key, HydroTilePool.TileState.SETTLING, "auto_channel_quiet")
	# registered_reach() starts with last_downstream_q=0, which is below exit Q.
	call_deferred(&"_scan_for_collapse")


func _scan_for_collapse() -> void:
	if _finished:
		return
	_policy.scan_once()


func _on_policy_collapse_completed(report: Dictionary) -> void:
	if _collapse_seen:
		return
	_collapse_seen = true
	if int(report.get("cell", -1)) != _source:
		_fail("automatic policy collapsed wrong reach")
		return
	if _store.is_refined_reach(_source):
		_fail("automatic collapse left refinement hole")
		return
	if _scheduler.pool.allocated_count() != 0:
		_fail("automatic collapse left sparse tile allocated")
		return
	if absf(_store.surface_storage_m3[_source] - _surface_before) > ABS_TOL:
		_fail("automatic channel round trip changed surface storage")
		return
	if absf(_store.channel_storage_m3[_source] - _channel_before) > ABS_TOL:
		_fail("automatic channel round trip did not restore channel storage")
		return
	if absf(_store.total_storage_m3() - _coarse_before) > ABS_TOL:
		_fail("automatic channel round trip did not restore coarse total")
		return
	if absf(_store.cumulative_promoted_to_fine_m3 \
			- _store.cumulative_demoted_from_fine_m3) > ABS_TOL:
		_fail("automatic channel ownership ledger did not close")
		return
	_diag = SparseHydroVolumeDiagnosticsGPU.new()
	add_child(_diag)
	_diag.initialized.connect(_on_diag_initialized)
	_diag.initialization_failed.connect(func(error: Error):
		_fail("final volume diagnostic initialization failed (%d)" % int(error)))
	_diag.volume_ready.connect(_on_final_volume)
	_diag.readback_failed.connect(func(_id: int, error: Error):
		_fail("final sparse volume readback failed (%d)" % int(error)))
	if _diag.initialize(_atlas.state_a_rid(), _atlas.occupancy_rid(),
		1, TILE_RES, TILE_DX) != OK:
		_fail("final volume diagnostic initialize rejected")


func _on_diag_initialized() -> void:
	if _diag.request_volume() < 0:
		_fail("final sparse volume request rejected")


func _on_final_volume(_request_id: int, volume_m3: float) -> void:
	if absf(volume_m3) > ABS_TOL:
		_fail("fine authoritative volume not zero after automatic collapse")
		return
	var policy_stats := _policy.stats()
	if int(policy_stats.get("promotions_completed", 0)) != 1 \
			or int(policy_stats.get("collapses_completed", 0)) != 1:
		_fail("policy did not record exactly one automatic round trip")
		return
	_finished = true
	print("HYDRO_AUTOMATIC_CHANNEL_REFINEMENT_GPU: PASS coarse=", _store.total_storage_m3(),
		" fine=", volume_m3)
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
	var destination := int(grid.nbr[source * 8])
	if destination == source or destination < 0 or destination >= grid.cell_count:
		_fail("fixture has no usable downstream neighbor")
		return {}
	fields.flow_dir[source] = 0
	fields.elev[source] = 110.0
	fields.elev[destination] = 90.0
	fields.discharge[source] = BASELINE_Q
	fields.stream_order[source] = 3
	fields.river_width[source] = 7.2 * sqrt(BASELINE_Q)
	var store := PlanetHydrologyRiverCoupledStore.new()
	if store.initialize(fields) != OK:
		_fail("store initialization failed")
		return {}
	store.set_climatology_fallback_enabled(false)
	return {"store": store, "source": source}


func _fail(message: String) -> void:
	if _finished:
		return
	_finished = true
	push_error("HYDRO_AUTOMATIC_CHANNEL_REFINEMENT_GPU: " + message)
	_cleanup()
	get_tree().quit(1)


func _cleanup() -> void:
	if _diag != null and is_instance_valid(_diag): _diag.release()
	if _policy != null and is_instance_valid(_policy): _policy.queue_free()
	if _collapse != null and is_instance_valid(_collapse): _collapse.release()
	if _promotion != null and is_instance_valid(_promotion): _promotion.release()
	if _coupling != null and is_instance_valid(_coupling): _coupling.release()
	if _identity != null and is_instance_valid(_identity): _identity.unbind()
	if _connectivity != null and is_instance_valid(_connectivity): _connectivity.release()
	if _atlas != null and is_instance_valid(_atlas): _atlas.release()
