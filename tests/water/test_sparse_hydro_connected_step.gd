extends Node
## Renderer-mode smoke gate for the first connected sparse SWE step.
##
## Two resident same-level tiles share an east/west boundary. The western tile
## starts at h=2 m and the eastern tile at h=1 m. One CFL-safe substep must move
## water across the resident boundary, preserve combined water volume, keep an
## interior cell unchanged, and leave atlas A canonical for readback.

const CAPACITY := 2
const TILE_RES := 8
const MID := TILE_RES >> 1
const DX := 1.0
const DT := 0.01
const TIMEOUT_FRAMES := 1200
const MASS_TOLERANCE := 2.0e-3

var _pool: HydroTilePool
var _atlas: SparseHydroAtlasGPU
var _connectivity: SparseHydroConnectivityGPU
var _solver: SparseHydroStepGPU
var _readback: HydroStateReadback
var _frames := 0
var _finished := false
var _initial_mass := 0.0


func _ready() -> void:
	if RenderingServer.get_rendering_device() == null:
		print("SPARSE_HYDRO_CONNECTED_STEP: SKIP (no global RenderingDevice)")
		get_tree().quit(0)
		return

	_pool = HydroTilePool.new(CAPACITY)
	var west := HydroTileKey.new(CubeSphere.FACE_PX, 5, 10, 12)
	var east := west.same_face_neighbor(1, 0)
	_require(east != null, "failed to construct same-face east neighbor")
	_require(_pool.allocate(west, 0) == 0, "west tile did not receive slot 0")
	_require(_pool.allocate(east, 0) == 1, "east tile did not receive slot 1")
	_pool.set_state(west, HydroTilePool.TileState.ACTIVE, "test_bootstrap")
	_pool.set_state(east, HydroTilePool.TileState.ACTIVE, "test_bootstrap")
	if _finished:
		return

	var state := PackedFloat32Array()
	state.resize(CAPACITY * TILE_RES * TILE_RES * 4)
	_fill_uniform_tile(state, 0, 2.0, Vector2.ZERO, 0.0)
	_fill_uniform_tile(state, 1, 1.0, Vector2.ZERO, 0.0)
	_initial_mass = _sum_depth(state)

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
	_require(_atlas.sync_pool(_pool) == OK, "atlas pool identity sync rejected")
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
	_require(_connectivity.sync_pool(_pool) == OK, "connectivity pool sync rejected")
	if _finished:
		return

	var arrays := SparseHydroConnectivityGPU.build_arrays(_pool)
	var slots := arrays["neighbor_slots"] as PackedInt32Array
	_require(slots[0 * 4 + HydroTileTopology.DIR_EAST] == 1,
		"slot0 east connectivity missing")
	_require(slots[1 * 4 + HydroTileTopology.DIR_WEST] == 0,
		"slot1 west connectivity missing")
	if _finished:
		return

	_solver = SparseHydroStepGPU.new()
	add_child(_solver)
	_solver.initialized.connect(_on_solver_initialized)
	_solver.initialization_failed.connect(func(error: Error):
		_fail("sparse step initialization failed (%d)" % int(error)))
	_solver.substep_recorded.connect(_on_substep_recorded)
	var err := _solver.initialize(_atlas, _connectivity)
	if err != OK:
		_fail("sparse step initialize rejected (%d)" % int(err))


func _on_solver_initialized() -> void:
	var step_id := _solver.substep(DT)
	if step_id < 0:
		_fail("sparse substep request rejected")


func _on_substep_recorded(_step_id: int) -> void:
	_readback = HydroStateReadback.new()
	add_child(_readback)
	_readback.state_ready.connect(_on_state_ready)
	_readback.readback_failed.connect(func(_request_id: int, error: Error):
		_fail("state readback failed (%d)" % int(error)))
	if _readback.request_state(_atlas.state_a_rid(), _atlas.total_cell_count()) < 0:
		_fail("state readback request rejected")


func _on_state_ready(_request_id: int, state: PackedFloat32Array) -> void:
	_require(state.size() == CAPACITY * TILE_RES * TILE_RES * 4,
		"readback state size mismatch")
	if _finished:
		return

	var final_mass := _sum_depth(state)
	_require(absf(final_mass - _initial_mass) <= MASS_TOLERANCE,
		"connected sparse step lost/gained water: initial=%.9g final=%.9g delta=%.9g" % [
			_initial_mass, final_mass, final_mass - _initial_mass])

	var west_east_depth := _depth(state, 0, TILE_RES - 1, MID)
	var east_west_depth := _depth(state, 1, 0, MID)
	_require(west_east_depth < 2.0,
		"west tile did not discharge across resident east boundary (h=%.9g)" % west_east_depth)
	_require(east_west_depth > 1.0,
		"east tile did not receive water across resident west boundary (h=%.9g)" % east_west_depth)

	var west_interior := _depth(state, 0, 2, MID)
	var east_interior := _depth(state, 1, TILE_RES - 3, MID)
	_require(absf(west_interior - 2.0) <= 1.0e-6,
		"west interior changed unexpectedly h=%.9g" % west_interior)
	_require(absf(east_interior - 1.0) <= 1.0e-6,
		"east interior changed unexpectedly h=%.9g" % east_interior)

	_finished = true
	print("SPARSE_HYDRO_CONNECTED_STEP: PASS mass_delta=", final_mass - _initial_mass,
		" west_edge=", west_east_depth, " east_edge=", east_west_depth)
	_cleanup()
	get_tree().quit(0)


func _fill_uniform_tile(state: PackedFloat32Array, slot: int, depth: float,
		momentum: Vector2, bed: float) -> void:
	var cells_per_tile := TILE_RES * TILE_RES
	for i in cells_per_tile:
		var o := (slot * cells_per_tile + i) * 4
		state[o] = depth
		state[o + 1] = momentum.x
		state[o + 2] = momentum.y
		state[o + 3] = bed


func _depth(state: PackedFloat32Array, slot: int, x: int, y: int) -> float:
	var cell := slot * TILE_RES * TILE_RES + y * TILE_RES + x
	return state[cell * 4]


func _sum_depth(state: PackedFloat32Array) -> float:
	var total := 0.0
	var cells := CAPACITY * TILE_RES * TILE_RES
	for i in cells:
		total += float(state[i * 4])
	return total


func _require(condition: bool, message: String) -> void:
	if condition or _finished:
		return
	_fail(message)


func _fail(message: String) -> void:
	if _finished:
		return
	_finished = true
	push_error("SPARSE_HYDRO_CONNECTED_STEP: " + message)
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
