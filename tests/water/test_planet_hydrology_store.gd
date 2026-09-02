extends Node
## CPU-only conservation/ownership gate for the persistent planetary hydrology store.
## Exit code 0 = pass, 1 = failure. No RenderingDevice is required.

const TEST_RES := 2
const TEST_RADIUS_M := 1000.0
const ABS_TOL_M3 := 1.0e-7
const REL_TOL := 1.0e-9

var _failures: Array[String] = []


func _ready() -> void:
	_test_zero_forcing_conservation()
	_test_uniform_precipitation_accounting()
	_test_snapshot_round_trip()
	_test_transactional_ownership_transfer()
	if _failures.is_empty():
		print("PLANET_HYDROLOGY_STORE: PASS")
		get_tree().quit(0)
	else:
		for failure in _failures:
			push_error("PLANET_HYDROLOGY_STORE: " + failure)
		get_tree().quit(1)


func _test_zero_forcing_conservation() -> void:
	var store := _make_store("zero forcing")
	if store == null:
		return
	var initial := store.total_storage_m3()
	var report := store.step(600.0)
	_expect(int(report.get("error", FAILED)) == OK,
		"zero-forcing step was rejected")
	_expect_close(store.total_storage_m3(), initial, ABS_TOL_M3,
		"zero forcing changed total storage")
	_expect_close(store.mass_error_m3(), 0.0, ABS_TOL_M3,
		"zero forcing produced a mass-ledger error")


func _test_uniform_precipitation_accounting() -> void:
	var store := _make_store("uniform precipitation")
	if store == null:
		return
	var rain_mps := 2.5e-6
	var rain := PackedFloat64Array()
	rain.resize(store.cell_count())
	rain.fill(rain_mps)
	_expect(store.set_precipitation_field_mps(rain) == OK,
		"uniform precipitation field was rejected")

	var land_area_m2 := 0.0
	for c in store.cell_count():
		if store.fields.elev[c] > 0.0:
			land_area_m2 += store.area_m2[c]
	var duration_s := 900.0
	var report := store.step(duration_s)
	if int(report.get("error", FAILED)) != OK:
		_failures.append("uniform-precipitation step was rejected")
		return
	var expected_m3 := rain_mps * duration_s * land_area_m2
	_expect_relative(store.total_storage_m3(), expected_m3, REL_TOL,
		"uniform precipitation volume mismatch")
	_expect_relative(float(report.get("precipitation_input_m3", -1.0)), expected_m3,
		REL_TOL, "uniform precipitation ledger input mismatch")
	_expect_close(store.mass_error_m3(), 0.0,
		maxf(ABS_TOL_M3, expected_m3 * REL_TOL),
		"uniform precipitation produced a mass-ledger error")


func _test_snapshot_round_trip() -> void:
	var source := _make_store("snapshot source")
	if source == null:
		return
	var rain := PackedFloat64Array()
	rain.resize(source.cell_count())
	for c in source.cell_count():
		rain[c] = 1.0e-6 * (1.0 + float(c % 5) * 0.2)
	source.set_precipitation_field_mps(rain)
	for _step in 3:
		var report := source.step(240.0)
		if int(report.get("error", FAILED)) != OK:
			_failures.append("snapshot source step was rejected")
			return
	var saved := source.snapshot()
	_expect(not saved.is_empty(), "snapshot source returned an empty snapshot")

	var restored := _make_store("snapshot target")
	if restored == null:
		return
	var err := restored.restore_snapshot(saved)
	_expect(err == OK, "snapshot restore failed (%d)" % int(err))
	if err != OK:
		return
	_expect_packed_close(restored.soil_water_m, source.soil_water_m,
		"snapshot soil water")
	_expect_packed_close(restored.surface_storage_m3, source.surface_storage_m3,
		"snapshot surface storage")
	_expect_packed_close(restored.channel_storage_m3, source.channel_storage_m3,
		"snapshot channel storage")
	_expect_packed_close(restored.channel_discharge_m3s, source.channel_discharge_m3s,
		"snapshot channel discharge")
	_expect_close(restored.total_storage_m3(), source.total_storage_m3(), ABS_TOL_M3,
		"snapshot total storage")
	_expect_close(restored.mass_error_m3(), source.mass_error_m3(), ABS_TOL_M3,
		"snapshot mass ledger")
	_expect(restored.step_count == source.step_count,
		"snapshot step count mismatch")
	_expect_close(restored.simulated_seconds, source.simulated_seconds, 1.0e-9,
		"snapshot simulated time mismatch")


func _test_transactional_ownership_transfer() -> void:
	var store := _make_store("ownership transfer")
	if store == null:
		return
	var cell := 0
	# Establish a known coarse ownership baseline. This is test fixture setup, so
	# reset initial_storage_m3 after injecting the fixture water directly.
	store.surface_storage_m3[cell] = 80.0
	store.channel_storage_m3[cell] = 20.0
	store.initial_storage_m3 = store.total_storage_m3()
	var baseline := store.total_storage_m3()

	var prepared := store.prepare_promotion(cell, 90.0)
	_expect(int(prepared.get("error", FAILED)) == OK,
		"promotion prepare failed")
	if int(prepared.get("error", FAILED)) != OK:
		return
	var transaction_id := int(prepared["transaction_id"])
	_expect_close(store.total_storage_m3(), baseline, ABS_TOL_M3,
		"prepare debited physical coarse storage")
	_expect_close(store.reserved_promotion_volume_m3(cell), 90.0, ABS_TOL_M3,
		"prepare reserved wrong volume")
	_expect(store.snapshot().is_empty(),
		"snapshot did not fail closed during unresolved ownership")

	var over_reserved := store.prepare_promotion(cell, 11.0)
	_expect(int(over_reserved.get("error", OK)) != OK,
		"over-reservation was accepted")
	_expect(store.pending_promotion_count() == 1,
		"over-reservation changed pending transaction count")

	var blocked_step := store.step(60.0)
	_expect(int(blocked_step.get("error", OK)) == ERR_BUSY,
		"coarse step advanced while ownership was unresolved")
	_expect_close(store.total_storage_m3(), baseline, ABS_TOL_M3,
		"blocked coarse step changed storage")
	_expect_close(store.simulated_seconds, 0.0, 1.0e-12,
		"blocked coarse step advanced simulated time")

	var rolled_back := store.rollback_promotion(transaction_id)
	_expect(int(rolled_back.get("error", FAILED)) == OK,
		"promotion rollback failed")
	_expect(store.pending_promotion_count() == 0,
		"rollback left a pending transaction")
	_expect_close(store.reserved_promotion_volume_m3(cell), 0.0, ABS_TOL_M3,
		"rollback left reserved water")
	_expect_close(store.total_storage_m3(), baseline, ABS_TOL_M3,
		"rollback changed physical storage")

	prepared = store.prepare_promotion(cell, 90.0)
	transaction_id = int(prepared.get("transaction_id", -1))
	var committed := store.commit_promotion(transaction_id)
	_expect(int(committed.get("error", FAILED)) == OK,
		"promotion commit failed")
	_expect_close(store.total_storage_m3(), baseline - 90.0, ABS_TOL_M3,
		"promotion commit debited wrong volume")
	_expect_close(store.cumulative_promoted_to_fine_m3, 90.0, ABS_TOL_M3,
		"promotion transfer ledger mismatch")
	_expect_close(store.mass_error_m3(), 0.0, ABS_TOL_M3,
		"promotion commit broke coarse mass accounting")

	var after_commit := store.total_storage_m3()
	var duplicate := store.commit_promotion(transaction_id)
	_expect(int(duplicate.get("error", OK)) != OK,
		"duplicate promotion commit was accepted")
	_expect_close(store.total_storage_m3(), after_commit, ABS_TOL_M3,
		"duplicate promotion commit debited storage twice")

	var committed_snapshot := store.snapshot()
	_expect(not committed_snapshot.is_empty(),
		"committed ownership snapshot was blocked")
	var pending_restore_test := store.prepare_promotion(cell, 5.0)
	_expect(int(pending_restore_test.get("error", FAILED)) == OK,
		"restore fixture reservation failed")
	var restore_err := store.restore_snapshot(committed_snapshot)
	_expect(restore_err == OK,
		"restore with pending reservation failed (%d)" % int(restore_err))
	_expect(store.pending_promotion_count() == 0,
		"snapshot restore resurrected a pending promotion")
	_expect_close(store.reserved_promotion_volume_m3(), 0.0, ABS_TOL_M3,
		"snapshot restore retained reservation bookkeeping")
	_expect_close(store.cumulative_promoted_to_fine_m3, 90.0, ABS_TOL_M3,
		"snapshot restore lost promotion ledger")

	# Return the exact parcel from a future collapsed fine domain. This closes the
	# representation round-trip without counting it as environmental input.
	var demoted := store.accept_demotion(cell, 80.0, 10.0)
	_expect(int(demoted.get("error", FAILED)) == OK,
		"fine-to-coarse demotion acceptance failed")
	_expect_close(store.total_storage_m3(), baseline, ABS_TOL_M3,
		"demotion did not restore coarse volume")
	_expect_close(store.cumulative_demoted_from_fine_m3, 90.0, ABS_TOL_M3,
		"demotion transfer ledger mismatch")
	_expect_close(store.mass_error_m3(), 0.0, ABS_TOL_M3,
		"promotion/demotion round-trip broke mass accounting")


func _make_store(label: String) -> PlanetHydrologyOwnershipStore:
	var cfg := GenConfig.new()
	cfg.face_res = TEST_RES
	cfg.planet_radius = TEST_RADIUS_M
	var grid := PlanetGrid.new(TEST_RES, TEST_RADIUS_M)
	var fields := PlanetFields.new(cfg, grid)
	# Every test cell is a closed land basin. Using 255 forces receiver=self and
	# makes the expected global volume independent of drainage ordering.
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
		_failures.append("%s store initialization failed (%d)" % [label, int(err)])
		return null
	store.set_climatology_fallback_enabled(false)
	return store


func _expect(condition: bool, message: String) -> void:
	if not condition:
		_failures.append(message)


func _expect_close(value: float, reference: float, tolerance: float,
		message: String) -> void:
	var error := absf(value - reference)
	if error > tolerance:
		_failures.append("%s: got %.12g expected %.12g (abs %.6g > %.6g)" % [
			message, value, reference, error, tolerance])


func _expect_relative(value: float, reference: float, tolerance: float,
		message: String) -> void:
	var error := absf(value - reference) / maxf(absf(reference), 1.0e-12)
	if error > tolerance:
		_failures.append("%s: got %.12g expected %.12g (rel %.6g > %.6g)" % [
			message, value, reference, error, tolerance])


func _expect_packed_close(value: PackedFloat64Array, reference: PackedFloat64Array,
		message: String) -> void:
	if value.size() != reference.size():
		_failures.append("%s size mismatch: %d != %d" % [
			message, value.size(), reference.size()])
		return
	var max_error := 0.0
	for i in value.size():
		max_error = maxf(max_error, absf(value[i] - reference[i]))
	if max_error > ABS_TOL_M3:
		_failures.append("%s max absolute error %.6g" % [message, max_error])
