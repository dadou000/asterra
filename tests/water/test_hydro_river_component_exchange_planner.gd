extends Node
## Headless CPU gate for component coupling readiness and compact exchange planning.
## A linear cross-reach component can become fine-continuous today. True multi-root
## confluences remain intentionally gated until branched junction seeding exists.

const TEST_RES := 3
const TEST_RADIUS_M := 1000.0
const BASELINE_Q := 10.0
const TILE_RES := 32
const CELL_SIZE_M := 4.0

var _failures: Array[String] = []


func _ready() -> void:
	_test_ready_linear_component_plan()
	_test_residual_internal_reach_falls_back()
	_test_internal_fine_gap_falls_back()
	_test_corridor_geometry_gap_falls_back()
	_test_unverified_confluence_falls_back()
	if _failures.is_empty():
		print("HYDRO_RIVER_COMPONENT_EXCHANGE_PLANNER: PASS")
		get_tree().quit(0)
	else:
		for failure in _failures:
			push_error("HYDRO_RIVER_COMPONENT_EXCHANGE_PLANNER: " + failure)
		get_tree().quit(1)


func _test_ready_linear_component_plan() -> void:
	var fixture := _make_linear_fixture()
	if fixture.is_empty():
		return
	var store := fixture["store"] as PlanetHydrologyRiverClusterStore
	var a := int(fixture["a"])
	var b := int(fixture["b"])
	var keys := _linear_keys()
	_promote_cluster(store, a, keys["a"] as HydroTileKey, 3, true, Vector2.RIGHT)
	_promote_cluster(store, b, keys["b"] as HydroTileKey, 4, false, Vector2.RIGHT)
	if not _failures.is_empty():
		return

	var merged := store.merge_refined_clusters(PackedInt32Array([a, b]))
	_expect(int(merged.get("error", FAILED)) == OK, "linear component union failed")
	if int(merged.get("error", FAILED)) != OK:
		return
	var component_id := int(merged.get("component_id", -1))
	var contract := HydroRiverComponentCouplingContract.evaluate(store, component_id,
		TILE_RES, CELL_SIZE_M)
	_expect(int(contract.get("error", FAILED)) == OK and bool(contract.get("ready", false)),
		"continuous full-internal linear component did not become coupling-ready")
	_expect(int(contract.get("physical_internal_link_count", -1)) == 1,
		"linear component did not publish its fine internal link")
	_expect(bool(contract.get("corridor_continuity_verified", false)),
		"linear component skipped corridor continuity validation")

	store.refined_pending_inflow_m3[a] = 3.0
	store.refined_inflow_rate_m3s[a] = 3.0
	store.refined_pending_inflow_m3[b] = 2.0
	store.refined_inflow_rate_m3s[b] = 2.0
	var coupling_records := _coupling_records(store, [a, b])
	var total_before := store.total_storage_m3()
	var plan := HydroRiverComponentExchangePlanner.plan(store, component_id,
		coupling_records, 1.0, 1.25, CELL_SIZE_M, TILE_RES)
	_expect(int(plan.get("error", FAILED)) == OK, "linear component exchange plan failed")
	if int(plan.get("error", FAILED)) != OK:
		return
	_expect(int(plan.get("boundary_record_count", 0)) == 2,
		"two one-member reaches should produce two component boundary records")
	_expect(int(plan.get("injection_record_count", 0)) == 2,
		"each coarse pending queue did not receive an injection record")
	_expect(int(plan.get("downstream_record_count", 0)) == 1,
		"linear component produced more than one downstream removal mouth")
	_expect(int(plan.get("external_upstream_mouth_count", 0)) == 1,
		"linear component should expose one external upstream root")
	_expect(is_equal_approx(float(plan.get("requested_add_m3", -1.0)), 5.0),
		"linear component requested-add total is incorrect")
	_expect(is_equal_approx(store.total_storage_m3(), total_before),
		"planning component exchange changed coarse ownership")

	var records := plan.get("exchange_records", []) as Array
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
			_expect(up and not down, "internal upstream reach owns illegal downstream mouth")
		elif cell == b:
			_expect(up and down, "one-member outlet did not combine add/remove flags")
	_expect(downstream_enabled == 1, "exchange records contain multiple downstream mouths")


func _test_residual_internal_reach_falls_back() -> void:
	var fixture := _make_linear_fixture()
	if fixture.is_empty():
		return
	var store := fixture["store"] as PlanetHydrologyRiverClusterStore
	var a := int(fixture["a"])
	var b := int(fixture["b"])
	var keys := _linear_keys()
	_promote_cluster(store, a, keys["a"] as HydroTileKey, 13, false, Vector2.RIGHT)
	_promote_cluster(store, b, keys["b"] as HydroTileKey, 14, false, Vector2.RIGHT)
	if not _failures.is_empty():
		return
	var merged := store.merge_refined_clusters(PackedInt32Array([a, b]))
	if int(merged.get("error", FAILED)) != OK:
		_fail("residual fallback fixture union failed")
		return
	var contract := HydroRiverComponentCouplingContract.evaluate(store,
		int(merged.get("component_id", -1)), TILE_RES, CELL_SIZE_M)
	_expect(int(contract.get("error", OK)) != OK and not bool(contract.get("ready", true)),
		"component with residual internal 1D reach became coupling-ready")
	_expect(String(contract.get("reason", "")) == "component_internal_reach_has_residual_1d",
		"residual internal reach published wrong fallback reason")


func _test_internal_fine_gap_falls_back() -> void:
	var fixture := _make_linear_fixture()
	if fixture.is_empty():
		return
	var store := fixture["store"] as PlanetHydrologyRiverClusterStore
	var a := int(fixture["a"])
	var b := int(fixture["b"])
	_promote_cluster(store, a, HydroTileKey.new(CubeSphere.FACE_PX, 6, 3, 3),
		23, true, Vector2.RIGHT)
	_promote_cluster(store, b, HydroTileKey.new(CubeSphere.FACE_PX, 6, 45, 45),
		24, false, Vector2.RIGHT)
	if not _failures.is_empty():
		return
	var merged := store.merge_refined_clusters(PackedInt32Array([a, b]))
	if int(merged.get("error", FAILED)) != OK:
		_fail("fine-gap fallback fixture union failed")
		return
	var contract := HydroRiverComponentCouplingContract.evaluate(store,
		int(merged.get("component_id", -1)), TILE_RES, CELL_SIZE_M)
	_expect(String(contract.get("reason", "")) == "component_internal_fine_gap",
		"non-neighbor fine gap published wrong fallback reason")


func _test_corridor_geometry_gap_falls_back() -> void:
	var fixture := _make_linear_fixture()
	if fixture.is_empty():
		return
	var store := fixture["store"] as PlanetHydrologyRiverClusterStore
	var a := int(fixture["a"])
	var b := int(fixture["b"])
	var keys := _linear_keys()
	_promote_cluster(store, a, keys["a"] as HydroTileKey, 33, true, Vector2.RIGHT)
	# Tile adjacency is valid, but this centerline is parallel to the shared west edge
	# and therefore never receives the upstream fine corridor.
	_promote_cluster(store, b, keys["b"] as HydroTileKey, 34, false, Vector2.UP)
	if not _failures.is_empty():
		return
	var merged := store.merge_refined_clusters(PackedInt32Array([a, b]))
	if int(merged.get("error", FAILED)) != OK:
		_fail("corridor-gap fallback fixture union failed")
		return
	var contract := HydroRiverComponentCouplingContract.evaluate(store,
		int(merged.get("component_id", -1)), TILE_RES, CELL_SIZE_M)
	_expect(String(contract.get("reason", "")) == "component_internal_corridor_edge_miss",
		"corridor discontinuity published wrong fallback reason")


func _test_unverified_confluence_falls_back() -> void:
	var fixture := _make_confluence_fixture()
	if fixture.is_empty():
		return
	var store := fixture["store"] as PlanetHydrologyRiverClusterStore
	var a := int(fixture["a"])
	var b := int(fixture["b"])
	var stem := int(fixture["stem"])
	var keys := _connected_confluence_keys()
	_promote_cluster(store, a, keys["a"] as HydroTileKey, 43, true, Vector2.RIGHT)
	_promote_cluster(store, b, keys["b"] as HydroTileKey, 44, true, Vector2.UP)
	_promote_cluster(store, stem, keys["stem"] as HydroTileKey, 45, false, Vector2.RIGHT)
	if not _failures.is_empty():
		return
	var merged := store.merge_refined_clusters(PackedInt32Array([a, b, stem]))
	if int(merged.get("error", FAILED)) != OK:
		_fail("unverified confluence fixture union failed")
		return
	var contract := HydroRiverComponentCouplingContract.evaluate(store,
		int(merged.get("component_id", -1)), TILE_RES, CELL_SIZE_M)
	_expect(String(contract.get("reason", "")) == "component_fine_junction_not_verified",
		"straight-corridor confluence was not held on conservative fallback")
	_expect(bool(contract.get("requires_branched_junction_seeding", false)),
		"confluence fallback did not identify branched junction requirement")


func _linear_keys() -> Dictionary:
	var a := HydroTileKey.new(CubeSphere.FACE_PX, 6, 20, 20)
	var link := HydroTileTopology.neighbor(a, HydroTileTopology.DIR_EAST)
	return {"a": a, "b": link.get("key") as HydroTileKey}


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
		"half_width_m": 2.0,
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
		out[cell] = {"cell": cell, "members": store.refined_cluster_members(cell)}
	return out


func _make_linear_fixture() -> Dictionary:
	var base := _make_fields()
	var fields := base["fields"] as PlanetFields
	var grid := base["grid"] as PlanetGrid
	for a in grid.cell_count:
		var b := _first_usable_neighbor(grid, a, {})
		if b < 0:
			continue
		var c := _first_usable_neighbor(grid, b, {a: true})
		if c < 0 or c == a:
			continue
		_configure_reach(fields, grid, a, b, 130.0, 120.0)
		_configure_reach(fields, grid, b, c, 120.0, 105.0)
		var store := _initialize_store(fields, [a, b])
		if store != null:
			return {"store": store, "a": a, "b": b}
	_fail("could not construct linear component fixture")
	return {}


func _make_confluence_fixture() -> Dictionary:
	var base := _make_fields()
	var fields := base["fields"] as PlanetFields
	var grid := base["grid"] as PlanetGrid
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
		var store := _initialize_store(fields, [tributaries[0], tributaries[1], stem])
		if store != null:
			return {
				"store": store,
				"a": tributaries[0],
				"b": tributaries[1],
				"stem": stem,
			}
	_fail("could not construct component confluence fixture")
	return {}


func _make_fields() -> Dictionary:
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
	return {"fields": fields, "grid": grid}


func _initialize_store(fields: PlanetFields, reach_cells: Array) -> PlanetHydrologyRiverClusterStore:
	var store := PlanetHydrologyRiverClusterStore.new()
	var err := store.initialize(fields)
	if err != OK:
		return null
	store.set_climatology_fallback_enabled(false)
	for value: Variant in reach_cells:
		var cell := int(value)
		store.channel_storage_m3[cell] = maxf(store.channel_storage_m3[cell], 20.0)
	store.initial_storage_m3 = store.total_storage_m3()
	return store


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
