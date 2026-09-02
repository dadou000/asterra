extends Node
## Renderer-mode end-to-end gate for HydroAutomaticSurfacePromotionPolicy.
##
## The policy itself chooses the flooded coarse cell and calls the real
## PlanetHydroPromotionBridge. The test verifies:
## - exactly one surface-flood promotion starts;
## - no channel storage is consumed;
## - coarse debit equals authoritative GPU fine volume;
## - a second scan cannot allocate another tile for the same still-flooded cell
##   because its mapped fine tile is already resident.

const TEST_RES := 2
const TEST_RADIUS_M := 1000.0
const TILE_RES := 8
const TILE_DX_M := 2.0
const SURFACE_DEPTH_M := 0.10
const CHANNEL_STORAGE_M3 := 500.0
const ABS_TOL_M3 := 2.0e-3
const REL_TOL := 8.0e-6
const TIMEOUT_FRAMES := 1200

var _failures: Array[String] = []
var _store: PlanetHydrologyOwnershipStore
var _scheduler: SparseHydroScheduler
var _atlas: SparseHydroAtlasGPU
var _identity: SparseHydroIdentityBridge
var _connectivity: SparseHydroConnectivityGPU
var _terrain: DryTerrainStage
var _bridge: PlanetHydroPromotionBridge
var _persistent_facade: TestPersistentFacade
var _water_facade: TestWaterFacade
var _policy: HydroAutomaticSurfacePromotionPolicy
var _diag: SparseHydroVolumeDiagnosticsGPU
var _coarse_before := 0.0
var _channel_before := 0.0
var _represented_m3 := 0.0
var _frames_waited := 0

enum Phase {
	WAIT_ATLAS,
	WAIT_CONNECTIVITY,
	WAIT_BRIDGE,
	WAIT_PROMOTION,
	WAIT_DIAGNOSTIC_INIT,
	WAIT_VOLUME,
	DONE,
}
var _phase := Phase.WAIT_ATLAS


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
		if not delta_override.is_empty() and delta_override.size() != test_atlas.cells_per_tile():
			return {"queued": false, "error": ERR_INVALID_PARAMETER}
		var state := PackedFloat32Array()
		state.resize(test_atlas.cells_per_tile() * SparseHydroAtlasGPU.STATE_FLOATS)
		var err := test_atlas.stage_slot_state(slot, state)
		if err != OK:
			return {"queued": false, "error": err}
		var request_id := next_request_id
		next_request_id += 1
		call_deferred(&"_publish_stage", request_id, key.packed(), slot)
		return {"queued": true, "error": OK, "request_id": request_id}

	func _publish_stage(request_id: int, tile_id: int, slot: int) -> void:
		stage_recorded.emit(request_id, tile_id, slot)


class TestPersistentFacade:
	extends Node
	signal store_rebuilt
	var backing_store: PlanetHydrologyOwnershipStore

	func setup(p_store: PlanetHydrologyOwnershipStore) -> void:
		backing_store = p_store

	func available() -> bool:
		return backing_store != null and backing_store.initialized

	func store() -> PlanetHydrologyOwnershipStore:
		return backing_store

	func cell_state(cell: int) -> Dictionary:
		return {} if backing_store == null else backing_store.cell_state(cell)


class TestWaterFacade:
	extends Node
	signal planet_promotion_completed(report: Dictionary)
	signal planet_promotion_failed(error: Error, stage: String)
	var automatic_coarse_promotion_enabled := true
	var backing_bridge: PlanetHydroPromotionBridge
	var backing_store: PlanetHydrologyOwnershipStore

	func setup(p_bridge: PlanetHydroPromotionBridge,
			p_store: PlanetHydrologyOwnershipStore) -> void:
		backing_bridge = p_bridge
		backing_store = p_store
		backing_bridge.promotion_completed.connect(_relay_completed)
		backing_bridge.promotion_failed.connect(_relay_failed)

	func planet_promotion_bridge_available() -> bool:
		return backing_bridge != null and backing_bridge.initialized_ok()

	func planet_promotion_bridge() -> PlanetHydroPromotionBridge:
		return backing_bridge

	func coarse_promotion_candidates(max_count: int = 64,
			surface_depth_threshold_m: float = 0.025,
			discharge_ratio_threshold: float = 2.0) -> Array[Dictionary]:
		return [] if backing_store == null else backing_store.promotion_candidates(
			max_count, surface_depth_threshold_m, discharge_ratio_threshold)

	func suggested_surface_promotion_volume_m3(cell: int) -> float:
		return 0.0 if backing_bridge == null else backing_bridge.suggested_surface_volume_m3(cell)

	func promote_coarse_surface_cell(cell: int, requested_volume_m3: float = -1.0,
			local_velocity: Vector2 = Vector2.ZERO) -> int:
		if backing_bridge == null or backing_bridge.busy():
			return -1
		var volume := requested_volume_m3
		if volume < 0.0:
			volume = backing_bridge.suggested_surface_volume_m3(cell)
		return backing_bridge.promote_cell(cell, volume, local_velocity)

	func _relay_completed(_request_id: int, report: Dictionary) -> void:
		planet_promotion_completed.emit(report.duplicate(true))

	func _relay_failed(_request_id: int, error: Error, stage: String) -> void:
		planet_promotion_failed.emit(error, stage)


func _ready() -> void:
	if RenderingServer.get_rendering_device() == null:
		print("HYDRO_AUTO_SURFACE_PROMOTION_GPU: SKIP (RenderingDevice unavailable)")
		get_tree().quit(0)
		return

	_store = _make_store()
	if _store == null:
		_finish()
		return
	var cell := 0
	_store.surface_storage_m3[cell] = _store.area_m2[cell] * SURFACE_DEPTH_M
	_store.channel_storage_m3[cell] = CHANNEL_STORAGE_M3
	_store.initial_storage_m3 = _store.total_storage_m3()
	_coarse_before = _store.total_storage_m3()
	_channel_before = _store.channel_storage_m3[cell]

	_scheduler = SparseHydroScheduler.new(2)
	_scheduler.freeze_wet_tiles = false
	_atlas = SparseHydroAtlasGPU.new()
	_atlas.name = "SparseHydroAtlasGPU"
	add_child(_atlas)
	_atlas.initialized.connect(_on_atlas_initialized)
	_atlas.initialization_failed.connect(func(error: Error):
		_fail("atlas initialization failed (%d)" % int(error)); _finish())
	var err := _atlas.initialize(2, TILE_RES, TILE_DX_M)
	if err != OK:
		_fail("atlas initialization submit failed (%d)" % int(err))
		_finish()


func _process(_delta: float) -> void:
	_frames_waited += 1
	if _phase != Phase.DONE and _frames_waited > TIMEOUT_FRAMES:
		_fail("timeout in phase %s" % Phase.keys()[_phase])
		_finish()


func _on_atlas_initialized() -> void:
	if _phase != Phase.WAIT_ATLAS:
		return
	_identity = SparseHydroIdentityBridge.new()
	_identity.name = "SparseHydroIdentityBridge"
	add_child(_identity)
	var bind_error := _identity.bind(_scheduler, _atlas)
	if bind_error != OK:
		_fail("identity bind failed (%d)" % int(bind_error))
		_finish()
		return

	_connectivity = SparseHydroConnectivityGPU.new()
	_connectivity.name = "SparseHydroConnectivityGPU"
	add_child(_connectivity)
	_connectivity.initialized.connect(_on_connectivity_initialized)
	_connectivity.initialization_failed.connect(func(error: Error):
		_fail("connectivity initialization failed (%d)" % int(error)); _finish())
	_phase = Phase.WAIT_CONNECTIVITY
	var err := _connectivity.initialize(2)
	if err != OK:
		_fail("connectivity initialization submit failed (%d)" % int(err))
		_finish()


func _on_connectivity_initialized() -> void:
	if _phase != Phase.WAIT_CONNECTIVITY:
		return
	var sync_error := _connectivity.sync_pool(_scheduler.pool)
	if sync_error != OK:
		_fail("initial connectivity sync failed (%d)" % int(sync_error))
		_finish()
		return

	_terrain = DryTerrainStage.new()
	_terrain.name = "DryTerrainStage"
	_terrain.setup(_atlas)
	add_child(_terrain)

	_bridge = PlanetHydroPromotionBridge.new()
	_bridge.name = "PlanetHydroPromotionBridge"
	add_child(_bridge)
	_bridge.initialized.connect(_on_bridge_initialized)
	_bridge.initialization_failed.connect(func(error: Error):
		_fail("promotion bridge initialization failed (%d)" % int(error)); _finish())
	_bridge.promotion_completed.connect(_on_bridge_promotion_completed)
	_bridge.promotion_failed.connect(func(_request_id: int, error: Error, stage: String):
		_fail("promotion failed at %s (%d)" % [stage, int(error)]); _finish())
	_phase = Phase.WAIT_BRIDGE
	var err := _bridge.initialize(_store, _scheduler, _atlas, _connectivity,
		_identity, _terrain, null)
	if err != OK:
		_fail("promotion bridge initialization submit failed (%d)" % int(err))
		_finish()


func _on_bridge_initialized() -> void:
	if _phase != Phase.WAIT_BRIDGE:
		return
	_persistent_facade = TestPersistentFacade.new()
	_persistent_facade.name = "TestPersistentFacade"
	_persistent_facade.setup(_store)
	add_child(_persistent_facade)

	_water_facade = TestWaterFacade.new()
	_water_facade.name = "TestWaterFacade"
	_water_facade.setup(_bridge, _store)
	add_child(_water_facade)

	_policy = HydroAutomaticSurfacePromotionPolicy.new()
	_policy.name = "AutomaticSurfacePromotionPolicy"
	_policy.scan_interval_s = 60.0
	_policy.surface_enter_depth_m = 0.05
	_policy.surface_exit_depth_m = 0.025
	_policy.candidate_limit = 8
	var bind_error := _policy.bind_facades_for_test(_water_facade, _persistent_facade)
	if bind_error != OK:
		_fail("policy facade bind failed (%d)" % int(bind_error))
		_finish()
		return
	add_child(_policy)

	_policy.scan_once()
	var stats := _policy.stats()
	_expect(int(stats.get("promotions_started", 0)) == 1,
		"first policy scan did not start exactly one promotion")
	_expect(int(stats.get("active_request_id", -1)) >= 0,
		"policy did not retain active transaction identity")
	_expect(not bool(stats.get("channel_only_promotion_enabled", true)),
		"channel-only policy unexpectedly enabled")
	_phase = Phase.WAIT_PROMOTION


func _on_bridge_promotion_completed(_request_id: int, report: Dictionary) -> void:
	if _phase != Phase.WAIT_PROMOTION:
		return
	# Let TestWaterFacade relay the completion to the policy before checking its
	# policy-local completion state.
	call_deferred(&"_after_policy_completion", report.duplicate(true))


func _after_policy_completion(report: Dictionary) -> void:
	if _phase != Phase.WAIT_PROMOTION:
		return
	_represented_m3 = float(report.get("represented_volume_m3", -1.0))
	_expect(_represented_m3 > 0.0, "promotion acknowledged a non-positive fine parcel")
	_expect_close(float(report.get("reserved_channel_volume_m3", -1.0)), 0.0,
		ABS_TOL_M3, "automatic promotion consumed channel storage")
	_expect_close(_store.channel_storage_m3[0], _channel_before,
		ABS_TOL_M3, "coarse channel storage changed during surface-only promotion")
	_expect(_store.pending_promotion_count() == 0,
		"automatic promotion left unresolved coarse ownership")
	_expect(_scheduler.pool.allocated_count() == 1,
		"automatic promotion did not leave exactly one resident fine tile")
	_expect_close(_store.total_storage_m3(), _coarse_before - _represented_m3,
		maxf(ABS_TOL_M3, _coarse_before * REL_TOL),
		"coarse debit differs from acknowledged fine parcel")
	var policy_stats := _policy.stats()
	_expect(int(policy_stats.get("promotions_completed", 0)) == 1,
		"policy did not observe bridge completion")
	_expect(int(policy_stats.get("active_request_id", -2)) == -1,
		"policy retained completed transaction identity")

	_diag = SparseHydroVolumeDiagnosticsGPU.new()
	_diag.name = "SparseHydroVolumeDiagnosticsGPU"
	add_child(_diag)
	_diag.initialized.connect(_on_diag_initialized)
	_diag.initialization_failed.connect(func(error: Error):
		_fail("fine volume diagnostic initialization failed (%d)" % int(error)); _finish())
	_diag.volume_ready.connect(_on_volume_ready)
	_diag.readback_failed.connect(func(_rid: int, error: Error):
		_fail("fine volume readback failed (%d)" % int(error)); _finish())
	_phase = Phase.WAIT_DIAGNOSTIC_INIT
	var err := _diag.initialize_from_atlas(_atlas)
	if err != OK:
		_fail("fine volume diagnostic submit failed (%d)" % int(err))
		_finish()


func _on_diag_initialized() -> void:
	if _phase != Phase.WAIT_DIAGNOSTIC_INIT:
		return
	_phase = Phase.WAIT_VOLUME
	if _diag.request_volume() < 0:
		_fail("fine volume request rejected")
		_finish()


func _on_volume_ready(_request_id: int, volume_m3: float) -> void:
	if _phase != Phase.WAIT_VOLUME:
		return
	_expect_close(volume_m3, _represented_m3,
		maxf(ABS_TOL_M3, absf(_represented_m3) * REL_TOL),
		"authoritative occupied fine volume differs from promotion acknowledgement")
	_expect_close(_store.total_storage_m3() + volume_m3, _coarse_before,
		maxf(ABS_TOL_M3, _coarse_before * REL_TOL),
		"coarse + occupied fine water is not conserved")

	# The coarse cell still holds roughly 10 cm of surface water after transferring
	# only one fine-tile footprint. A second scan must therefore see the flood but
	# suppress it because the mapped destination tile is already resident.
	_policy.scan_once()
	var stats := _policy.stats()
	_expect(int(stats.get("promotions_started", 0)) == 1,
		"second scan started a duplicate promotion into a resident tile")
	_expect(int(stats.get("resident_suppressed", 0)) >= 1,
		"second scan did not record resident-tile suppression")
	_expect(_scheduler.pool.allocated_count() == 1,
		"resident suppression changed sparse allocation count")
	_expect(_store.pending_promotion_count() == 0,
		"resident suppression opened a coarse ownership transaction")
	_finish()


func _make_store() -> PlanetHydrologyOwnershipStore:
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
	fields.floodplain.fill(0.15)
	fields.relief.fill(20.0)
	fields.discharge.fill(0.0)
	fields.stream_order.fill(1)
	var store := PlanetHydrologyOwnershipStore.new()
	var err := store.initialize(fields)
	if err != OK:
		_fail("coarse store initialization failed (%d)" % int(err))
		return null
	store.set_climatology_fallback_enabled(false)
	return store


func _expect(condition: bool, message: String) -> void:
	if not condition:
		_fail(message)


func _expect_close(value: float, reference: float, tolerance: float,
		message: String) -> void:
	var error := absf(value - reference)
	if error > tolerance:
		_fail("%s: got %.12g expected %.12g (abs %.6g > %.6g)" % [
			message, value, reference, error, tolerance])


func _fail(message: String) -> void:
	if not _failures.has(message):
		_failures.append(message)


func _finish() -> void:
	if _phase == Phase.DONE:
		return
	_phase = Phase.DONE
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
	if _failures.is_empty():
		print("HYDRO_AUTO_SURFACE_PROMOTION_GPU: PASS")
		get_tree().quit(0)
	else:
		for failure in _failures:
			push_error("HYDRO_AUTO_SURFACE_PROMOTION_GPU: " + failure)
		get_tree().quit(1)
