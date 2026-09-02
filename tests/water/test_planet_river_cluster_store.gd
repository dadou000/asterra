extends Node
## CPU/headless gate for one coarse refinement hole represented by N sparse members.

const TEST_RES := 2
const TEST_RADIUS_M := 1000.0
const BASELINE_Q := 10.0
const ABS_TOL := 1.0e-6

var _failures: Array[String] = []


func _ready() -> void:
	var fixture := _make_fixture()
	if not fixture.is_empty():
		_run(fixture)
	if _failures.is_empty():
		print("PLANET_RIVER_CLUSTER_STORE: PASS")
		get_tree().quit(0)
	else:
		for failure in _failures:
			push_error("PLANET_RIVER_CLUSTER_STORE: " + failure)
		get_tree().quit(1)


func _run(fixture: Dictionary) -> void:
	var store := fixture["store"] as PlanetHydrologyRiverClusterStore
	var cell := int(fixture["source"])
	var total_before := store.total_storage_m3()
	var channel_before := store.channel_storage_m3[cell]
	var transfer := minf(channel_before * 0.15, 30.0)
	var prepared := store.prepare_channel_promotion(cell, transfer)
	_expect(int(prepared.get("error", FAILED)) == OK, "cluster channel prepare failed")
	if int(prepared.get("error", FAILED)) != OK:
		return
	var committed := store.commit_promotion(int(prepared["transaction_id"]))
	_expect(int(committed.get("error", FAILED)) == OK, "cluster channel commit failed")

	var k0 := HydroTileKey.new(CubeSphere.FACE_PX, 6, 20, 20)
	var k1 := HydroTileTopology.neighbor(k0, HydroTileTopology.DIR_EAST).get("key") as HydroTileKey
	var k2 := HydroTileTopology.neighbor(k1, HydroTileTopology.DIR_EAST).get("key") as HydroTileKey
	var members: Array[Dictionary] = [
		{"tile_id": k0.packed(), "slot": 2},
		{"tile_id": k1.packed(), "slot": 7},
		{"tile_id": k2.packed(), "slot": 11},
	]
	var registered := store.register_refined_cluster(cell, members, transfer)
	_expect(int(registered.get("error", FAILED)) == OK, "cluster registration failed")
	_expect(store.refined_reach_count() == 1, "cluster created more than one coarse hole")
	_expect(store.refined_cluster_size(cell) == 3, "cluster member count is not three")
	var record := store.refined_reach_record(cell)
	_expect(int(record.get("tile_id", -1)) == k0.packed(),
		"legacy primary identity is not upstream member")
	_expect(int(record.get("member_count", 0)) == 3, "cluster record lost member count")
	_expect_close(store.total_storage_m3() + transfer, total_before, ABS_TOL,
		"coarse+fine ownership did not close after cluster registration")

	# Pending donor water remains coarse-owned while the cluster exists.
	var pending := minf(5.0, store.channel_storage_m3[cell])
	store.channel_storage_m3[cell] -= pending
	store.refined_pending_inflow_m3[cell] += pending
	var total_with_pending := store.total_storage_m3()
	_expect_close(total_with_pending + transfer, total_before, ABS_TOL,
		"moving residual channel water to pending queue changed ownership total")
	var unregistered := store.unregister_refined_reach(cell, true)
	_expect(int(unregistered.get("error", FAILED)) == OK, "cluster unregister failed")
	_expect_close(float(unregistered.get("returned_pending_m3", -1.0)), pending,
		ABS_TOL, "pending cluster inflow was not returned")
	_expect(not store.is_refined_reach(cell), "cluster coarse hole survived unregister")
	var returned := store.accept_demotion(cell, 0.0, transfer)
	_expect(int(returned.get("error", FAILED)) == OK, "cluster fine volume return failed")
	_expect_close(store.channel_storage_m3[cell], channel_before, ABS_TOL,
		"cluster ownership round trip did not restore channel storage")
	_expect_close(store.total_storage_m3(), total_before, ABS_TOL,
		"cluster ownership round trip changed total coarse water")
	_expect_close(store.mass_error_m3(), 0.0, ABS_TOL,
		"cluster ownership ledger did not close")


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
		_fail("fixture has no downstream neighbor")
		return {}
	fields.flow_dir[source] = direction_slot
	fields.elev[source] = 110.0
	fields.elev[destination] = 90.0
	fields.discharge[source] = BASELINE_Q
	fields.stream_order[source] = 3
	fields.river_width[source] = 7.2 * sqrt(BASELINE_Q)
	var store := PlanetHydrologyRiverClusterStore.new()
	var err := store.initialize(fields)
	if err != OK:
		_fail("cluster store initialization failed (%d)" % int(err))
		return {}
	store.set_climatology_fallback_enabled(false)
	store.initial_storage_m3 = store.total_storage_m3()
	return {"store": store, "source": source}


func _expect(condition: bool, message: String) -> void:
	if not condition:
		_fail(message)


func _expect_close(value: float, expected: float, tolerance: float, message: String) -> void:
	if absf(value - expected) > tolerance:
		_fail("%s: got %.12g expected %.12g tolerance %.6g" % [
			message, value, expected, tolerance])


func _fail(message: String) -> void:
	if not _failures.has(message):
		_failures.append(message)
