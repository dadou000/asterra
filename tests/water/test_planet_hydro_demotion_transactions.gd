extends Node
## CPU-only gate for incoming fine->coarse ownership transactions.

const TEST_RES := 2
const TEST_RADIUS_M := 1000.0
const ABS_TOL_M3 := 1.0e-8

var _failures: Array[String] = []


func _ready() -> void:
	var store := _make_store()
	if store != null:
		_test_prepare_rollback(store)
		_test_prepare_commit(store)
	if _failures.is_empty():
		print("PLANET_HYDRO_DEMOTION_TRANSACTIONS: PASS")
		get_tree().quit(0)
	else:
		for failure in _failures:
			push_error("PLANET_HYDRO_DEMOTION_TRANSACTIONS: " + failure)
		get_tree().quit(1)


func _test_prepare_rollback(store: PlanetHydrologyOwnershipStore) -> void:
	var baseline := store.total_storage_m3()
	var prepared := store.prepare_demotion(0, 30.0, 10.0)
	_expect(int(prepared.get("error", FAILED)) == OK,
		"prepare_demotion rejected valid incoming parcel")
	if int(prepared.get("error", FAILED)) != OK:
		return
	var transaction_id := int(prepared.get("transaction_id", -1))
	_expect(transaction_id >= 0, "demotion transaction has invalid id")
	_expect(String(prepared.get("transfer_direction", "")) == "fine_to_coarse",
		"demotion transfer-direction label mismatch")
	_expect(prepared.get("direction", null) is Vector3,
		"demotion changed the legacy direction Vector3 contract")
	_expect(store.pending_demotion_count() == 1,
		"prepared demotion not visible as pending")
	_expect(store.pending_ownership_transaction_count() == 1,
		"ownership pending count did not include demotion")
	_expect_close(store.total_storage_m3(), baseline,
		"prepare_demotion changed physical coarse storage")
	_expect(store.snapshot().is_empty(),
		"snapshot did not fail closed during pending demotion")

	var blocked_step := store.step(60.0)
	_expect(int(blocked_step.get("error", OK)) == ERR_BUSY,
		"coarse step advanced during pending demotion")
	_expect(String(blocked_step.get("reason", "")) == "demotion_transaction_pending",
		"pending demotion reported wrong step-block reason")
	_expect_close(store.total_storage_m3(), baseline,
		"blocked step changed physical storage")

	var blocked_promotion := store.prepare_promotion(0, 1.0)
	_expect(int(blocked_promotion.get("error", OK)) == ERR_BUSY,
		"promotion started while incoming demotion was pending")
	var blocked_demotion := store.prepare_demotion(1, 1.0, 0.0)
	_expect(int(blocked_demotion.get("error", OK)) == ERR_BUSY,
		"second demotion started while ownership was unresolved")

	var rolled_back := store.rollback_demotion(transaction_id)
	_expect(int(rolled_back.get("error", FAILED)) == OK,
		"rollback_demotion failed")
	_expect(store.pending_demotion_count() == 0,
		"rollback_demotion left a pending transaction")
	_expect_close(store.total_storage_m3(), baseline,
		"rollback_demotion changed physical storage")
	_expect_close(store.cumulative_demoted_from_fine_m3, 0.0,
		"rollback_demotion changed representation ledger")
	_expect(not store.snapshot().is_empty(),
		"snapshot remained blocked after demotion rollback")


func _test_prepare_commit(store: PlanetHydrologyOwnershipStore) -> void:
	var baseline := store.total_storage_m3()
	var prepared := store.prepare_demotion(0, 30.0, 10.0)
	var transaction_id := int(prepared.get("transaction_id", -1))
	if transaction_id < 0:
		_failures.append("commit fixture demotion prepare failed")
		return
	var committed := store.commit_demotion(transaction_id)
	_expect(int(committed.get("error", FAILED)) == OK,
		"commit_demotion failed")
	_expect(store.pending_demotion_count() == 0,
		"commit_demotion left a pending transaction")
	_expect_close(store.total_storage_m3(), baseline + 40.0,
		"commit_demotion added wrong physical volume")
	_expect_close(store.surface_storage_m3[0], 30.0,
		"commit_demotion surface classification mismatch")
	_expect_close(store.channel_storage_m3[0], 10.0,
		"commit_demotion channel classification mismatch")
	_expect_close(store.cumulative_demoted_from_fine_m3, 40.0,
		"commit_demotion representation ledger mismatch")
	_expect_close(store.mass_error_m3(), 0.0,
		"commit_demotion broke representation-aware mass ledger")

	var after_commit := store.total_storage_m3()
	var duplicate := store.commit_demotion(transaction_id)
	_expect(int(duplicate.get("error", OK)) != OK,
		"duplicate demotion commit was accepted")
	_expect_close(store.total_storage_m3(), after_commit,
		"duplicate demotion commit added water twice")

	var saved := store.snapshot()
	_expect(not saved.is_empty(), "committed demotion snapshot was blocked")
	var pending := store.prepare_demotion(0, 2.0, 0.0)
	_expect(int(pending.get("error", FAILED)) == OK,
		"restore fixture demotion prepare failed")
	var restore_error := store.restore_snapshot(saved)
	_expect(restore_error == OK,
		"restore_snapshot failed with pending demotion (%d)" % int(restore_error))
	_expect(store.pending_ownership_transaction_count() == 0,
		"snapshot restore resurrected pending demotion bookkeeping")
	_expect_close(store.cumulative_demoted_from_fine_m3, 40.0,
		"snapshot restore lost demotion ledger")
	_expect_close(store.mass_error_m3(), 0.0,
		"snapshot restore broke demotion mass ledger")


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
		_failures.append("store initialization failed (%d)" % int(err))
		return null
	store.set_climatology_fallback_enabled(false)
	return store


func _expect(condition: bool, message: String) -> void:
	if not condition:
		_failures.append(message)


func _expect_close(value: float, reference: float, message: String) -> void:
	var error := absf(value - reference)
	if error > ABS_TOL_M3:
		_failures.append("%s: got %.12g expected %.12g (abs %.6g)" % [
			message, value, reference, error])
