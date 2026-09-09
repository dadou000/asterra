extends Node
## Renderer-mode combined gate for coarse refined-reach store + real GPU mouth
## exchange + production coupling controller.

const RES := 3
const RADIUS_M := 2000.0
const TILE_RES := 8
const DX := 2.0
const INITIAL_FINE_M3 := 20.0
const QUEUED_INFLOW_M3 := 4.0
const EXCHANGE_DT := 0.25
const TIMEOUT_FRAMES := 1600
const TOL := 4.0e-3

var store: PlanetHydrologyRiverCoupledStore
var refined := -1
var buffer_cell := -1
var scheduler: SparseHydroScheduler
var atlas: SparseHydroAtlasGPU
var runtime: RiverCouplingRuntimeStub
var coupling: HydroRiverReachCouplingProduction
var readback: HydroStateReadback
var key: HydroTileKey
var initial_total := 0.0
var exchange_report: Dictionary = {}
var frames := 0
var finished := false
var coarse_enabled_before := true


func _ready() -> void:
	if RenderingServer.get_rendering_device() == null:
		print("PLANET_RIVER_CONTINUOUS_COUPLING: SKIP (RenderingDevice unavailable)")
		get_tree().quit(0)
		return
	var fx := _make_store_fixture()
	if fx.is_empty(): return
	store = fx["store"] as PlanetHydrologyRiverCoupledStore
	refined = int(fx["refined"])
	buffer_cell = int(fx["buffer"])

	store.soil_water_m.fill(0.0); store.surface_storage_m3.fill(0.0)
	store.channel_storage_m3.fill(0.0); store.clear_precipitation()
	store.set_climatology_fallback_enabled(false)
	store.channel_storage_m3[refined] = 100.0
	store.channel_storage_m3[buffer_cell] = QUEUED_INFLOW_M3
	store.initial_storage_m3 = store.total_storage_m3()
	initial_total = store.initial_storage_m3
	var prepared := store.prepare_channel_promotion(refined, INITIAL_FINE_M3)
	if int(prepared.get("error", FAILED)) != OK: _fail("promotion prepare failed"); return
	var committed := store.commit_promotion(int(prepared["transaction_id"]))
	if int(committed.get("error", FAILED)) != OK: _fail("promotion commit failed"); return

	scheduler = SparseHydroScheduler.new(1)
	key = HydroTileKey.new(CubeSphere.FACE_PX, 5, 10, 10)
	if scheduler.wake(key, 0, "continuous_coupling_test") != 0:
		_fail("scheduler did not allocate slot zero"); return

	var initial_state := _fine_state(INITIAL_FINE_M3)
	atlas = SparseHydroAtlasGPU.new(); add_child(atlas)
	atlas.initialized.connect(func(): _on_atlas_initialized(initial_state))
	atlas.initialization_failed.connect(func(error: Error): _fail("atlas init %d" % int(error)))
	var err := atlas.initialize(1, TILE_RES, DX, initial_state)
	if err != OK: _fail("atlas initialize rejected %d" % int(err))


func _process(_delta: float) -> void:
	if finished: return
	frames += 1
	if frames > TIMEOUT_FRAMES: _fail("timed out")


func _on_atlas_initialized(initial_state: PackedFloat32Array) -> void:
	if atlas.stage_slot_state(0, initial_state) != OK \
			or atlas.set_occupancy(PackedInt32Array([1])) != OK:
		_fail("failed to publish initial fine state"); return
	runtime = RiverCouplingRuntimeStub.new(); add_child(runtime)
	runtime.atlas = atlas; runtime.scheduler = scheduler; runtime.enabled = true
	coupling = HydroRiverReachCouplingProduction.new(); add_child(coupling)
	coupling.initialized.connect(_on_coupling_initialized)
	coupling.initialization_failed.connect(func(error: Error): _fail("coupling init %d" % int(error)))
	coupling.exchange_completed.connect(_on_exchange_completed)
	coupling.coupling_failed.connect(func(error: Error, stage: String):
		_fail("coupling failed %s (%d)" % [stage, int(error)]))
	var err := coupling.initialize(store, runtime)
	if err != OK: _fail("coupling initialize rejected %d" % int(err))


func _on_coupling_initialized() -> void:
	var report := {
		"cell": refined,
		"tile_id": key.packed(),
		"slot": 0,
		"represented_volume_m3": INITIAL_FINE_M3,
		"corridor_center_cell": Vector2(4.0, 4.0),
		"corridor_direction_cell": Vector2(1.0, 0.0),
		"corridor_half_width_m": 2.0,
		"local_velocity_mps": Vector2(1.0, 0.0),
	}
	var err := coupling.register_promoted_reach(report)
	if err != OK: _fail("coupling registration failed %d" % int(err)); return

	# Move an already-coarse-owned donor parcel into the refined pending queue. This
	# is an internal coarse transfer and must not change total coarse storage.
	store.channel_storage_m3[buffer_cell] -= QUEUED_INFLOW_M3
	store.refined_pending_inflow_m3[refined] += QUEUED_INFLOW_M3
	store.refined_inflow_rate_m3s[refined] = QUEUED_INFLOW_M3 / EXCHANGE_DT
	var before_combined := store.total_storage_m3() + INITIAL_FINE_M3
	if absf(before_combined - initial_total) > TOL:
		_fail("pre-exchange combined ownership does not close"); return

	coarse_enabled_before = bool(PersistentHydrologySystem.enabled)
	runtime.cycle_completed.emit(1, {"advanced_dt_s": EXCHANGE_DT})


func _on_exchange_completed(_request_id: int, report: Dictionary) -> void:
	exchange_report = report.duplicate(true)
	if absf(float(report.get("added_1d_to_2d_m3", 0.0)) - QUEUED_INFLOW_M3) > TOL:
		_fail("queued coarse inflow was not added exactly"); return
	if float(report.get("removed_2d_to_1d_m3", 0.0)) <= 0.0:
		_fail("fine downstream outflow was not returned to 1D"); return
	if store.refined_inflow_available_m3(refined) > TOL:
		_fail("acknowledged pending inflow remains in coarse queue"); return
	readback = HydroStateReadback.new(); add_child(readback)
	readback.state_ready.connect(_on_state_ready)
	readback.readback_failed.connect(func(_id: int, error: Error): _fail("readback %d" % int(error)))
	if readback.request_state(atlas.state_a_rid(), TILE_RES * TILE_RES) < 0:
		_fail("state readback request rejected")


func _on_state_ready(_request_id: int, state: PackedFloat32Array) -> void:
	var fine_volume := 0.0
	for i in TILE_RES * TILE_RES:
		fine_volume += maxf(float(state[i * 4]), 0.0) * DX * DX
	var combined := store.total_storage_m3() + fine_volume
	if absf(combined - initial_total) > TOL:
		_fail("combined coarse+fine mass mismatch got %.9g expected %.9g" % [combined, initial_total]); return
	if absf(store.mass_error_m3()) > TOL:
		_fail("coarse ownership ledger error %.9g" % store.mass_error_m3()); return
	var expected_fine := INITIAL_FINE_M3 \
		+ float(exchange_report["added_1d_to_2d_m3"]) \
		- float(exchange_report["removed_2d_to_1d_m3"])
	if absf(fine_volume - expected_fine) > TOL:
		_fail("fine exchange equation mismatch got %.9g expected %.9g" % [fine_volume, expected_fine]); return
	finished = true
	PersistentHydrologySystem.enabled = coarse_enabled_before
	print("PLANET_RIVER_CONTINUOUS_COUPLING: PASS fine=", fine_volume,
		" add=", exchange_report["added_1d_to_2d_m3"],
		" remove=", exchange_report["removed_2d_to_1d_m3"])
	_cleanup(); get_tree().quit(0)


func _fine_state(target_volume: float) -> PackedFloat32Array:
	var state := PackedFloat32Array(); state.resize(TILE_RES * TILE_RES * 4)
	var wet_cells := 16.0 # two rows across eight columns for this corridor
	var h := target_volume / (wet_cells * DX * DX)
	for y in TILE_RES:
		for x in TILE_RES:
			var i := y * TILE_RES + x
			state[i * 4 + 3] = 100.0
			if y == 3 or y == 4:
				state[i * 4] = h
				state[i * 4 + 1] = h
	return state


func _make_store_fixture() -> Dictionary:
	var cfg := GenConfig.new(); cfg.face_res = RES; cfg.planet_radius = RADIUS_M
	var grid := PlanetGrid.new(RES, RADIUS_M)
	var fields := PlanetFields.new(cfg, grid)
	fields.elev.fill(100.0); fields.base_elev.fill(100.0); fields.flow_dir.fill(255)
	fields.lake_level.fill(-1.0e9); fields.soil_depth.fill(0.4)
	fields.soil_sand.fill(0.45); fields.soil_silt.fill(0.35); fields.soil_clay.fill(0.2)
	fields.soil_organic.fill(0.05); fields.soil_moisture.fill(0.0); fields.aquifer.fill(0.3)
	fields.floodplain.fill(0.2); fields.relief.fill(20.0); fields.discharge.fill(0.0)
	fields.stream_order.fill(1); fields.river_width.fill(0.0)
	var cell := 0
	var receiver := -1; var slot := -1
	for s in 8:
		var n := int(grid.nbr[cell * 8 + s])
		if n != cell: receiver = n; slot = s; break
	if receiver < 0: _fail("no receiver for fixture"); return {}
	var buffer := -1
	for c in grid.cell_count:
		if c != cell and c != receiver: buffer = c; break
	if buffer < 0: _fail("no buffer cell for fixture"); return {}
	fields.flow_dir[cell] = slot
	fields.elev[cell] = 110.0; fields.elev[receiver] = 90.0
	fields.discharge[cell] = 8.0; fields.stream_order[cell] = 3
	fields.river_width[cell] = 7.2 * sqrt(8.0)
	var s := PlanetHydrologyRiverCoupledStore.new()
	var err := s.initialize(fields)
	if err != OK: _fail("store init %d" % int(err)); return {}
	return {"store": s, "refined": cell, "buffer": buffer}


func _fail(message: String) -> void:
	if finished: return
	finished = true
	if PersistentHydrologySystem != null: PersistentHydrologySystem.enabled = coarse_enabled_before
	push_error("PLANET_RIVER_CONTINUOUS_COUPLING: " + message)
	_cleanup(); get_tree().quit(1)


func _cleanup() -> void:
	if coupling != null and is_instance_valid(coupling): coupling.release()
	if atlas != null and is_instance_valid(atlas): atlas.release()
