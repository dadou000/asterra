extends Node
## CPU/headless gate for channel-only 1D -> 2D ownership transactions.

const TEST_RES := 2
const TEST_RADIUS_M := 1000.0
const BASELINE_Q := 10.0
const ABS_TOL := 1.0e-6

var _failures: Array[String] = []


func _ready() -> void:
	var fixture := _make_fixture()
	if not fixture.is_empty():
		_run_transactions(fixture)
	if _failures.is_empty():
		print("PLANET_RIVER_CHANNEL_OWNERSHIP: PASS")
		get_tree().quit(0)
	else:
		for failure in _failures:
			push_error("PLANET_RIVER_CHANNEL_OWNERSHIP: " + failure)
		get_tree().quit(1)


func _run_transactions(fixture: Dictionary) -> void:
	var store := fixture["store"] as PlanetHydrologyRiverPromotionStore
	var cell := int(fixture["source"])
	var surface_before := store.surface_storage_m3[cell]
	var channel_before := store.channel_storage_m3[cell]
	var total_before := store.total_storage_m3()
	var requested := minf(channel_before * 0.20, 50.0)
	_expect(requested > 0.0, "fixture has no reservable channel water")

	var prepared := store.prepare_channel_promotion(cell, requested)
	_expect(int(prepared.get("error", FAILED)) == OK,
		"channel reservation was rejected")
	if int(prepared.get("error", FAILED)) != OK:
		return
	var tx := int(prepared["transaction_id"])
	_expect_close(float(prepared.get("reserved_surface_volume_m3", -1.0)), 0.0,
		ABS_TOL, "channel reservation borrowed surface storage")
	_expect_close(float(prepared.get("reserved_channel_volume_m3", -1.0)), requested,
		ABS_TOL, "channel reservation amount differs from request")
	_expect_close(store.surface_storage_m3[cell], surface_before, ABS_TOL,
		"prepare changed surface storage")
	_expect_close(store.channel_storage_m3[cell], channel_before, ABS_TOL,
		"prepare changed channel storage")

	var committed := store.commit_promotion(tx)
	_expect(int(committed.get("error", FAILED)) == OK,
		"channel commit was rejected")
	_expect_close(store.surface_storage_m3[cell], surface_before, ABS_TOL,
		"channel commit debited surface storage")
	_expect_close(store.channel_storage_m3[cell], channel_before - requested,
		maxf(ABS_TOL, requested * 1.0e-10),
		"channel commit did not debit exact requested parcel")
	_expect_close(store.total_storage_m3(), total_before - requested,
		maxf(ABS_TOL, total_before * 1.0e-10),
		"coarse total did not decrease by promoted channel parcel")
	_expect_close(store.mass_error_m3(), 0.0,
		maxf(ABS_TOL, total_before * 1.0e-10),
		"ownership ledger did not close after channel promotion")

	var returned := store.accept_demotion(cell, 0.0, requested)
	_expect(int(returned.get("error", FAILED)) == OK,
		"channel demotion return was rejected")
	_expect_close(store.surface_storage_m3[cell], surface_before, ABS_TOL,
		"channel demotion polluted surface storage")
	_expect_close(store.channel_storage_m3[cell], channel_before,
		maxf(ABS_TOL, channel_before * 1.0e-10),
		"channel promotion/demotion did not restore channel storage")
	_expect_close(store.total_storage_m3(), total_before,
		maxf(ABS_TOL, total_before * 1.0e-10),
		"channel ownership round trip changed total coarse water")
	_expect_close(store.mass_error_m3(), 0.0,
		maxf(ABS_TOL, total_before * 1.0e-10),
		"channel ownership round trip broke ledger")

	# Insufficient channel water must fail even if surface water is abundant.
	store.surface_storage_m3[cell] = 1.0e6
	var too_much := store.available_channel_promotion_volume_m3(cell) + 1.0
	var rejected := store.prepare_channel_promotion(cell, too_much)
	_expect(int(rejected.get("error", OK)) != OK,
		"channel reservation incorrectly borrowed abundant surface water")


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
		_fail("PlanetGrid did not provide a usable reach neighbor")
		return {}
	fields.flow_dir[source] = direction_slot
	fields.elev[source] = 110.0
	fields.elev[destination] = 90.0
	fields.discharge[source] = BASELINE_Q
	fields.stream_order[source] = 3
	fields.river_width[source] = 7.2 * sqrt(BASELINE_Q)

	var store := PlanetHydrologyRiverPromotionStore.new()
	var err := store.initialize(fields)
	if err != OK:
		_fail("river promotion store initialization failed (%d)" % int(err))
		return {}
	store.surface_storage_m3[source] = 25.0
	store.initial_storage_m3 = store.total_storage_m3()
	return {"store": store, "source": source}


func _expect(condition: bool, message: String) -> void:
	if not condition:
		_fail(message)


func _expect_close(value: float, expected: float, tolerance: float,
		message: String) -> void:
	if absf(value - expected) > tolerance:
		_fail("%s: got %.12g expected %.12g tolerance %.6g" % [
			message, value, expected, tolerance])


func _fail(message: String) -> void:
	if not _failures.has(message):
		_failures.append(message)
