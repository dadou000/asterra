extends Node
## CPU/headless gate for the production hybrid coarse channel model.
##
## Verifies generated-width river cells become calibrated 1D Manning reaches,
## baseline discharge is reproduced by normal depth, routing moves at most one
## macro cell per step, over-bank storage is returned to coarse surface water, and
## all transfers remain inside the existing PlanetHydrologyStore mass ledger.

const TEST_RES := 2
const TEST_RADIUS_M := 1000.0
const BASELINE_Q := 10.0
const EXCESS_M3 := 50.0
const ABS_TOL := 1.0e-6
const Q_REL_TOL := 2.0e-5

var _failures: Array[String] = []


func _ready() -> void:
	_test_geometry_and_one_hop_routing()
	_test_bankfull_overflow_conservation()
	if _failures.is_empty():
		print("PLANET_RIVER_REACH_STORE: PASS")
		get_tree().quit(0)
	else:
		for failure in _failures:
			push_error("PLANET_RIVER_REACH_STORE: " + failure)
		get_tree().quit(1)


func _test_geometry_and_one_hop_routing() -> void:
	var fixture := _make_fixture()
	if fixture.is_empty():
		return
	var store := fixture["store"] as PlanetHydrologyReachOwnershipStore
	var source := int(fixture["source"])
	var destination := int(fixture["destination"])
	var reaches := store.river_reaches
	_expect(reaches != null and reaches.initialized, "reach store did not initialize")
	_expect(reaches.is_reach_cell(source), "generated-width source was not a 1D reach")
	_expect(not reaches.is_reach_cell(destination),
		"closed downstream fixture unexpectedly became a reach")
	if reaches == null or not reaches.is_reach_cell(source):
		return

	var normal_depth := reaches.normal_depth_m[source]
	var bankfull_depth := reaches.bankfull_depth_m[source]
	_expect(normal_depth > 0.0, "normal depth is not positive")
	_expect(bankfull_depth > normal_depth,
		"bankfull depth is not above normal depth")
	var calibrated_q := reaches.discharge_for_depth(source, normal_depth)
	_expect_relative(calibrated_q, BASELINE_Q, Q_REL_TOL,
		"normal depth does not reproduce generated baseline discharge")
	_expect_close(store.channel_storage_m3[source], reaches.normal_storage_m3[source],
		maxf(ABS_TOL, reaches.normal_storage_m3[source] * 1.0e-10),
		"production initialization did not install 1D normal-flow storage")

	# Remove unrelated vertical sources; only the source reach may move water.
	store.soil_water_m.fill(0.0)
	store.surface_storage_m3.fill(0.0)
	store.clear_precipitation()
	store.set_climatology_fallback_enabled(false)
	store.initial_storage_m3 = store.total_storage_m3()
	var source_before := store.channel_storage_m3[source]
	var destination_before := store.channel_storage_m3[destination]
	var total_before := store.total_storage_m3()
	var report := store.step(10.0)
	_expect(int(report.get("error", FAILED)) == OK, "reach step was rejected")
	var moved := store.channel_discharge_m3s[source] * 10.0
	_expect(moved > 0.0, "normal-flow reach produced no downstream outflow")
	_expect_close(source_before - store.channel_storage_m3[source], moved,
		maxf(ABS_TOL, moved * 1.0e-9), "source debit differs from reported outflow")
	_expect_close(store.channel_storage_m3[destination] - destination_before, moved,
		maxf(ABS_TOL, moved * 1.0e-9), "one-hop receiver did not receive exact outflow")
	_expect_close(store.total_storage_m3(), total_before,
		maxf(ABS_TOL, total_before * 1.0e-10), "closed one-hop routing changed total water")
	_expect_close(store.mass_error_m3(), 0.0,
		maxf(ABS_TOL, total_before * 1.0e-10), "one-hop routing broke mass ledger")


func _test_bankfull_overflow_conservation() -> void:
	var fixture := _make_fixture()
	if fixture.is_empty():
		return
	var store := fixture["store"] as PlanetHydrologyReachOwnershipStore
	var source := int(fixture["source"])
	var reaches := store.river_reaches
	store.soil_water_m.fill(0.0)
	store.surface_storage_m3.fill(0.0)
	store.clear_precipitation()
	store.set_climatology_fallback_enabled(false)
	store.channel_storage_m3[source] = reaches.bankfull_storage_m3[source] + EXCESS_M3
	store.initial_storage_m3 = store.total_storage_m3()
	var total_before := store.total_storage_m3()
	var report := store.step(1.0)
	_expect(int(report.get("error", FAILED)) == OK, "overflow reach step was rejected")
	var spill := float(report.get("river_overbank_spill_m3", 0.0))
	_expect_close(spill, EXCESS_M3, maxf(ABS_TOL, EXCESS_M3 * 1.0e-10),
		"bankfull excess was not transferred exactly to surface storage")
	_expect(store.surface_storage_m3[source] >= EXCESS_M3 - ABS_TOL,
		"overbank parcel is not present in coarse surface storage")
	_expect(store.channel_storage_m3[source] <= reaches.bankfull_storage_m3[source] + ABS_TOL,
		"1D channel retained storage above bankfull after overflow")
	_expect_close(store.total_storage_m3(), total_before,
		maxf(ABS_TOL, total_before * 1.0e-10), "overbank transfer changed total water")
	_expect_close(store.mass_error_m3(), 0.0,
		maxf(ABS_TOL, total_before * 1.0e-10), "overbank transfer broke mass ledger")

	var candidates := store.channel_reach_candidates(8, 1.0, 0.10)
	_expect(not candidates.is_empty(), "reach anomaly query returned no river candidate")
	if not candidates.is_empty():
		_expect(int(candidates[0].get("cell", -1)) == source,
			"reach anomaly query did not rank test river cell")


func _make_fixture() -> Dictionary:
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
		_fail("PlanetGrid did not provide a usable neighbor for reach fixture")
		return {}
	fields.flow_dir[source] = direction_slot
	fields.elev[source] = 110.0
	fields.elev[destination] = 90.0
	fields.discharge[source] = BASELINE_Q
	fields.stream_order[source] = 3
	fields.river_width[source] = 7.2 * sqrt(BASELINE_Q)

	var store := PlanetHydrologyReachOwnershipStore.new()
	var err := store.initialize(fields)
	if err != OK:
		_fail("reach-aware store initialization failed (%d)" % int(err))
		return {}
	store.set_climatology_fallback_enabled(false)
	return {"store": store, "source": source, "destination": destination}


func _expect(condition: bool, message: String) -> void:
	if not condition:
		_fail(message)


func _expect_close(value: float, expected: float, tolerance: float,
		message: String) -> void:
	if absf(value - expected) > tolerance:
		_fail("%s: got %.12g expected %.12g tolerance %.6g" % [
			message, value, expected, tolerance])


func _expect_relative(value: float, expected: float, tolerance: float,
		message: String) -> void:
	var rel := absf(value - expected) / maxf(absf(expected), 1.0e-12)
	if rel > tolerance:
		_fail("%s: got %.12g expected %.12g relative %.6g > %.6g" % [
			message, value, expected, rel, tolerance])


func _fail(message: String) -> void:
	if not _failures.has(message):
		_failures.append(message)
