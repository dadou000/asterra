extends Node
## CPU/headless gate for cross-macro-reach river component union metadata.

const TEST_RES := 3
const TEST_RADIUS_M := 1000.0
const BASELINE_Q := 10.0

var _failures: Array[String] = []


func _ready() -> void:
	_test_confluence_component_union()
	_test_union_of_existing_components()
	_test_invalid_multi_outlet_is_atomic()
	if _failures.is_empty():
		print("PLANET_RIVER_CLUSTER_COMPONENTS: PASS")
		get_tree().quit(0)
	else:
		for failure in _failures:
			push_error("PLANET_RIVER_CLUSTER_COMPONENTS: " + failure)
		get_tree().quit(1)


func _test_confluence_component_union() -> void:
	var fixture := _make_fixture()
	if fixture.is_empty():
		return
	var store := fixture["store"] as PlanetHydrologyRiverClusterStore
	var tributary_a := int(fixture["tributary_a"])
	var tributary_b := int(fixture["tributary_b"])
	var stem := int(fixture["stem"])
	_register_test_cluster(store, tributary_a, 10, 100)
	_register_test_cluster(store, tributary_b, 20, 200)
	_register_test_cluster(store, stem, 30, 300)
	if not _failures.is_empty():
		return

	var total_before := store.total_storage_m3()
	var merged := store.merge_refined_clusters(PackedInt32Array([
		tributary_a, tributary_b, stem,
	]))
	_expect(int(merged.get("error", FAILED)) == OK,
		"valid confluence component union failed")
	if int(merged.get("error", FAILED)) != OK:
		return
	var component := merged.get("component", {}) as Dictionary
	var component_id := int(merged.get("component_id", -1))
	_expect(component_id >= 0, "component union returned invalid id")
	_expect(store.refined_component_count() == 1, "component registry count is not one")
	_expect(int(component.get("reach_count", 0)) == 3, "component lost refined reaches")
	_expect(int(component.get("fine_member_count", 0)) == 3,
		"component fine member count is incorrect")
	_expect(int(component.get("upstream_mouth_count", 0)) == 2,
		"confluence did not expose two upstream mouths")
	var roots := component.get("upstream_mouth_cells", PackedInt32Array()) as PackedInt32Array
	_expect(roots.has(tributary_a) and roots.has(tributary_b),
		"component upstream mouths do not match tributaries")
	_expect(int(component.get("downstream_outlet_cell", -1)) == stem,
		"component downstream outlet is not the main stem")
	_expect(store.refined_component_id_for_cell(tributary_a) == component_id,
		"tributary A component index missing")
	_expect(store.refined_component_id_for_cell(tributary_b) == component_id,
		"tributary B component index missing")
	_expect(store.refined_component_id_for_cell(stem) == component_id,
		"stem component index missing")
	_expect(is_equal_approx(store.total_storage_m3(), total_before),
		"component metadata union changed coarse-owned water")

	# Per-reach collapse/unregister must not invalidate an active component contract.
	var blocked := store.unregister_refined_reach(tributary_a, true)
	_expect(int(blocked.get("error", FAILED)) == ERR_BUSY,
		"component member could unregister independently")
	_expect(store.is_refined_reach(tributary_a),
		"blocked unregister unexpectedly removed refinement")

	var dissolved := store.dissolve_refined_component(component_id)
	_expect(int(dissolved.get("error", FAILED)) == OK, "component dissolve failed")
	_expect(store.refined_component_count() == 0, "component survived dissolve")
	_expect(store.refined_component_id_for_cell(tributary_a) < 0,
		"component reverse index survived dissolve")
	_expect(store.is_refined_reach(tributary_a) and store.is_refined_reach(tributary_b) \
			and store.is_refined_reach(stem),
		"dissolving union metadata altered fine ownership")


func _test_union_of_existing_components() -> void:
	var fixture := _make_linear_fixture()
	if fixture.is_empty():
		return
	var store := fixture["store"] as PlanetHydrologyRiverClusterStore
	var a := int(fixture["a"])
	var b := int(fixture["b"])
	var c := int(fixture["c"])
	var d := int(fixture["d"])
	_register_test_cluster(store, a, 40, 400)
	_register_test_cluster(store, b, 50, 500)
	_register_test_cluster(store, c, 60, 600)
	_register_test_cluster(store, d, 70, 700)
	if not _failures.is_empty():
		return

	var first := store.merge_refined_clusters(PackedInt32Array([a, b]))
	var second := store.merge_refined_clusters(PackedInt32Array([c, d]))
	_expect(int(first.get("error", FAILED)) == OK and int(second.get("error", FAILED)) == OK,
		"could not create seed components")
	if int(first.get("error", FAILED)) != OK or int(second.get("error", FAILED)) != OK:
		return
	_expect(store.refined_component_count() == 2, "seed component count is not two")

	# Request one cell from each component. The store must expand both entire source
	# components before validating and then replace them atomically with one union.
	var unioned := store.merge_refined_clusters(PackedInt32Array([b, c]))
	_expect(int(unioned.get("error", FAILED)) == OK, "existing components did not union")
	_expect(store.refined_component_count() == 1, "old components survived component union")
	var component := unioned.get("component", {}) as Dictionary
	_expect(int(component.get("reach_count", 0)) == 4,
		"component union did not include complete source components")
	var component_id := int(unioned.get("component_id", -1))
	for cell in [a, b, c, d]:
		_expect(store.refined_component_id_for_cell(cell) == component_id,
			"union reverse index missing cell %d" % cell)


func _test_invalid_multi_outlet_is_atomic() -> void:
	var fixture := _make_disconnected_fixture()
	if fixture.is_empty():
		return
	var store := fixture["store"] as PlanetHydrologyRiverClusterStore
	var a := int(fixture["a"])
	var b := int(fixture["b"])
	_register_test_cluster(store, a, 80, 800)
	_register_test_cluster(store, b, 90, 900)
	if not _failures.is_empty():
		return
	var before_components := store.refined_component_count()
	var before_members := store.refined_sparse_member_count()
	var result := store.merge_refined_clusters(PackedInt32Array([a, b]))
	_expect(int(result.get("error", OK)) != OK,
		"disconnected/multi-outlet reaches formed one component")
	_expect(String(result.get("reason", "")) == "component_requires_single_downstream_outlet",
		"multi-outlet rejection published wrong reason")
	_expect(store.refined_component_count() == before_components,
		"failed component union mutated registry")
	_expect(store.refined_sparse_member_count() == before_members,
		"failed component union changed fine-member ownership")
	_expect(store.refined_component_id_for_cell(a) < 0 and store.refined_component_id_for_cell(b) < 0,
		"failed component union left reverse-index residue")


func _register_test_cluster(store: PlanetHydrologyRiverClusterStore, cell: int,
		slot: int, tile_seed: int) -> void:
	var transfer := minf(maxf(store.channel_storage_m3[cell] * 0.10, 0.01), 5.0)
	var prepared := store.prepare_channel_promotion(cell, transfer)
	if int(prepared.get("error", FAILED)) != OK:
		_fail("promotion prepare failed for cell %d" % cell)
		return
	var committed := store.commit_promotion(int(prepared["transaction_id"]))
	if int(committed.get("error", FAILED)) != OK:
		_fail("promotion commit failed for cell %d" % cell)
		return
	# Identity only needs to be stable/unique for this CPU registry gate.
	var key := HydroTileKey.new(CubeSphere.FACE_PX, 8, tile_seed % 200,
		(tile_seed / 7) % 200)
	var registered := store.register_refined_cluster(cell, [
		{"tile_id": key.packed(), "slot": slot},
	], transfer)
	if int(registered.get("error", FAILED)) != OK:
		_fail("cluster registration failed for cell %d (%s)" % [
			cell, String(registered.get("reason", "unknown"))])


func _make_fixture() -> Dictionary:
	var base := _make_fields()
	if base.is_empty():
		return {}
	var fields := base["fields"] as PlanetFields
	var grid := base["grid"] as PlanetGrid
	# Find two distinct cells that share a downstream receiver, then ensure that
	# receiver itself is a valid reach with a downstream continuation.
	for stem in grid.cell_count:
		var downstream := _first_usable_neighbor(grid, stem, {})
		if downstream < 0:
			continue
		var tributaries: Array[int] = []
		for candidate in grid.cell_count:
			if candidate == stem or candidate == downstream:
				continue
			var slot := _neighbor_slot_to(grid, candidate, stem)
			if slot >= 0:
				tributaries.append(candidate)
				if tributaries.size() == 2:
					break
		if tributaries.size() < 2:
			continue
		_configure_reach(fields, grid, tributaries[0], stem, 130.0, 120.0)
		_configure_reach(fields, grid, tributaries[1], stem, 128.0, 120.0)
		_configure_reach(fields, grid, stem, downstream, 120.0, 105.0)
		var store := _initialize_store(fields)
		if store != null:
			return {
				"store": store,
				"tributary_a": tributaries[0],
				"tributary_b": tributaries[1],
				"stem": stem,
			}
	_fail("could not construct confluence fixture")
	return {}


func _make_linear_fixture() -> Dictionary:
	var base := _make_fields()
	if base.is_empty():
		return {}
	var fields := base["fields"] as PlanetFields
	var grid := base["grid"] as PlanetGrid
	for a in grid.cell_count:
		var used: Dictionary = {a: true}
		var b := _first_usable_neighbor(grid, a, used)
		if b < 0:
			continue
		used[b] = true
		var c := _first_usable_neighbor(grid, b, used)
		if c < 0:
			continue
		used[c] = true
		var d := _first_usable_neighbor(grid, c, used)
		if d < 0:
			continue
		used[d] = true
		var e := _first_usable_neighbor(grid, d, used)
		if e < 0:
			continue
		_configure_reach(fields, grid, a, b, 150.0, 140.0)
		_configure_reach(fields, grid, b, c, 140.0, 130.0)
		_configure_reach(fields, grid, c, d, 130.0, 120.0)
		_configure_reach(fields, grid, d, e, 120.0, 110.0)
		var store := _initialize_store(fields)
		if store != null:
			return {"store": store, "a": a, "b": b, "c": c, "d": d}
	_fail("could not construct linear component fixture")
	return {}


func _make_disconnected_fixture() -> Dictionary:
	var base := _make_fields()
	if base.is_empty():
		return {}
	var fields := base["fields"] as PlanetFields
	var grid := base["grid"] as PlanetGrid
	for a in grid.cell_count:
		var a_out := _first_usable_neighbor(grid, a, {})
		if a_out < 0:
			continue
		for b in grid.cell_count:
			if b == a or b == a_out:
				continue
			var b_out := _first_usable_neighbor(grid, b, {a: true, a_out: true})
			if b_out < 0 or b_out == a_out:
				continue
			_configure_reach(fields, grid, a, a_out, 140.0, 120.0)
			_configure_reach(fields, grid, b, b_out, 135.0, 115.0)
			var store := _initialize_store(fields)
			if store != null:
				return {"store": store, "a": a, "b": b}
	_fail("could not construct disconnected fixture")
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


func _configure_reach(fields: PlanetFields, grid: PlanetGrid, cell: int,
		destination: int, elevation: float, destination_elevation: float) -> void:
	var slot := _neighbor_slot_to(grid, cell, destination)
	if slot < 0:
		_fail("configured destination is not a grid neighbor")
		return
	fields.flow_dir[cell] = slot
	fields.elev[cell] = elevation
	fields.elev[destination] = minf(float(fields.elev[destination]), destination_elevation)
	fields.discharge[cell] = BASELINE_Q
	fields.stream_order[cell] = 3
	fields.river_width[cell] = 7.2 * sqrt(BASELINE_Q)


func _initialize_store(fields: PlanetFields) -> PlanetHydrologyRiverClusterStore:
	var store := PlanetHydrologyRiverClusterStore.new()
	var err := store.initialize(fields)
	if err != OK:
		_fail("cluster component store initialization failed (%d)" % int(err))
		return null
	store.set_climatology_fallback_enabled(false)
	for c in store.cell_count():
		if store.river_reaches.is_reach_cell(c):
			store.channel_storage_m3[c] = maxf(store.channel_storage_m3[c], 20.0)
	store.initial_storage_m3 = store.total_storage_m3()
	return store


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
