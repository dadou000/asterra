extends Node
## Renderer-mode round-trip conservation gate:
## persistent coarse -> exact sparse seed -> compact tile reduction -> coarse.

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
var _global_diag: SparseHydroVolumeDiagnosticsGPU
var _baseline := 0.0
var _promoted_m3 := 0.0
var _frames := 0

enum Phase {
	WAIT_ATLAS,
	WAIT_CONNECTIVITY,
	WAIT_PROMOTION_BRIDGE,
	WAIT_PROMOTION,
	WAIT_DEMOTION_BRIDGE,
	WAIT_DEMOTION,
	WAIT_GLOBAL_DIAG,
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


func _ready() -> void:
	if RenderingServer.get_rendering_device() == null:
		print("PLANET_HYDRO_DEMOTION_BRIDGE: SKIP (RenderingDevice unavailable)")
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

	_scheduler = SparseHydroScheduler.new(1)
	_scheduler.freeze_wet_tiles = false
	_atlas = SparseHydroAtlasGPU.new()
	add_child(_atlas)
	_atlas.initialized.connect(_on_atlas_initialized)
	_atlas.initialization_failed.connect(func(error: Error):
		_fail("atlas init failed (%d)" % int(error)); _finish())
	var err := _atlas.initialize(1, TILE_RES, TILE_DX_M)
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
	var err := _connectivity.initialize(1)
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
	_expect(_promoted_m3 > 0.0, "promotion reported non-positive volume")
	_expect(_scheduler.pool.allocated_count() == 1,
		"promotion did not leave one fine tile resident")
	_expect_close(_store.total_storage_m3(), _baseline - _promoted_m3,
		"coarse debit after promotion")

	_demotion = PlanetHydroDemotionBridge.new()
	add_child(_demotion)
	_demotion.initialized.connect(_on_demotion_initialized)
	_demotion.initialization_failed.connect(func(error: Error):
		_fail("demotion bridge init failed (%d)" % int(error)); _finish())
	_demotion.demotion_completed.connect(_on_demotion_completed)
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
	_phase = Phase.WAIT_DEMOTION
	if _demotion.demote_cell(0) < 0:
		_fail("demotion request rejected"); _finish()


func _on_demotion_completed(_request_id: int, report: Dictionary) -> void:
	if _phase != Phase.WAIT_DEMOTION:
		return
	var reduced := float(report.get("fine_volume_m3", -1.0))
	_expect_close(reduced, _promoted_m3, "tile reducer differs from promoted parcel")
	_expect_close(float(report.get("surface_volume_m3", -1.0)), reduced,
		"demotion did not classify fine flood water as surface storage")
	_expect_close(float(report.get("channel_volume_m3", -1.0)), 0.0,
		"demotion unexpectedly returned water to channel storage")
	_expect(_store.pending_ownership_transaction_count() == 0,
		"demotion left unresolved ownership")
	_expect(_scheduler.pool.allocated_count() == 0,
		"demotion left fine tile resident")
	_expect_close(_store.total_storage_m3(), _baseline,
		"promotion/demotion round trip did not restore coarse storage")
	_expect_close(_store.surface_storage_m3[0], 80.0,
		"round trip changed coarse surface storage classification")
	_expect_close(_store.channel_storage_m3[0], 20.0,
		"round trip changed coarse channel storage")
	_expect_close(_store.cumulative_promoted_to_fine_m3, _promoted_m3,
		"promotion representation ledger")
	_expect_close(_store.cumulative_demoted_from_fine_m3, reduced,
		"demotion representation ledger")
	_expect_close(_store.mass_error_m3(), 0.0,
		"round trip broke coarse representation mass ledger")

	_global_diag = SparseHydroVolumeDiagnosticsGPU.new()
	add_child(_global_diag)
	_global_diag.initialized.connect(_on_global_diag_initialized)
	_global_diag.initialization_failed.connect(func(error: Error):
		_fail("global fine diagnostic init failed (%d)" % int(error)); _finish())
	_global_diag.volume_ready.connect(_on_final_volume)
	_global_diag.readback_failed.connect(func(_id: int, error: Error):
		_fail("global fine volume readback failed (%d)" % int(error)); _finish())
	_phase = Phase.WAIT_GLOBAL_DIAG
	var err := _global_diag.initialize_from_atlas(_atlas)
	if err != OK:
		_fail("global fine diagnostic submit failed (%d)" % int(err)); _finish()


func _on_global_diag_initialized() -> void:
	if _phase != Phase.WAIT_GLOBAL_DIAG:
		return
	_phase = Phase.WAIT_FINAL_VOLUME
	if _global_diag.request_volume() < 0:
		_fail("final fine volume request rejected"); _finish()


func _on_final_volume(_request_id: int, volume_m3: float) -> void:
	if _phase != Phase.WAIT_FINAL_VOLUME:
		return
	_expect_close(volume_m3, 0.0,
		"unoccupied sparse atlas retained authoritative fine water")
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
	if _global_diag != null and is_instance_valid(_global_diag):
		_global_diag.release()
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
		print("PLANET_HYDRO_DEMOTION_BRIDGE: PASS")
		get_tree().quit(0)
	else:
		for failure in _failures:
			push_error("PLANET_HYDRO_DEMOTION_BRIDGE: " + failure)
		get_tree().quit(1)
