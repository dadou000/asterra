extends Node
## Renderer-mode policy gate for automatic quiet-surface collapse.
## Uses real promotion/demotion bridges and GPU volume reduction; only the runtime
## facade is a deterministic idle stub so policy timing is independent of auto-run.

const TEST_RES := 2
const TEST_RADIUS_M := 1000.0
const TILE_RES := 8
const TILE_DX_M := 2.0
const TRANSFER_M3 := 64.0
const ABS_TOL_M3 := 2.0e-3
const REL_TOL := 8.0e-6
const TIMEOUT_FRAMES := 1500

var _failures: Array[String] = []
var _store: PlanetHydrologyOwnershipStore
var _scheduler: SparseHydroScheduler
var _atlas: SparseHydroAtlasGPU
var _identity: SparseHydroIdentityBridge
var _connectivity: SparseHydroConnectivityGPU
var _terrain: DryTerrainStage
var _promotion: PlanetHydroPromotionBridge
var _demotion: PlanetHydroDemotionBridge
var _runtime_stub: IdleRuntime
var _water_facade: WaterFacade
var _persistent_facade: PersistentFacade
var _policy: HydroAutomaticSurfaceDemotionPolicy
var _diag: SparseHydroVolumeDiagnosticsGPU
var _baseline := 0.0
var _promoted_m3 := 0.0
var _promoted_tile_id := -1
var _frames := 0

enum Phase {
	WAIT_ATLAS,
	WAIT_CONNECTIVITY,
	WAIT_PROMOTION_BRIDGE,
	WAIT_PROMOTION,
	WAIT_DEMOTION_BRIDGE,
	WAIT_POLICY,
	WAIT_FINAL_DIAG,
	WAIT_FINAL_VOLUME,
	DONE,
}
var _phase := Phase.WAIT_ATLAS


class DryTerrainStage:
	extends HydroTerrainBedGPU
	var test_atlas: SparseHydroAtlasGPU
	var next_request_id := 1

	func setup(p_atlas: SparseHydroAtlasGPU) -> void:
		test_atlas = p_atlas

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


class IdleRuntime:
	extends SparseHydrologyRuntime

	func setup(p_scheduler: SparseHydroScheduler) -> void:
		scheduler = p_scheduler

	func initialized_ok() -> bool:
		return true

	func busy() -> bool:
		return false


class PersistentFacade:
	extends Node
	signal store_rebuilt
	var hydro_store: PlanetHydrologyOwnershipStore

	func available() -> bool:
		return hydro_store != null and hydro_store.initialized

	func store() -> PlanetHydrologyOwnershipStore:
		return hydro_store


class WaterFacade:
	extends Node
	signal planet_demotion_completed(report: Dictionary)
	signal planet_demotion_failed(error: Error, stage: String)
	signal sparse_runtime_state_changed(state: String)

	var automatic_fine_demotion_enabled := true
	var bridge: PlanetHydroDemotionBridge
	var runtime_stub: SparseHydrologyRuntime

	func setup(p_bridge: PlanetHydroDemotionBridge,
			p_runtime: SparseHydrologyRuntime) -> void:
		bridge = p_bridge
		runtime_stub = p_runtime
		bridge.demotion_completed.connect(
			func(_request_id: int, report: Dictionary):
				planet_demotion_completed.emit(report.duplicate(true)))
		bridge.demotion_failed.connect(
			func(_request_id: int, error: Error, stage: String):
				planet_demotion_failed.emit(error, stage))

	func planet_demotion_bridge_available() -> bool:
		return bridge != null and bridge.initialized_ok()

	func planet_demotion_bridge() -> PlanetHydroDemotionBridge:
		return bridge

	func sparse_runtime() -> SparseHydrologyRuntime:
		return runtime_stub

	func demote_fine_surface_cell(cell: int) -> int:
		return -1 if bridge == null else bridge.demote_cell(cell)


func _ready() -> void:
	if RenderingServer.get_rendering_device() == null:
		print("HYDRO_AUTOMATIC_SURFACE_DEMOTION_GPU: SKIP (RenderingDevice unavailable)")
		get_tree().quit(0)
		return
	_store = _make_store()
	if _store == null:
		_finish()
		return
	_store.surface_storage_m3[0] = 80.0
	_store.channel_storage_m3[0] = 20.0
	_store.initial_storage_m3 = _store.total_storage_m3()
	_baseline = _store.total_storage_m3()

	_scheduler = SparseHydroScheduler.new(2)
	_scheduler.freeze_wet_tiles = false
	_atlas = SparseHydroAtlasGPU.new()
	add_child(_atlas)
	_atlas.initialized.connect(_on_atlas_initialized)
	_atlas.initialization_failed.connect(func(error: Error):
		_fail("atlas init failed (%d)" % int(error)); _finish())
	var err := _atlas.initialize(2, TILE_RES, TILE_DX_M)
	if err != OK:
		_fail("atlas init submit failed (%d)" % int(err)); _finish()


func _process(_delta: float) -> void:
	_frames += 1
	if _phase != Phase.DONE and _frames > TIMEOUT_FRAMES:
		_fail("timeout in phase %s" % Phase.keys()[_phase])
		_finish()


func _on_atlas_initialized() -> void:
	if _phase != Phase.WAIT_ATLAS:
		return
	_identity = SparseHydroIdentityBridge.new()
	add_child(_identity)
	var bind_error := _identity.bind(_scheduler, _atlas)
	if bind_error != OK:
		_fail("identity bind failed (%d)" % int(bind_error)); _finish(); return
	_connectivity = SparseHydroConnectivityGPU.new()
	add_child(_connectivity)
	_connectivity.initialized.connect(_on_connectivity_initialized)
	_connectivity.initialization_failed.connect(func(error: Error):
		_fail("connectivity init failed (%d)" % int(error)); _finish())
	_phase = Phase.WAIT_CONNECTIVITY
	var err := _connectivity.initialize(2)
	if err != OK:
		_fail("connectivity submit failed (%d)" % int(err)); _finish()


func _on_connectivity_initialized() -> void:
	if _phase != Phase.WAIT_CONNECTIVITY:
		return
	var sync_error := _connectivity.sync_pool(_scheduler.pool)
	if sync_error != OK:
		_fail("initial connectivity sync failed (%d)" % int(sync_error)); _finish(); return
	_terrain = DryTerrainStage.new()
	_terrain.setup(_atlas)
	add_child(_terrain)
	_promotion = PlanetHydroPromotionBridge.new()
	add_child(_promotion)
	_promotion.initialized.connect(_on_promotion_initialized)
	_promotion.initialization_failed.connect(func(error: Error):
		_fail("promotion bridge init failed (%d)" % int(error)); _finish())
	_promotion.promotion_completed.connect(_on_promotion_completed)
	_promotion.promotion_failed.connect(func(_id: int, error: Error, stage: String):
		_fail("promotion failed at %s (%d)" % [stage, int(error)]); _finish())
	_phase = Phase.WAIT_PROMOTION_BRIDGE
	var err := _promotion.initialize(_store, _scheduler, _atlas, _connectivity,
		_identity, _terrain, null)
	if err != OK:
		_fail("promotion bridge submit failed (%d)" % int(err)); _finish()


func _on_promotion_initialized() -> void:
	if _phase != Phase.WAIT_PROMOTION_BRIDGE:
		return
	_phase = Phase.WAIT_PROMOTION
	if _promotion.promote_cell(0, TRANSFER_M3) < 0:
		_fail("promotion request rejected"); _finish()


func _on_promotion_completed(_request_id: int, report: Dictionary) -> void:
	if _phase != Phase.WAIT_PROMOTION:
		return
	_promoted_m3 = float(report.get("represented_volume_m3", -1.0))
	_promoted_tile_id = int(report.get("tile_id", -1))
	_expect(_promoted_m3 > 0.0, "promotion reported non-positive volume")
	_expect(_promoted_tile_id >= 0, "promotion did not report tile identity")
	var key := HydroTileKey.unpack(_promoted_tile_id)
	_expect(key != null and _scheduler.pool.contains(key),
		"promoted tile is not resident")
	if key == null:
		_finish(); return
	# Simulate the GPU activity classifier having observed a long quiet shallow
	# interval. The policy consumes these exact scheduler summary fields.
	_scheduler.pool.update_activity(key, 0.08, 0.001, 0.0005, 1.0e-5, 30.0)
	_scheduler.pool.set_state(key, HydroTilePool.TileState.SETTLING, "policy_test_quiet")

	_demotion = PlanetHydroDemotionBridge.new()
	add_child(_demotion)
	_demotion.initialized.connect(_on_demotion_initialized)
	_demotion.initialization_failed.connect(func(error: Error):
		_fail("demotion bridge init failed (%d)" % int(error)); _finish())
	_demotion.demotion_failed.connect(func(_id: int, error: Error, stage: String):
		_fail("demotion failed at %s (%d)" % [stage, int(error)]); _finish())
	_phase = Phase.WAIT_DEMOTION_BRIDGE
	var err := _demotion.initialize(_store, _scheduler, _atlas, _connectivity,
		_identity, null)
	if err != OK:
		_fail("demotion bridge submit failed (%d)" % int(err)); _finish()


func _on_demotion_initialized() -> void:
	if _phase != Phase.WAIT_DEMOTION_BRIDGE:
		return
	_runtime_stub = IdleRuntime.new()
	_runtime_stub.setup(_scheduler)
	add_child(_runtime_stub)
	_water_facade = WaterFacade.new()
	_water_facade.setup(_demotion, _runtime_stub)
	add_child(_water_facade)
	_persistent_facade = PersistentFacade.new()
	_persistent_facade.hydro_store = _store
	add_child(_persistent_facade)

	_policy = HydroAutomaticSurfaceDemotionPolicy.new()
	_policy.scan_interval_s = 1000.0
	_policy.minimum_quiet_time_s = 20.0
	_policy.maximum_surface_depth_m = 0.15
	var bind_error := _policy.bind_facades_for_test(_water_facade, _persistent_facade)
	if bind_error != OK:
		_fail("policy test facade bind failed (%d)" % int(bind_error)); _finish(); return
	_policy.automatic_surface_demotion_completed.connect(_on_policy_demotion_completed)
	_policy.automatic_surface_demotion_failed.connect(
		func(_cell: int, _tile: int, error: Error, stage: String):
			_fail("policy demotion failed at %s (%d)" % [stage, int(error)]); _finish())
	add_child(_policy)
	_policy.register_promoted_tile(0, _promoted_tile_id)
	_phase = Phase.WAIT_POLICY
	_policy.scan_once()
	var stats := _policy.stats()
	_expect(int(stats.get("demotions_started", 0)) == 1,
		"policy did not start exactly one quiet-tile demotion")


func _on_policy_demotion_completed(cell: int, tile_id: int, report: Dictionary) -> void:
	if _phase != Phase.WAIT_POLICY:
		return
	_expect(cell == 0, "policy demoted wrong coarse cell")
	_expect(tile_id == _promoted_tile_id, "policy demoted wrong fine tile")
	_expect_close(float(report.get("fine_volume_m3", -1.0)), _promoted_m3,
		"policy demotion reducer volume")
	_expect(_policy.tracked_tile_count() == 0,
		"successful policy collapse left tile in automatic registry")
	_expect(_scheduler.pool.allocated_count() == 0,
		"successful policy collapse left sparse tile resident")
	_expect_close(_store.total_storage_m3(), _baseline,
		"automatic promotion/demotion round trip coarse storage")
	_expect_close(_store.mass_error_m3(), 0.0,
		"automatic round trip broke representation ledger")

	_diag = SparseHydroVolumeDiagnosticsGPU.new()
	add_child(_diag)
	_diag.initialized.connect(_on_diag_initialized)
	_diag.initialization_failed.connect(func(error: Error):
		_fail("final diagnostic init failed (%d)" % int(error)); _finish())
	_diag.volume_ready.connect(_on_final_volume)
	_diag.readback_failed.connect(func(_id: int, error: Error):
		_fail("final diagnostic readback failed (%d)" % int(error)); _finish())
	_phase = Phase.WAIT_FINAL_DIAG
	var err := _diag.initialize_from_atlas(_atlas)
	if err != OK:
		_fail("final diagnostic submit failed (%d)" % int(err)); _finish()


func _on_diag_initialized() -> void:
	if _phase != Phase.WAIT_FINAL_DIAG:
		return
	_phase = Phase.WAIT_FINAL_VOLUME
	if _diag.request_volume() < 0:
		_fail("final sparse volume request rejected"); _finish()


func _on_final_volume(_request_id: int, volume_m3: float) -> void:
	if _phase != Phase.WAIT_FINAL_VOLUME:
		return
	_expect_close(volume_m3, 0.0,
		"automatic collapse left authoritative sparse water")
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
		_fail("store init failed (%d)" % int(err))
		return null
	store.set_climatology_fallback_enabled(false)
	return store


func _expect(condition: bool, message: String) -> void:
	if not condition:
		_fail(message)


func _expect_close(value: float, reference: float, label: String) -> void:
	var abs_error := absf(value - reference)
	var rel_error := abs_error / maxf(absf(reference), 1.0)
	if abs_error > ABS_TOL_M3 and rel_error > REL_TOL:
		_fail("%s: got %.12g expected %.12g (abs %.6g rel %.6g)" % [
			label, value, reference, abs_error, rel_error])


func _fail(message: String) -> void:
	if not _failures.has(message):
		_failures.append(message)


func _finish() -> void:
	if _phase == Phase.DONE:
		return
	_phase = Phase.DONE
	if _diag != null and is_instance_valid(_diag):
		_diag.release()
	if _demotion != null and is_instance_valid(_demotion):
		_demotion.release()
	if _promotion != null and is_instance_valid(_promotion):
		_promotion.release()
	if _identity != null and is_instance_valid(_identity):
		_identity.unbind()
	if _connectivity != null and is_instance_valid(_connectivity):
		_connectivity.release()
	if _atlas != null and is_instance_valid(_atlas):
		_atlas.release()
	if _failures.is_empty():
		print("HYDRO_AUTOMATIC_SURFACE_DEMOTION_GPU: PASS")
		get_tree().quit(0)
	else:
		for failure in _failures:
			push_error("HYDRO_AUTOMATIC_SURFACE_DEMOTION_GPU: " + failure)
		get_tree().quit(1)
