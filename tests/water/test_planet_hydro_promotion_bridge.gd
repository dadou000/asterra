extends Node
## Renderer-mode end-to-end conservation gate for PlanetHydroPromotionBridge.
##
## Uses the real sparse atlas, scheduler, identity bridge, connectivity buffers,
## exact-volume seed kernel and coarse ownership store. Terrain staging is replaced
## by a deterministic dry-bed stub so this numerical ownership test does not depend
## on a full procedural planet bake.
##
## Exit code 0 = pass, 1 = failure. If the global RenderingDevice is unavailable,
## the test prints SKIP and exits 0 like the other GPU water gates.

const TEST_RES := 2
const TEST_RADIUS_M := 1000.0
const TILE_RES := 8
const TILE_DX_M := 2.0
const TRANSFER_M3 := 64.0
const ABS_TOL_M3 := 2.0e-4
const REL_TOL := 5.0e-6
const TIMEOUT_FRAMES := 1200

var _failures: Array[String] = []
var _store: PlanetHydrologyOwnershipStore
var _scheduler: SparseHydroScheduler
var _atlas: SparseHydroAtlasGPU
var _identity: SparseHydroIdentityBridge
var _connectivity: SparseHydroConnectivityGPU
var _terrain: DryTerrainStage
var _bridge: PlanetHydroPromotionBridge
var _diag_a: HydroVolumeDiagnosticsGPU
var _diag_b: HydroVolumeDiagnosticsGPU
var _coarse_before := 0.0
var _represented_m3 := 0.0
var _promotion_report: Dictionary = {}
var _volume_a := NAN
var _volume_b := NAN
var _frames_waited := 0

enum Phase {
	WAIT_ATLAS,
	WAIT_CONNECTIVITY,
	WAIT_BRIDGE,
	WAIT_PROMOTION,
	WAIT_DIAGNOSTICS_INIT,
	WAIT_VOLUMES,
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
		# h/hu/hv/bed are all zero: a deterministic flat dry destination.
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
		print("PLANET_HYDRO_PROMOTION_BRIDGE: SKIP (RenderingDevice unavailable)")
		get_tree().quit(0)
		return

	_store = _make_store()
	if _store == null:
		_finish()
		return
	var cell := 0
	_store.surface_storage_m3[cell] = 80.0
	_store.channel_storage_m3[cell] = 20.0
	_store.initial_storage_m3 = _store.total_storage_m3()
	_coarse_before = _store.total_storage_m3()

	_scheduler = SparseHydroScheduler.new(1)
	_scheduler.freeze_wet_tiles = false
	_atlas = SparseHydroAtlasGPU.new()
	_atlas.name = "SparseHydroAtlasGPU"
	add_child(_atlas)
	_atlas.initialized.connect(_on_atlas_initialized)
	_atlas.initialization_failed.connect(func(error: Error):
		_fail("atlas initialization failed (%d)" % int(error)))
	var err := _atlas.initialize(1, TILE_RES, TILE_DX_M)
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
		_fail("connectivity initialization failed (%d)" % int(error)))
	_phase = Phase.WAIT_CONNECTIVITY
	var err := _connectivity.initialize(1)
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
		_fail("promotion bridge initialization failed (%d)" % int(error)))
	_bridge.promotion_completed.connect(_on_promotion_completed)
	_bridge.promotion_failed.connect(func(_request_id: int, error: Error, stage: String):
		_fail("promotion failed at %s (%d)" % [stage, int(error)])
		_finish())
	_phase = Phase.WAIT_BRIDGE
	var err := _bridge.initialize(_store, _scheduler, _atlas, _connectivity,
		_identity, _terrain, null)
	if err != OK:
		_fail("promotion bridge initialization submit failed (%d)" % int(err))
		_finish()


func _on_bridge_initialized() -> void:
	if _phase != Phase.WAIT_BRIDGE:
		return
	var request_id := _bridge.promote_cell(0, TRANSFER_M3)
	if request_id < 0:
		_fail("promotion request was rejected")
		_finish()
		return
	_phase = Phase.WAIT_PROMOTION


func _on_promotion_completed(_request_id: int, report: Dictionary) -> void:
	if _phase != Phase.WAIT_PROMOTION:
		return
	_promotion_report = report.duplicate(true)
	_represented_m3 = float(report.get("represented_volume_m3", -1.0))
	var slot := int(report.get("slot", -1))
	_expect(slot == 0, "single-slot atlas promotion did not use slot 0")
	_expect(_scheduler.pool.contains(HydroTileKey.unpack(int(report.get("tile_id", -1)))),
		"promoted tile is not resident after acknowledgement")
	_expect(_store.pending_promotion_count() == 0,
		"promotion left unresolved coarse ownership")
	_expect_close(_store.total_storage_m3(), _coarse_before - _represented_m3,
		ABS_TOL_M3, "coarse debit does not match represented fine parcel")
	_expect_close(_store.mass_error_m3(), 0.0, ABS_TOL_M3,
		"coarse ownership ledger is not closed after promotion")

	_diag_a = HydroVolumeDiagnosticsGPU.new()
	_diag_a.name = "VolumeA"
	add_child(_diag_a)
	_diag_b = HydroVolumeDiagnosticsGPU.new()
	_diag_b.name = "VolumeB"
	add_child(_diag_b)
	_diag_a.initialized.connect(_try_request_volumes)
	_diag_b.initialized.connect(_try_request_volumes)
	_diag_a.initialization_failed.connect(func(error: Error):
		_fail("A volume diagnostic initialization failed (%d)" % int(error)))
	_diag_b.initialization_failed.connect(func(error: Error):
		_fail("B volume diagnostic initialization failed (%d)" % int(error)))
	_diag_a.volume_ready.connect(_on_volume_a)
	_diag_b.volume_ready.connect(_on_volume_b)
	_diag_a.readback_failed.connect(func(_id: int, error: Error):
		_fail("A volume readback failed (%d)" % int(error)); _finish())
	_diag_b.readback_failed.connect(func(_id: int, error: Error):
		_fail("B volume readback failed (%d)" % int(error)); _finish())
	_phase = Phase.WAIT_DIAGNOSTICS_INIT
	var err_a := _diag_a.initialize(_atlas.state_a_rid(), TILE_RES, TILE_RES, TILE_DX_M)
	var err_b := _diag_b.initialize(_atlas.state_b_rid(), TILE_RES, TILE_RES, TILE_DX_M)
	if err_a != OK or err_b != OK:
		_fail("volume diagnostic submit failed (A=%d B=%d)" % [int(err_a), int(err_b)])
		_finish()


func _try_request_volumes() -> void:
	if _phase != Phase.WAIT_DIAGNOSTICS_INIT or not _diag_a.initialized_ok() \
			or not _diag_b.initialized_ok():
		return
	_phase = Phase.WAIT_VOLUMES
	if _diag_a.request_volume() < 0 or _diag_b.request_volume() < 0:
		_fail("volume request was rejected")
		_finish()


func _on_volume_a(_request_id: int, volume_m3: float) -> void:
	_volume_a = volume_m3
	_try_finish_volume_gate()


func _on_volume_b(_request_id: int, volume_m3: float) -> void:
	_volume_b = volume_m3
	_try_finish_volume_gate()


func _try_finish_volume_gate() -> void:
	if _phase != Phase.WAIT_VOLUMES or is_nan(_volume_a) or is_nan(_volume_b):
		return
	_expect_relative(_volume_a, _represented_m3, REL_TOL,
		"state A fine volume differs from acknowledged parcel")
	_expect_relative(_volume_b, _represented_m3, REL_TOL,
		"state B fine volume differs from acknowledged parcel")
	_expect_relative(_volume_a, _volume_b, REL_TOL,
		"ping-pong seed volumes differ")
	_expect_close(_store.total_storage_m3() + _volume_a, _coarse_before,
		maxf(ABS_TOL_M3, _coarse_before * REL_TOL),
		"coarse + authoritative fine volume is not conserved")

	# Representation return: unpublish the fine tile first, then return the exact
	# acknowledged parcel to coarse storage. Raw slot bytes may remain stale but are
	# no longer authoritative and must be overwritten before slot reuse.
	var key := HydroTileKey.unpack(int(_promotion_report.get("tile_id", -1)))
	_expect(_scheduler.force_release(key, "promotion_bridge_test_demote"),
		"failed to unpublish fine tile for demotion")
	var demoted := _store.accept_demotion(0, _represented_m3, 0.0)
	_expect(int(demoted.get("error", FAILED)) == OK,
		"coarse store rejected returned fine parcel")
	_expect_close(_store.total_storage_m3(), _coarse_before, ABS_TOL_M3,
		"promotion/demotion round trip did not restore coarse storage")
	_expect_close(_store.mass_error_m3(), 0.0, ABS_TOL_M3,
		"promotion/demotion round trip broke ownership ledger")
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


func _expect_relative(value: float, reference: float, tolerance: float,
		message: String) -> void:
	var error := absf(value - reference) / maxf(absf(reference), 1.0e-12)
	if error > tolerance:
		_fail("%s: got %.12g expected %.12g (rel %.6g > %.6g)" % [
			message, value, reference, error, tolerance])


func _fail(message: String) -> void:
	if not _failures.has(message):
		_failures.append(message)


func _finish() -> void:
	if _phase == Phase.DONE:
		return
	_phase = Phase.DONE
	if _bridge != null and is_instance_valid(_bridge):
		_bridge.release()
	if _identity != null and is_instance_valid(_identity):
		_identity.unbind()
	if _connectivity != null and is_instance_valid(_connectivity):
		_connectivity.release()
	if _atlas != null and is_instance_valid(_atlas):
		_atlas.release()
	if _diag_a != null and is_instance_valid(_diag_a):
		_diag_a.release()
	if _diag_b != null and is_instance_valid(_diag_b):
		_diag_b.release()
	if _failures.is_empty():
		print("PLANET_HYDRO_PROMOTION_BRIDGE: PASS")
		get_tree().quit(0)
	else:
		for failure in _failures:
			push_error("PLANET_HYDRO_PROMOTION_BRIDGE: " + failure)
		get_tree().quit(1)
