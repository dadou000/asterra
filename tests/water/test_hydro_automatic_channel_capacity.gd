extends Node
## Headless integration gate for production cluster-capacity policy.
## Uses a real coarse river store and a tiny facade that records whether the policy
## attempted to enter the ownership bridge.

const TEST_RES := 2
const TEST_RADIUS_M := 1000.0
const BASELINE_Q := 10.0

var _failures: Array[String] = []


class PolicyPersistentFacade:
	extends Node
	signal store_rebuilt
	var test_store: PlanetHydrologyRiverClusterStore
	func available() -> bool:
		return test_store != null and test_store.initialized
	func store() -> PlanetHydrologyRiverClusterStore:
		return test_store


class PolicyWaterFacade:
	extends Node
	signal river_reach_promotion_completed(report: Dictionary)
	signal river_reach_promotion_failed(error: Error, stage: String)
	signal river_reach_collapse_completed(report: Dictionary)
	signal river_reach_collapse_failed(error: Error, stage: String)

	var automatic_channel_promotion_enabled := true
	var automatic_channel_demotion_enabled := false
	var test_store: PlanetHydrologyRiverClusterStore
	var requested_members := 3
	var free_slots := 3
	var promotion_calls := 0
	var next_request_id := 100

	func river_reach_promotion_bridge_available() -> bool:
		return true
	func river_reach_coupling_available() -> bool:
		return true
	func channel_reach_candidates(max_count: int, q_ratio: float,
			bank_ratio: float) -> Array[Dictionary]:
		return test_store.channel_reach_candidates(max_count, q_ratio, bank_ratio)
	func river_cluster_requested_member_count(_cell: int) -> int:
		return requested_members
	func river_cluster_default_member_count() -> int:
		return requested_members
	func river_cluster_free_member_slots() -> int:
		return free_slots
	func suggested_river_reach_promotion_volume_m3(_cell: int) -> float:
		return 1.0
	func promote_coarse_river_reach(_cell: int, _volume_m3: float) -> int:
		promotion_calls += 1
		var result := next_request_id
		next_request_id += 1
		return result


func _ready() -> void:
	_test_sparse_slot_guard()
	_test_member_budget_guard()
	_test_exact_fit_starts_promotion()
	if _failures.is_empty():
		print("HYDRO_AUTOMATIC_CHANNEL_CAPACITY: PASS")
		get_tree().quit(0)
	else:
		for failure in _failures:
			push_error("HYDRO_AUTOMATIC_CHANNEL_CAPACITY: " + failure)
		get_tree().quit(1)


func _test_sparse_slot_guard() -> void:
	var fixture := _make_fixture()
	if fixture.is_empty():
		return
	var store := fixture["store"] as PlanetHydrologyRiverClusterStore
	var water := PolicyWaterFacade.new()
	water.test_store = store
	water.requested_members = 3
	water.free_slots = 2
	var persistent := PolicyPersistentFacade.new()
	persistent.test_store = store
	var policy := _make_policy(water, persistent)
	policy.max_auto_refined_member_slots = 24
	policy.scan_once()
	_expect(water.promotion_calls == 0,
		"policy entered promotion bridge without all cluster slots free")
	_expect(String(policy.stats().get("last_reason", "")) \
			== "sparse_member_capacity_insufficient",
		"sparse slot suppression did not publish diagnostic reason")
	policy.queue_free()
	water.queue_free()
	persistent.queue_free()


func _test_member_budget_guard() -> void:
	var fixture := _make_fixture()
	if fixture.is_empty():
		return
	var store := fixture["store"] as PlanetHydrologyRiverClusterStore
	var water := PolicyWaterFacade.new()
	water.test_store = store
	water.requested_members = 3
	water.free_slots = 8
	var persistent := PolicyPersistentFacade.new()
	persistent.test_store = store
	var policy := _make_policy(water, persistent)
	policy.max_auto_refined_member_slots = 2
	policy.scan_once()
	_expect(water.promotion_calls == 0,
		"policy exceeded configured river-member budget")
	_expect(String(policy.stats().get("last_reason", "")) \
			== "refined_member_budget_reached",
		"member-budget suppression did not publish diagnostic reason")
	policy.queue_free()
	water.queue_free()
	persistent.queue_free()


func _test_exact_fit_starts_promotion() -> void:
	var fixture := _make_fixture()
	if fixture.is_empty():
		return
	var store := fixture["store"] as PlanetHydrologyRiverClusterStore
	var water := PolicyWaterFacade.new()
	water.test_store = store
	water.requested_members = 3
	water.free_slots = 3
	var persistent := PolicyPersistentFacade.new()
	persistent.test_store = store
	var policy := _make_policy(water, persistent)
	policy.max_auto_refined_member_slots = 3
	policy.scan_once()
	_expect(water.promotion_calls == 1,
		"exact-fit cluster did not enter promotion bridge")
	_expect(String(policy.stats().get("active_kind", "")) == "promotion",
		"exact-fit promotion was not marked single-flight active")
	policy.queue_free()
	water.queue_free()
	persistent.queue_free()


func _make_policy(water: PolicyWaterFacade,
		persistent: PolicyPersistentFacade) -> HydroAutomaticChannelRefinementClusterCapacity:
	var policy := HydroAutomaticChannelRefinementClusterCapacity.new()
	policy.scan_interval_s = 999.0
	policy.minimum_stream_order = 1
	var err := policy.bind_facades_for_test(water, persistent)
	if err != OK:
		_fail("policy facade bind failed (%d)" % int(err))
	add_child(water)
	add_child(persistent)
	add_child(policy)
	return policy


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
	var destination := int(grid.nbr[source * 8])
	if destination == source or destination < 0 or destination >= grid.cell_count:
		_fail("fixture has no usable downstream neighbor")
		return {}
	fields.flow_dir[source] = 0
	fields.elev[source] = 110.0
	fields.elev[destination] = 90.0
	fields.discharge[source] = BASELINE_Q
	fields.stream_order[source] = 3
	fields.river_width[source] = 7.2 * sqrt(BASELINE_Q)
	var store := PlanetHydrologyRiverClusterStore.new()
	var err := store.initialize(fields)
	if err != OK:
		_fail("cluster store initialize failed (%d)" % int(err))
		return {}
	store.set_climatology_fallback_enabled(false)
	store.channel_discharge_m3s[source] = BASELINE_Q * 2.5
	return {"store": store, "source": source}


func _expect(condition: bool, message: String) -> void:
	if not condition:
		_fail(message)


func _fail(message: String) -> void:
	if not _failures.has(message):
		_failures.append(message)
