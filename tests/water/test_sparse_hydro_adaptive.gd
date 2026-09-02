extends Node
## Renderer-mode gate for sparse GPU adaptive CFL scheduling.
##
## Slot 0 is the only resident tile. Slot 1 deliberately contains stale, very fast
## water while remaining unoccupied; it must not tighten CFL or appear in health
## counts. The first macro advance must split into several safe substeps and fully
## consume requested time. The second uses cap=1 and must under-advance + clamp.

const CAPACITY := 2
const TILE_RES := 8
const DX := 1.0
const FIRST_DT := 0.20
const FIRST_CAP := 8
const CLAMP_DT := 0.50
const TIMEOUT_FRAMES := 1200
const MASS_TOLERANCE := 2.0e-3

var _pool: HydroTilePool
var _atlas: SparseHydroAtlasGPU
var _connectivity: SparseHydroConnectivityGPU
var _solver: SparseHydroStepGPU
var _readback: HydroStateReadback
var _frames := 0
var _finished := false
var _stage := 0
var _initial_mass := 0.0


func _ready() -> void:
	if RenderingServer.get_rendering_device() == null:
		print("SPARSE_HYDRO_ADAPTIVE: SKIP (no global RenderingDevice)")
		get_tree().quit(0)
		return

	_pool = HydroTilePool.new(CAPACITY)
	var key := HydroTileKey.new(CubeSphere.FACE_PZ, 5, 12, 12)
	_require(_pool.allocate(key, 0) == 0, "live tile did not receive slot 0")
	if _finished:
		return

	var state := PackedFloat32Array()
	state.resize(CAPACITY * TILE_RES * TILE_RES * 4)
	_fill_uniform_tile(state, 0, 4.0, Vector2.ZERO, 0.0)
	# If occupancy filtering is broken this stale slot would dominate CFL.
	_fill_uniform_tile(state, 1, 100.0, Vector2(500.0, -300.0), -100.0)
	_initial_mass = float(TILE_RES * TILE_RES) * 4.0

	_atlas = SparseHydroAtlasGPU.new()
	add_child(_atlas)
	_atlas.initialized.connect(_on_atlas_initialized)
	_atlas.initialization_failed.connect(func(error: Error):
		_fail("atlas initialization failed (%d)" % int(error)))
	var err := _atlas.initialize(CAPACITY, TILE_RES, DX, state)
	if err != OK:
		_fail("atlas initialize rejected (%d)" % int(err))


func _process(_delta: float) -> void:
	if _finished:
		return
	_frames += 1
	if _frames > TIMEOUT_FRAMES:
		_fail("timed out")


func _on_atlas_initialized() -> void:
	_require(_atlas.sync_pool(_pool) == OK, "atlas pool sync rejected")
	if _finished:
		return
	_connectivity = SparseHydroConnectivityGPU.new()
	add_child(_connectivity)
	_connectivity.initialized.connect(_on_connectivity_initialized)
	_connectivity.initialization_failed.connect(func(error: Error):
		_fail("connectivity initialization failed (%d)" % int(error)))
	var err := _connectivity.initialize(CAPACITY)
	if err != OK:
		_fail("connectivity initialize rejected (%d)" % int(err))


func _on_connectivity_initialized() -> void:
	_require(_connectivity.sync_pool(_pool) == OK, "connectivity sync rejected")
	if _finished:
		return
	_solver = SparseHydroStepGPU.new()
	_solver.manning_n = 0.0
	add_child(_solver)
	_solver.initialized.connect(_on_solver_initialized)
	_solver.initialization_failed.connect(func(error: Error):
		_fail("adaptive sparse solver initialization failed (%d)" % int(error)))
	_solver.diagnostics_ready.connect(_on_diagnostics_ready)
	var err := _solver.initialize(_atlas, _connectivity)
	if err != OK:
		_fail("adaptive sparse solver initialize rejected (%d)" % int(err))


func _on_solver_initialized() -> void:
	_stage = 1
	if _solver.advance(FIRST_DT, FIRST_CAP, true) < 0:
		_fail("first adaptive advance rejected")


func _on_diagnostics_ready(_step_id: int, d: Dictionary) -> void:
	if _stage == 1:
		_require(int(d["pre_invalid_cells"]) == 0 and int(d["post_invalid_cells"]) == 0,
			"invalid cells reported during first adaptive advance")
		_require(int(d["pre_wet_cells"]) == TILE_RES * TILE_RES,
			"unoccupied stale slot leaked into wet-cell reduction")
		_require(float(d["pre_max_depth_m"]) > 3.99 and float(d["pre_max_depth_m"]) < 4.01,
			"unoccupied stale slot leaked into max-depth reduction: %s" % str(d))
		_require(float(d["pre_max_speed_mps"]) > 6.0 and float(d["pre_max_speed_mps"]) < 7.0,
			"unexpected live-tile characteristic speed: %s" % str(d))
		_require(int(d["steps_taken"]) >= 3,
			"0.2 s macro step was not split into multiple CFL substeps: %s" % str(d))
		_require(not bool(d["cfl_clamped"]), "first advance unexpectedly hit CFL cap")
		_require(absf(float(d["advanced_dt_s"]) - FIRST_DT) <= 2.0e-6,
			"first advance did not consume requested time: %s" % str(d))
		_require(float(d["remaining_dt_s"]) <= 2.0e-6,
			"first advance left macro time unconsumed: %s" % str(d))
		if _finished:
			return
		_stage = 2
		if _solver.advance(CLAMP_DT, 1, true) < 0:
			_fail("clamped adaptive advance rejected")
		return

	if _stage == 2:
		_require(bool(d["cfl_clamped"]), "one-substep oversized advance did not clamp")
		_require(int(d["steps_taken"]) == 1, "clamped advance should take exactly one step")
		_require(float(d["advanced_dt_s"]) < CLAMP_DT,
			"clamped advance incorrectly consumed unsafe macro dt")
		_require(float(d["remaining_dt_s"]) > 0.0,
			"clamped advance did not preserve unsimulated time")
		_require(int(d["post_invalid_cells"]) == 0, "clamped advance produced invalid state")
		if _finished:
			return
		_request_final_state()


func _request_final_state() -> void:
	_stage = 3
	_readback = HydroStateReadback.new()
	add_child(_readback)
	_readback.state_ready.connect(_on_state_ready)
	_readback.readback_failed.connect(func(_request_id: int, error: Error):
		_fail("final state readback failed (%d)" % int(error)))
	if _readback.request_state(_atlas.state_a_rid(), _atlas.total_cell_count()) < 0:
		_fail("final state readback rejected")


func _on_state_ready(_request_id: int, state: PackedFloat32Array) -> void:
	var live_mass := 0.0
	for y in TILE_RES:
		for x in TILE_RES:
			var i := y * TILE_RES + x
			live_mass += float(state[i * 4])
	_require(absf(live_mass - _initial_mass) <= MASS_TOLERANCE,
		"adaptive closed tile changed water mass initial=%.9g final=%.9g" % [
			_initial_mass, live_mass])
	# Commit pass clears nonresident/recycled scratch, so stale slot must not become
	# authoritative after any adaptive iteration.
	var stale_depth_sum := 0.0
	var stale_base := TILE_RES * TILE_RES
	for i in TILE_RES * TILE_RES:
		stale_depth_sum += float(state[(stale_base + i) * 4])
	_require(absf(stale_depth_sum) <= 1.0e-6,
		"unoccupied stale slot survived canonical commit")
	if _finished:
		return
	_finished = true
	print("SPARSE_HYDRO_ADAPTIVE: PASS diagnostics=", _solver.latest_diagnostics())
	_cleanup()
	get_tree().quit(0)


func _fill_uniform_tile(state: PackedFloat32Array, slot: int, depth: float,
		momentum: Vector2, bed: float) -> void:
	var cells := TILE_RES * TILE_RES
	for i in cells:
		var o := (slot * cells + i) * 4
		state[o] = depth
		state[o + 1] = momentum.x
		state[o + 2] = momentum.y
		state[o + 3] = bed


func _require(condition: bool, message: String) -> void:
	if condition or _finished:
		return
	_fail(message)


func _fail(message: String) -> void:
	if _finished:
		return
	_finished = true
	push_error("SPARSE_HYDRO_ADAPTIVE: " + message)
	_cleanup()
	get_tree().quit(1)


func _cleanup() -> void:
	if _readback != null and is_instance_valid(_readback):
		_readback.queue_free()
	if _solver != null and is_instance_valid(_solver):
		_solver.release()
		_solver.queue_free()
	if _connectivity != null and is_instance_valid(_connectivity):
		_connectivity.release()
		_connectivity.queue_free()
	if _atlas != null and is_instance_valid(_atlas):
		_atlas.release()
		_atlas.queue_free()
