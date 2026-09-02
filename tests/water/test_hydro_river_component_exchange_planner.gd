extends Node
## Headless CPU gate for the component readiness contract and multi-mouth exchange
## planner. No RenderingDevice is required: the GPU ABI records are inspected before
## dispatch while coarse ownership remains authoritative.

const TEST_RES := 3
const TEST_RADIUS_M := 1000.0
const BASELINE_Q := 10.0
const CELL_SIZE_M := 4.0

var _failures: Array[String] = []


func _ready() -> void:
	_test_ready_confluence_plan()
	_test_residual_internal_reach_falls_back()
	_test_internal_fine_gap_falls_back()
	if _failures.is_empty():
		print("HYDRO_RIVER_COMPONENT_EXCHANGE_PLANNER: PASS")
		get_tree().quit(0)
	else:
		for failure in _failures:
			push_error("HYDRO_RIVER_COMPONENT_EXCHANGE_PLANNER: " + failure)
		get_tree().quit(1)


func _test_ready_confluence_plan() -> void:
	var fixture := _make_confluence_fixture()
	if fixture.is_empty():
		return
	var store := fixture["store"] as PlanetHydrologyRiverClusterStore
	var a := int(fixture["a"])
	var b := int(fixture["b"])
	var stem := int(fixture["stem"])
	var keys := _connected_confluence_keys()
	_promote_cluster(store, a, keys["a"] as HydroTileKey, 3, true, Vector2.RIGHT)
	_promote_cluster(store, b, keys["b"] as HydroTileKey, 4, true, Vector2.UP)
	_promote_cluster(store, stem, keys["stem"] as HydroTileKey, 5, false, Vector2.RIGHT)
	if not _failures.is_empty():
		return

	var merged := store.merge_refined_clusters(PackedInt32Array([a, b, stem]))
	_expect(int(merged.get("error", FAILED)) == OK, "ready confluence union failed")
	if int(merged.get("error", FAILED)) != OK:
		return
	var component_id := int(merged.get("component_id", -1))
	var contract := HydroRiverComponentCouplingContract.evaluate(store, component_id)
	_expect(int(contract.get("error", FAILED)) == OK and bool(contract.get("ready", false)),
		"physically continuous/full internal component did not become coupling-ready")
	_expect(int(contract.get("physical_internal_link_count", -1)) == 2,
		"confluence contract did not publish both fine internal links")
	_expect(int(contract.get("internal_coarse_mouths_bypassed", -1)) == 2,
		"confluence contract did not bypass both internal coarse mouths")

	store.refined_pending_inflow_m3[a] = 3.0
	store.refined_inflow_rate_m3s[a] = 3.0
	store.refined_pending_inflow_m3[b] = 5.0
	store.refined_inflow_rate_m3s[b] = 5.0
	store.refined_pending_inflow_m3[stem] = 2.0
	store.refined_inflow_rate_m3s[stem] = 2.0
	var coupling_records := _coupling_records(store, [a, b, stem])
	var total_before := store.total_storage_m3()
	var plan := HydroRiverComponentExchangePlanner.plan(store, component_id,
		coupling_records, 1.0, 1.25, CELL_SIZE_M)
	_expect(int(plan.get("error", FAILED)) == OK, "multi-mouth exchange plan failed")
	if int(plan.get("error", FAILED)) != OK:
		return
	_expect(int(plan.get("boundary_record_count", 0)) == 3,
		"one-member confluence should produce three combined boundary records")
	_expect(int(plan.get("injection_record_count", 0)) == 3,
		"per-reach pending queues did not each receive an injection record")
	_expect(int(plan.get("downstream_record_count", 0)) == 1,
		"component produced more than one downstream removal mouth")
	_expect(int(plan.get("external_upstream_mouth_count", 0)) == 2,
		"component did not retain two external upstream roots")
	_expect(is_equal_approx(float(plan.get("requested_add_m3", -1.0)), 10.0),
		"component requested-add total is incorrect")
	_expect(is_equal_approx(store.total_storage_m3(), total_before),
		"planning component exchange changed coarse ownership")

	var records := plan.get("exchange_records", []) as Array
	var seen_a := false
	var seen_b := false
	var seen_stem := false
	var downstream_enabled := 0
	for value: Variant in records:
		if not (value is Dictionary):
			_fail("exchange plan emitted non-dictionary record")
			continue
		var record := value as Dictionary
		var cell := int(record.get("cell", -1))
		var up := bool(record.get("upstream_enabled", false))
		var down := bool(record.get("downstream_enabled", false))
		if down:
			downstream_enabled += 1
		if cell == a:
			seen_a = true
			_expect(up and not down, "tributary A owns an illegal downstream mouth")
		elif cell == b:
			seen_b = true
			_expect(up and not down, "tributary B owns an illegal downstream mouth")
		elif cell == stem:
			seen_stem = true
			_expect(up and down, "one-member outlet did not combine injection/removal flags")
	_expect(seen_a and seen_b and seen_stem, "exchange plan omitted a component reach")
	_expect(downstream_enabled == 1, "exchange records contain multiple downstream mouths")


func _test_residual_internal_reach_falls_back() -> void:
	var fixture := _make_confluence_fixture()
	if fixture.is_empty():
		return
	var store := fixture["store"] as PlanetHydrologyRiverClusterStore
	var a := int(fixture["a"])
	var b := int(fixture["b"])
	var stem := int(fixture["stem"])
	var keys := _connected_confluence_keys()
	# A remains partially coarse, so bypassing its downstream mouth would skip an
	# authoritative residual 1D segment.
	_promote_cluster(store, a, keys["a"] as HydroTileKey, 13, false, Vector2.RIGHT)
	_promote_cluster(store, b, keys["b"] as HydroTileKey, 14, true, Vector2.UP)
	_promote_cluster(store, stem, keys["stem"] as HydroTileKey, 15, false, Vector2.RIGHT)
	if not _failures.is_empty():
		return
	var merged := store.merge_refined_clusters(PackedInt32Array([a, b, stem]))
	if int(merged.get("error", FAILED)) != OK:
		_fail("residual fallback fixture union failed")
		return
	var contract := HydroRiverComponentCouplingContract.evaluate(store,
		int(merged.get("component_id", -1)))
	_expect(int(contract.get("error", OK)) != OK and not bool(contract.get("ready", true)),
		"component with residual internal 1D reach became coupling-ready")
	_expect(String(contract.get("reason", "")) == "component_internal_reach_has_residual_1d",
		"residual internal reach published wrong fallback reason")


func _test_internal_fine_gap_falls_back() -> void:
	var fixture := _make_confluence_fixture()
	if fixture.is_empty():
		return
	var store := fixture["store"] as PlanetHydrologyRiverClusterStore
	var a := int(fixture["a"])
	var b := int(fixture["b"])
	var stem := int(fixture["stem"])
	var stem_key := HydroTileKey.new(CubeSphere.FACE_PX, 6, 20, 20)
	var far_a := HydroTileKey.new(CubeSphere.FACE_PX, 6, 3, 3)
	var far_b := HydroTileKey.new(CubeSphere.FACE_PX, 6, 45, 45)
	_promote_cluster(store, a, far_a, 23, true, Vector2.RIGHT)
	_promote_cluster(store, b, far_b, 24, true, Vector2.UP)
	_promote_cluster(store, stem, stem_key, 25, false, Vector2.RIGHT)
	if not _failures.is_empty():
		return
	var merged := store.merge_refined_clusters(PackedInt32Array([a, b, stem]))
	if int(merged.get("error", FAILED)) != OK:
		_fail("fine-gap fallback fixture union failed")
		return
	var contract := HydroRiverComponentCouplingContract.evaluate(store,
		int(merged.get("component_id", -1)))
	_expect(int(contract.get("error", OK)) != OK and not bool(contract.get("ready", true)),
		"component with disconnected fine boundaries became coupling-ready")
	_expect(String(contract.get("reason", "")) == "component_internal_fine_gap",
		"fine gap published wrong fallback reason")


func _connected_confluence_keys() -> Dictionary:
	var stem := HydroTileKey.new(CubeSphere.FACE_PX, 6, 20, 20)
	var a_link := HydroTileTopology.neighbor(stem, HydroTileTopology.DIR_WEST)
	var b_link := HydroTileTopology.neighbor(stem, HydroTileTopology.DIR_SOUTH)
	return {
		"stem": stem,
		"a": a_link.get("key") as HydroTileKey,
		"b": b_link.get("key") as HydroTileKey,
	}


func _promote_cluster(store: PlanetHydrologyRiverClusterStore, cell: int,
		key: HydroTileKey, slot: int, full_reach: bool, direction: Vector2) -> void:
	if key == null:
		_fail("test sparse key is null")
		return
	var available := store.available_channel_promotion_volume_m3(cell)
	var transfer := available if full_reach else maxf(minf(available * 0.25, 5.0), 0.01)
	var prepared := store.prepare_channel_promotion(cell, transfer)
	if int(prepared.get("error", FAILED)) != OK:
		_fail("promotion prepare failed for cell %d" % cell)
		return
	var committed := store.commit_promotion(int(prepared["transaction_id"]))
	if int(committed.get("error", FAILED)) != OK:
		_fail("promotion commit failed for cell %d" % cell)
		return
	var member := {
		"tile_id": key.packed(),
		"slot": slot,
		"center_cell": Vector2(16.0, 16.0),
		"direction_cell": direction.normalized(),
		"local_velocity": direction.normalized() * 1.5,
		"half_width_m": 6.0,
	}
	var registered := store.register_refined_cluster(cell, [member], transfer)
	if int(registered.get("error", FAILED)) != OK:
		_fail("cluster registration failed for cell %d (%s)" % [
			cell, String(registered.get("reason", "unknown"))])


func _coupling_records(store: PlanetHydrologyRiverClusterStore,
		cells: Array) -> Dictionary:
	var out: Dictionary = {}
	for value: Variant in cells:
		var cell := int(value)
		out[cell] = {
			"cell": cell,
			"members": store.refined_cluster_members(cell),
		}
	return out


func _make_confluence_fixture() -> Dictionary:
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

	for stem in grid.cell_count:
		var downstream := _first_usable_neighbor(grid, stem, {})
		if downstream < 0:
			continue
		var tributaries: Array[int] = []
		for candidate in grid.cell_count:
			if candidate == stem or candidate == downstream:
				continue
			if _neighbor_slot_to(grid, candidate, stem) >= 0:
				tributaries.append(candidate)
				if tributaries.size() == 2:
					break
		if tributaries.size() < 2:
			continue
		_configure_reach(fields, grid, tributaries[0], stem, 130.0, 120.0)
		_configure_reach(fields, grid, tributaries[1], stem, 128.0, 120.0)
		_configure_reach(fields, grid, stem, downstream, 120.0, 105.0)
		var store := PlanetHydrologyRiverClusterStore.new()
		var err := store.initialize(fields)
		if err != OK:
			continue
		store.set_climatology_fallback_enabled(false)
		for cell in [tributaries[0], tributaries[1], stem]:
			store.channel_storage_m3[cell] = maxf(store.channel_storage_m3[cell], 20.0)
		store.initial_storage_m3 = store.total_storage_m3()
		return {
			"store": store,
			"a": tributaries[0],
			"b": tributaries[1],
			"stem": stem,
		}
	_fail("could not construct component exchange confluence fixture")
	return {}


func _configure_reach(fields: PlanetFields, grid: PlanetGrid, cell: int,
		destination: int, elevation: float, destination_elevation: float) -> void:
	var slot := _neighbor_slot_to(grid, cell, destination)
	if slot < 0:
		return
	fields.flow_dir[cell] = slot
	fields.elev[cell] = elevation
	fields.elev[destination] = minf(float(fields.elev[destination]), destination_elevation)
	fields.discharge[cell] = BASELINE_Q
	fields.stream_order[cell] = 3
	fields.river_width[cell] = 7.2 * sqrt(BASELINE_Q)


func _neighbor_slot_to(grid: PlanetGrid, cell: int, destination: int) -> int:
	for slot in 8:
		if int(grid.nbr[cell * 8 + slot]) == destination:
			return slot
	return -1


func _first_usable_neighbor(grid: PlanetGrid, cell: int, excluded: Dictionary) -> int:
	for slot in 8:
		var candidate := int(grid.nbr[cell * 8 + slot])
		if candidate >= 0 and candidate < grid.cell_count and candidate != cell \
				and not excluded.has(candidate):
			return candidate
	return -1


func _expect(condition: bool, message: String) -> void:
	if not condition:
		_fail(message)


func _fail(message: String) -> void:
	if not _failures.has(message):
		_failures.append(message)
