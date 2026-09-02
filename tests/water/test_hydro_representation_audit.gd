extends Node
## Coordinated CPU+GPU representation audit gate.
##
## Coarse starts with 100 m3. A 64 m3 promotion is committed into the coarse
## ownership ledger and the sparse atlas independently contains exactly 64 m3.
## The audit must pause both owners, read only four GPU bytes, report 100 m3 total,
## close the strict environmental balance and restore both prior enabled states.

const TEST_RES := 2
const TEST_RADIUS_M := 1000.0
const TILE_RES := 8
const DX_M := 1.0
const FINE_VOLUME_M3 := 64.0
const ABS_TOL_M3 := 2.0e-4
const TIMEOUT_FRAMES := 1200

var _atlas: SparseHydroAtlasGPU
var _diagnostic: SparseHydroVolumeDiagnosticsGPU
var _audit: HydroRepresentationAudit
var _runtime: FakeRuntime
var _coarse_owner: FakeCoarseOwner
var _store: PlanetHydrologyOwnershipStore
var _finished := false
var _frames := 0


class FakeRuntime:
	extends SparseHydrologyRuntime
	func initialized_ok() -> bool:
		return true
	func busy() -> bool:
		return false


class FakeCoarseOwner:
	extends Node
	var enabled := true


func _ready() -> void:
	if RenderingServer.get_rendering_device() == null:
		print("HYDRO_REPRESENTATION_AUDIT: SKIP (RenderingDevice unavailable)")
		get_tree().quit(0)
		return
	_store = _make_store()
	if _store == null:
		return
	_store.surface_storage_m3[0] = 100.0
	_store.initial_storage_m3 = 100.0
	var prepared := _store.prepare_promotion(0, FINE_VOLUME_M3)
	_require(int(prepared.get("error", FAILED)) == OK, "prepare promotion failed")
	if _finished:
		return
	var committed := _store.commit_promotion(int(prepared.get("transaction_id", -1)))
	_require(int(committed.get("error", FAILED)) == OK, "commit promotion failed")
	_require(absf(_store.total_storage_m3() - 36.0) <= 1.0e-9,
		"coarse fixture debit is incorrect")
	if _finished:
		return

	var state := PackedFloat32Array()
	state.resize(TILE_RES * TILE_RES * SparseHydroAtlasGPU.STATE_FLOATS)
	for i in TILE_RES * TILE_RES:
		state[i * 4] = 1.0
	var occupancy := PackedInt32Array([1])
	_atlas = SparseHydroAtlasGPU.new()
	add_child(_atlas)
	_atlas.initialized.connect(_on_atlas_initialized)
	_atlas.initialization_failed.connect(func(error: Error):
		_fail("atlas initialization failed (%d)" % int(error)))
	var err := _atlas.initialize(1, TILE_RES, DX_M, state, occupancy)
	if err != OK:
		_fail("atlas initialization submit failed (%d)" % int(err))


func _process(_delta: float) -> void:
	if _finished:
		return
	_frames += 1
	if _frames > TIMEOUT_FRAMES:
		_fail("timeout")


func _on_atlas_initialized() -> void:
	_diagnostic = SparseHydroVolumeDiagnosticsGPU.new()
	add_child(_diagnostic)
	_diagnostic.initialized.connect(_on_diagnostic_initialized)
	_diagnostic.initialization_failed.connect(func(error: Error):
		_fail("diagnostic initialization failed (%d)" % int(error)))
	var err := _diagnostic.initialize_from_atlas(_atlas)
	if err != OK:
		_fail("diagnostic initialization submit failed (%d)" % int(err))


func _on_diagnostic_initialized() -> void:
	_runtime = FakeRuntime.new()
	_runtime.enabled = true
	add_child(_runtime)
	_coarse_owner = FakeCoarseOwner.new()
	_coarse_owner.enabled = true
	add_child(_coarse_owner)
	_audit = HydroRepresentationAudit.new()
	add_child(_audit)
	_audit.audit_ready.connect(_on_audit_ready)
	_audit.audit_failed.connect(func(_audit_id: int, error: Error, stage: String):
		_fail("audit failed at %s (%d)" % [stage, int(error)]))
	var err := _audit.initialize(_store, _runtime, _diagnostic, _coarse_owner)
	_require(err == OK, "audit initialization failed (%d)" % int(err))
	if _finished:
		return
	var audit_id := _audit.request_audit(true, ABS_TOL_M3, 1.0e-6)
	_require(audit_id > 0, "strict audit request rejected")
	# request_audit pauses both owners synchronously before queueing the readback.
	_require(not _runtime.enabled, "sparse runtime was not paused during audit")
	_require(not _coarse_owner.enabled, "coarse owner was not paused during audit")


func _on_audit_ready(_audit_id: int, report: Dictionary) -> void:
	_require(absf(float(report.get("coarse_storage_m3", -1.0)) - 36.0) <= ABS_TOL_M3,
		"coarse snapshot is incorrect")
	_require(absf(float(report.get("active_sparse_volume_m3", -1.0)) - FINE_VOLUME_M3)
		<= ABS_TOL_M3, "fine GPU volume is incorrect")
	_require(absf(float(report.get("combined_owned_storage_m3", -1.0)) - 100.0)
		<= ABS_TOL_M3, "combined representation volume is incorrect")
	_require(absf(float(report.get("environmental_balance_residual_m3", INF)))
		<= ABS_TOL_M3, "strict environmental residual is nonzero")
	_require(bool(report.get("strict_environmental_balance_testable", false)),
		"strict audit was not marked testable")
	_require(bool(report.get("strict_environmental_balance_pass", false)),
		"strict representation balance did not pass")
	_require(absf(float(report.get("fine_minus_net_promoted_m3", INF))) <= ABS_TOL_M3,
		"fine volume differs from net promoted ownership")
	_require(_runtime.enabled, "sparse runtime enabled state was not restored")
	_require(_coarse_owner.enabled, "coarse owner enabled state was not restored")
	if _finished:
		return
	_finished = true
	print("HYDRO_REPRESENTATION_AUDIT: PASS")
	_cleanup()
	get_tree().quit(0)


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


func _require(condition: bool, message: String) -> void:
	if not condition and not _finished:
		_fail(message)


func _fail(message: String) -> void:
	if _finished:
		return
	_finished = true
	push_error("HYDRO_REPRESENTATION_AUDIT: " + message)
	_cleanup()
	get_tree().quit(1)


func _cleanup() -> void:
	if _audit != null and is_instance_valid(_audit):
		_audit.release()
		_audit.queue_free()
	if _diagnostic != null and is_instance_valid(_diagnostic):
		_diagnostic.release()
		_diagnostic.queue_free()
	if _atlas != null and is_instance_valid(_atlas):
		_atlas.release()
		_atlas.queue_free()
	if _runtime != null and is_instance_valid(_runtime):
		_runtime.queue_free()
	if _coarse_owner != null and is_instance_valid(_coarse_owner):
		_coarse_owner.queue_free()
