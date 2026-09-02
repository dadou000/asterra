extends Node
## Renderer-mode cube-seam continuity gate for sparse SWE.
##
## Two resident tiles on different cube faces contain the same physical uniform
## current, but their stored (hu,hv) vectors are expressed in different face-local
## frames. This case deliberately crosses +X north into a polar face so the local
## solver axes rotate across the seam. A missing momentum rotation therefore
## creates an immediate numerical wave instead of accidentally passing.

const CAPACITY := 2
const TILE_RES := 8
const MID := TILE_RES >> 1
const DX := 1.0
const DT := 0.01
const TIMEOUT_FRAMES := 1200
const STATE_TOLERANCE := 2.0e-5
const MASS_TOLERANCE := 2.0e-3
const SOURCE_DIRECTION := HydroTileTopology.DIR_NORTH

var _pool: HydroTilePool
var _atlas: SparseHydroAtlasGPU
var _connectivity: SparseHydroConnectivityGPU
var _solver: SparseHydroStepGPU
var _readback: HydroStateReadback
var _source: HydroTileKey
var _destination: HydroTileKey
var _link: Dictionary
var _source_q := Vector2.ZERO
var _destination_q := Vector2.ZERO
var _frames := 0
var _finished := false
var _initial_mass := 0.0


func _ready() -> void:
	if RenderingServer.get_rendering_device() == null:
		print("SPARSE_HYDRO_SEAM_STEP: SKIP (no global RenderingDevice)")
		get_tree().quit(0)
		return

	const LEVEL := 5
	var side := 1 << LEVEL
	_source = HydroTileKey.new(CubeSphere.FACE_PX, LEVEL, side >> 1, side - 1)
	_link = HydroTileTopology.neighbor(_source, SOURCE_DIRECTION)
	_require(not _link.is_empty() and bool(_link["crossed_face"]),
		"selected source did not resolve across a cube seam")
	if _finished:
		return
	_destination = _link["key"] as HydroTileKey
	var destination_direction := int(_link["destination_direction"])
	_require(destination_direction != HydroTileTopology.opposite_direction(SOURCE_DIRECTION),
		"selected seam did not rotate local edge axes; test would be too weak")
	if _finished:
		return
	var back := HydroTileTopology.neighbor(_destination, destination_direction)
	_require(not back.is_empty() and (back["key"] as HydroTileKey).equals(_source),
		"seam reciprocal topology missing")
	if _finished:
		return

	_source_q = HydroEdgeFrame.edge_normal(SOURCE_DIRECTION) * 0.40 \
		+ HydroEdgeFrame.edge_tangent(SOURCE_DIRECTION) * 0.15
	_destination_q = HydroEdgeFrame.momentum_to_source(_source_q,
		destination_direction, int(back["destination_direction"]),
		int(back["edge_orientation"]))
	var mapped_back := HydroEdgeFrame.momentum_across_link(
		_destination_q, SOURCE_DIRECTION, _link)
	_require(mapped_back.distance_to(_source_q) < 1.0e-7,
		"CPU seam frame construction is not reciprocal")
	_require(_destination_q.distance_to(_source_q) > 0.1,
		"seam test did not produce a meaningfully rotated local momentum vector")
	if _finished:
		return

	_pool = HydroTilePool.new(CAPACITY)
	_require(_pool.allocate(_source, 0) == 0, "source tile did not receive slot 0")
	_require(_pool.allocate(_destination, 0) == 1, "destination tile did not receive slot 1")
	_pool.set_state(_source, HydroTilePool.TileState.ACTIVE, "test_bootstrap")
	_pool.set_state(_destination, HydroTilePool.TileState.ACTIVE, "test_bootstrap")
	if _finished:
		return

	var state := PackedFloat32Array()
	state.resize(CAPACITY * TILE_RES * TILE_RES * 4)
	_fill_uniform_tile(state, 0, 1.0, _source_q, 0.0)
	_fill_uniform_tile(state, 1, 1.0, _destination_q, 0.0)
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
	_require(_connectivity.sync_pool(_pool) == OK, "connectivity pool sync rejected")
	if _finished:
		return
	var arrays := SparseHydroConnectivityGPU.build_arrays(_pool)
	var slots := arrays["neighbor_slots"] as PackedInt32Array
	var links := arrays["neighbor_links"] as PackedInt32Array
	var source_index := SOURCE_DIRECTION
	_require(slots[source_index] == 1, "source seam does not point to destination slot")
	_require(SparseHydroConnectivityGPU.unpack_destination_direction(links[source_index])
		== int(_link["destination_direction"]), "GPU destination edge encoding mismatch")
	_require(SparseHydroConnectivityGPU.unpack_reversed(links[source_index])
		== (int(_link["edge_orientation"]) < 0), "GPU seam reversal encoding mismatch")
	if _finished:
		return

	_solver = SparseHydroStepGPU.new()
	_solver.manning_n = 0.0
	add_child(_solver)
	_solver.initialized.connect(_on_solver_initialized)
	_solver.initialization_failed.connect(func(error: Error):
		_fail("sparse step initialization failed (%d)" % int(error)))
	_solver.substep_recorded.connect(_on_substep_recorded)
	var err := _solver.initialize(_atlas, _connectivity)
	if err != OK:
		_fail("sparse step initialize rejected (%d)" % int(err))


func _on_solver_initialized() -> void:
	if _solver.substep(DT) < 0:
		_fail("sparse seam substep request rejected")


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
		"cube seam step changed total water mass delta=%.9g" % (final_mass - _initial_mass))

	var source_cell := _edge_cell(SOURCE_DIRECTION, MID)
	var destination_k := TILE_RES - 1 - MID \
		if int(_link["edge_orientation"]) < 0 else MID
	var destination_cell := _edge_cell(int(_link["destination_direction"]), destination_k)
	var source_state := _cell(state, 0, source_cell)
	var destination_state := _cell(state, 1, destination_cell)

	_require(absf(source_state.x - 1.0) <= STATE_TOLERANCE,
		"source seam depth developed discontinuity h=%.9g" % source_state.x)
	_require(Vector2(source_state.y, source_state.z).distance_to(_source_q) <= STATE_TOLERANCE,
		"source seam momentum changed: got=%s expected=%s" % [
			str(Vector2(source_state.y, source_state.z)), str(_source_q)])
	_require(absf(destination_state.x - 1.0) <= STATE_TOLERANCE,
		"destination seam depth developed discontinuity h=%.9g" % destination_state.x)
	_require(Vector2(destination_state.y, destination_state.z).distance_to(_destination_q)
		<= STATE_TOLERANCE,
		"destination seam momentum changed: got=%s expected=%s" % [
			str(Vector2(destination_state.y, destination_state.z)), str(_destination_q)])

	_finished = true
	print("SPARSE_HYDRO_SEAM_STEP: PASS source=", _source,
		" destination=", _destination, " destination_edge=", _link["destination_direction"],
		" orientation=", _link["edge_orientation"], " q_src=", _source_q,
		" q_dst=", _destination_q)
	_cleanup()
	get_tree().quit(0)


func _edge_cell(direction: int, k: int) -> Vector2i:
	match direction:
		HydroTileTopology.DIR_WEST: return Vector2i(0, k)
		HydroTileTopology.DIR_EAST: return Vector2i(TILE_RES - 1, k)
		HydroTileTopology.DIR_SOUTH: return Vector2i(k, 0)
		HydroTileTopology.DIR_NORTH: return Vector2i(k, TILE_RES - 1)
	return Vector2i.ZERO


func _cell(state: PackedFloat32Array, slot: int, p: Vector2i) -> Vector4:
	var i := slot * TILE_RES * TILE_RES + p.y * TILE_RES + p.x
	var o := i * 4
	return Vector4(state[o], state[o + 1], state[o + 2], state[o + 3])


func _fill_uniform_tile(state: PackedFloat32Array, slot: int, depth: float,
		momentum: Vector2, bed: float) -> void:
	var cells_per_tile := TILE_RES * TILE_RES
	for i in cells_per_tile:
		var o := (slot * cells_per_tile + i) * 4
		state[o] = depth
		state[o + 1] = momentum.x
		state[o + 2] = momentum.y
		state[o + 3] = bed


func _sum_depth(state: PackedFloat32Array) -> float:
	var total := 0.0
	var cell_count := CAPACITY * TILE_RES * TILE_RES
	for i in cell_count:
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
	push_error("SPARSE_HYDRO_SEAM_STEP: " + message)
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
