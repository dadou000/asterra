extends Node
## CPU/headless gate for coarse donor/confluence -> fine-node -> residual-1D ownership.

const RES := 3
const RADIUS_M := 2000.0
const ABS_TOL := 1.0e-6
var failures: Array[String] = []


func _ready() -> void:
	var fixture := _make_fixture()
	if not fixture.is_empty():
		_run_gate(fixture)
	if failures.is_empty():
		print("PLANET_RIVER_REFINED_COUPLING: PASS")
		get_tree().quit(0)
	else:
		for f in failures: push_error("PLANET_RIVER_REFINED_COUPLING: " + f)
		get_tree().quit(1)


func _run_gate(fx: Dictionary) -> void:
	var store := fx["store"] as PlanetHydrologyRiverCoupledStore
	var refined := int(fx["refined"])
	var donor_a := int(fx["donor_a"])
	var donor_b := int(fx["donor_b"])

	store.soil_water_m.fill(0.0)
	store.surface_storage_m3.fill(0.0)
	store.channel_storage_m3.fill(0.0)
	store.clear_precipitation()
	store.set_climatology_fallback_enabled(false)
	store.channel_storage_m3[refined] = 100.0
	store.channel_storage_m3[donor_a] = 15.0
	store.channel_storage_m3[donor_b] = 25.0
	store.initial_storage_m3 = store.total_storage_m3()

	var prepared := store.prepare_channel_promotion(refined, 20.0)
	_expect(int(prepared.get("error", FAILED)) == OK, "channel promotion prepare failed")
	if int(prepared.get("error", FAILED)) != OK: return
	var committed := store.commit_promotion(int(prepared["transaction_id"]))
	_expect(int(committed.get("error", FAILED)) == OK, "channel promotion commit failed")
	var reg := store.register_refined_reach(refined, 1234, 0, 20.0)
	_expect(int(reg.get("error", FAILED)) == OK, "refined reach registration failed")
	if int(reg.get("error", FAILED)) != OK: return

	var fine_volume := 20.0
	var combined_before := store.total_storage_m3() + fine_volume
	_expect_close(combined_before, store.initial_storage_m3, 1.0e-5,
		"initial coarse+fine ownership does not close")

	var a_before := store.channel_storage_m3[donor_a]
	var b_before := store.channel_storage_m3[donor_b]
	var report := store.step(10.0)
	_expect(int(report.get("error", FAILED)) == OK, "coupled coarse step failed")
	var donor_debit := (a_before - store.channel_storage_m3[donor_a]) \
		+ (b_before - store.channel_storage_m3[donor_b])
	var pending := store.refined_inflow_available_m3(refined)
	_expect(donor_debit > 0.0, "donors produced no routed inflow")
	_expect_close(pending, donor_debit, maxf(ABS_TOL, donor_debit * 1.0e-9),
		"confluence donor debit did not become exact pending fine inflow")
	_expect(store.refined_inflow_rate(refined) > 0.0,
		"refined inflow rate was not published")

	var transfer := pending * 0.6
	var consumed := store.consume_refined_inflow(refined, transfer)
	_expect(int(consumed.get("error", FAILED)) == OK, "pending inflow consume failed")
	fine_volume += transfer
	_expect_close(store.refined_inflow_available_m3(refined), pending - transfer,
		maxf(ABS_TOL, pending * 1.0e-9), "pending inflow was not debited exactly")

	var return_volume := minf(5.0, fine_volume)
	var accepted := store.accept_refined_outflow(refined, return_volume)
	_expect(int(accepted.get("error", FAILED)) == OK, "fine outflow accept failed")
	fine_volume -= return_volume

	var combined_after := store.total_storage_m3() + fine_volume
	_expect_close(combined_after, combined_before,
		maxf(1.0e-5, combined_before * 1.0e-9),
		"1D<->2D exchange changed combined authoritative volume")
	_expect_close(store.mass_error_m3(), 0.0,
		maxf(1.0e-5, store.initial_storage_m3 * 1.0e-9),
		"coarse ownership ledger does not close after exchange")


func _make_fixture() -> Dictionary:
	var cfg := GenConfig.new(); cfg.face_res = RES; cfg.planet_radius = RADIUS_M
	var grid := PlanetGrid.new(RES, RADIUS_M)
	var fields := PlanetFields.new(cfg, grid)
	fields.elev.fill(100.0); fields.base_elev.fill(100.0); fields.flow_dir.fill(255)
	fields.lake_level.fill(-1.0e9); fields.soil_depth.fill(0.4)
	fields.soil_sand.fill(0.45); fields.soil_silt.fill(0.35); fields.soil_clay.fill(0.20)
	fields.soil_organic.fill(0.05); fields.soil_moisture.fill(0.0); fields.aquifer.fill(0.3)
	fields.floodplain.fill(0.2); fields.relief.fill(20.0); fields.discharge.fill(0.0)
	fields.stream_order.fill(1); fields.river_width.fill(0.0)

	var refined := 0
	var receiver := -1
	var receiver_slot := -1
	for s in 8:
		var n := int(grid.nbr[refined * 8 + s])
		if n != refined:
			receiver = n; receiver_slot = s; break
	if receiver < 0:
		_fail("could not find receiver")
		return {}
	var donors: Array[int] = []
	for c in grid.cell_count:
		if c == refined or c == receiver: continue
		for s in 8:
			if int(grid.nbr[c * 8 + s]) == refined:
				donors.append(c)
				fields.flow_dir[c] = s
				break
		if donors.size() >= 2: break
	if donors.size() < 2:
		_fail("could not find two confluence donors")
		return {}
	fields.flow_dir[refined] = receiver_slot
	fields.elev[refined] = 110.0; fields.elev[receiver] = 90.0
	fields.elev[donors[0]] = 120.0; fields.elev[donors[1]] = 122.0
	fields.discharge[refined] = 8.0; fields.stream_order[refined] = 3
	fields.river_width[refined] = 7.2 * sqrt(8.0)
	var store := PlanetHydrologyRiverCoupledStore.new()
	var err := store.initialize(fields)
	if err != OK:
		_fail("store init failed %d" % int(err)); return {}
	return {"store": store, "refined": refined, "receiver": receiver,
		"donor_a": donors[0], "donor_b": donors[1]}


func _expect(ok: bool, msg: String) -> void:
	if not ok: _fail(msg)
func _expect_close(v: float, e: float, tol: float, msg: String) -> void:
	if absf(v - e) > tol: _fail("%s got %.12g expected %.12g tol %.6g" % [msg, v, e, tol])
func _fail(msg: String) -> void:
	if not failures.has(msg): failures.append(msg)
