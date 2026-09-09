extends Node
## End-to-end renderer gate for the first self-expanding sparse flood path.
##
## A wet source tile on +X sits at its north cube seam. Only that boundary is
## declared reachable. GPU activity produces predictive frontier candidates,
## policy reserves the polar-face neighbor without publishing it, the destination
## bed is reconstructed on GPU, the conservative handoff pre-wets the mapped edge,
## then the tile becomes active and connectivity opens. Finally adaptive sparse SWE
## advances across the newly live seam.

const CAPACITY := 2
const TILE_RES := 8
const MID := TILE_RES >> 1
const DX := 1.0
const SOURCE_DIRECTION := HydroTileTopology.DIR_NORTH
const FRONTIER_THRESHOLD := 0.05
const SEED_DT := 0.02
const MAX_SEED_FRACTION := 0.12
const SOLVER_DT := 0.03
const TIMEOUT_FRAMES := 1800
const MASS_TOLERANCE := 3.0e-3
const MOMENTUM_TOLERANCE := 3.0e-5
const MACRO_FACE_RES := 16

var _scheduler: SparseHydroScheduler
var _atlas: SparseHydroAtlasGPU
var _identity_bridge: SparseHydroIdentityBridge
var _connectivity: SparseHydroConnectivityGPU
var _terrain_bed: HydroTerrainBedGPU
var _activity: HydroTileActivityGPU
var _frontier: HydroFrontierCandidatesGPU
var _activation: HydroFrontierActivationPipeline
var _solver: SparseHydroStepGPU
var _readback: HydroStateReadback
var _macro: Texture2DArray

var _source: HydroTileKey
var _destination: HydroTileKey
var _link: Dictionary
var _source_q := Vector2(0.10, 0.20)
var _initial_mass := 0.0
var _handoff_destination_mass := 0.0
var _stage := 0
var _frames := 0
var _finished := false


func _ready() -> void:
	if RenderingServer.get_rendering_device() == null:
		print("SPARSE_HYDRO_FRONTIER_HANDOFF: SKIP (no global RenderingDevice)")
		get_tree().quit(0)
		return

	const LEVEL := 5
	var side := 1 << LEVEL
	_source = HydroTileKey.new(CubeSphere.FACE_PX, LEVEL, side >> 1, side - 1)
	_link = HydroTileTopology.neighbor(_source, SOURCE_DIRECTION)
	_require(not _link.is_empty() and bool(_link["crossed_face"]),
		"fixture source did not cross cube seam")
	if _finished:
		return
	_destination = _link["key"] as HydroTileKey
	_require(int(_link["destination_direction"])
		!= HydroTileTopology.opposite_direction(SOURCE_DIRECTION),
		"fixture seam does not rotate local frame")
	if _finished:
		return

	_scheduler = SparseHydroScheduler.new(CAPACITY)
	_scheduler.wake_flux_threshold_m3s = FRONTIER_THRESHOLD
	_require(_scheduler.wake(_source, 0, "seed_source") == 0,
		"source tile did not receive slot 0")
	if _finished:
		return

	var state := PackedFloat32Array()
	state.resize(CAPACITY * TILE_RES * TILE_RES * 4)
	_fill_uniform_tile(state, 0, 1.0, _source_q, 0.0)
	# Slot 1 deliberately contains impossible stale state. GPU terrain staging must
	# overwrite both A and B before the reservation is published.
	_fill_uniform_tile(state, 1, 50.0, Vector2(900.0, -700.0), -50.0)
	_initial_mass = float(TILE_RES * TILE_RES) * 1.0

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
		_fail("timed out at stage %d" % _stage)


func _on_atlas_initialized() -> void:
	_identity_bridge = SparseHydroIdentityBridge.new()
	add_child(_identity_bridge)
	var bridge_error := _identity_bridge.bind(_scheduler, _atlas)
	_require(bridge_error == OK, "identity bridge bind failed (%d)" % int(bridge_error))
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
	_require(_connectivity.sync_pool(_scheduler.pool) == OK,
		"initial connectivity sync failed")
	if _finished:
		return

	_macro = _make_zero_macro_texture()
	_require(_macro != null, "synthetic macro texture creation failed")
	if _finished:
		return
	_terrain_bed = HydroTerrainBedGPU.new()
	add_child(_terrain_bed)
	_terrain_bed.initialized.connect(_on_terrain_bed_initialized)
	_terrain_bed.initialization_failed.connect(func(error: Error):
		_fail("terrain bed initialization failed (%d)" % int(error)))
	_terrain_bed.stage_failed.connect(func(_request_id: int, error: Error):
		_fail("terrain bed staging failed (%d)" % int(error)))
	var terrain_error := _terrain_bed.initialize(_atlas, _macro, MACRO_FACE_RES, {
		"planet_radius": 1000.0,
		"base_spacing": 1.0,
		"terrain_level": 0,
		"detail_seed": 1,
		"detail_strength": 0.0,
	})
	if terrain_error != OK:
		_fail("terrain bed initialize rejected (%d)" % int(terrain_error))


func _make_zero_macro_texture() -> Texture2DArray:
	var images: Array[Image] = []
	for _face in 6:
		var image := Image.create(MACRO_FACE_RES, MACRO_FACE_RES, false, Image.FORMAT_RF)
		image.fill(Color(0.0, 0.0, 0.0, 1.0))
		if image.generate_mipmaps() != OK:
			return null
		images.append(image)
	var texture := Texture2DArray.new()
	return texture if texture.create_from_images(images) == OK else null


func _on_terrain_bed_initialized() -> void:
	_activation = HydroFrontierActivationPipeline.new()
	add_child(_activation)
	_activation.initialized.connect(_on_activation_initialized)
	_activation.initialization_failed.connect(func(error: Error):
		_fail("activation pipeline initialization failed (%d)" % int(error)))
	_activation.batch_completed.connect(_on_activation_completed)
	_activation.batch_failed.connect(func(_batch_id: int, error: Error):
		_fail("activation batch failed (%d)" % int(error)))
	var err := _activation.initialize(_scheduler, _atlas, _connectivity, _identity_bridge)
	if err != OK:
		_fail("activation initialize rejected (%d)" % int(err))


func _on_activation_initialized() -> void:
	_activity = HydroTileActivityGPU.new()
	add_child(_activity)
	_activity.initialized.connect(_on_activity_initialized)
	_activity.initialization_failed.connect(func(error: Error):
		_fail("activity initialization failed (%d)" % int(error)))
	_activity.summaries_ready.connect(_on_summaries_ready)
	_activity.classification_failed.connect(func(_request_id: int, error: Error):
		_fail("activity classification failed (%d)" % int(error)))
	var err := _activity.initialize(_atlas.state_a_rid(), _atlas.occupancy_rid(),
		CAPACITY, TILE_RES, DX)
	if err != OK:
		_fail("activity initialize rejected (%d)" % int(err))


func _on_activity_initialized() -> void:
	_stage = 1
	if _activity.classify(true) < 0:
		_fail("activity classify rejected")


func _on_summaries_ready(_request_id: int, summaries: Array[Dictionary]) -> void:
	_require(summaries.size() == CAPACITY, "summary count mismatch")
	var source_summary := summaries[0]
	var stale_summary := summaries[1]
	_require(bool(source_summary["active"]), "source slot is not GPU-active")
	_require(not bool(stale_summary["active"]),
		"stale destination slot became active before reservation")
	_require(float(source_summary["wetting_north_m3s"]) > FRONTIER_THRESHOLD,
		"source hydrostatic/momentum head did not produce north wetting potential")
	if _finished:
		return

	_frontier = HydroFrontierCandidatesGPU.new()
	add_child(_frontier)
	_frontier.initialized.connect(_on_frontier_initialized)
	_frontier.initialization_failed.connect(func(error: Error):
		_fail("frontier initialization failed (%d)" % int(error)))
	_frontier.candidates_ready.connect(_on_candidates_ready)
	_frontier.queue_failed.connect(func(_request_id: int, error: Error):
		_fail("frontier queue failed (%d)" % int(error)))
	var err := _frontier.initialize(_activity.summary_rid(), _atlas.tile_metadata_rid(),
		CAPACITY, FRONTIER_THRESHOLD)
	if err != OK:
		_fail("frontier initialize rejected (%d)" % int(err))


func _on_frontier_initialized() -> void:
	if _frontier.generate(true) < 0:
		_fail("frontier generation rejected")


func _on_candidates_ready(_request_id: int, candidates: Array[Dictionary],
		overflow: bool) -> void:
	_require(not overflow, "frontier queue overflowed")
	var found_north := false
	for candidate in candidates:
		if int(candidate.get("slot", -1)) == 0 \
				and int(candidate.get("direction", -1)) == SOURCE_DIRECTION:
			found_north = true
	_require(found_north, "north seam frontier candidate missing")
	if _finished:
		return

	_stage = 2
	var batch := _activation.process_candidates(candidates,
		Callable(self, &"_reachability"), Callable(self, &"_destination_state"),
		SEED_DT, MAX_SEED_FRACTION, 9.81)
	if batch < 0:
		_fail("activation batch rejected")


func _reachability(_source_key: HydroTileKey, direction: int,
		destination_key: HydroTileKey, _flux_m3s: float, _topology_link: Dictionary) -> bool:
	return direction == SOURCE_DIRECTION and destination_key.equals(_destination)


func _destination_state(destination_key: HydroTileKey, destination_slot: int) -> Dictionary:
	_require(destination_key.equals(_destination), "initializer received wrong destination")
	_require(destination_slot == 1, "reserved destination did not receive slot 1")
	var zero_deltas := PackedFloat32Array()
	zero_deltas.resize(TILE_RES * TILE_RES)
	return _terrain_bed.stage_reserved_tile(destination_key, destination_slot, zero_deltas)


func _on_activation_completed(_batch_id: int, results: Array[Dictionary]) -> void:
	var activated := false
	for result in results:
		if int(result.get("destination_tile_id", -1)) == _destination.packed() \
				and result.get("reason", "") == "activated":
			activated = true
	_require(activated, "destination was not activated: %s" % str(results))
	var record := _scheduler.pool.record(_destination)
	_require(int(record.get("state", -1)) == HydroTilePool.TileState.ACTIVE,
		"destination policy state is not ACTIVE")
	_require(_scheduler.pool.slot_for(_destination) == 1,
		"destination slot identity changed")

	var arrays := SparseHydroConnectivityGPU.build_arrays(_scheduler.pool)
	var slots := arrays["neighbor_slots"] as PackedInt32Array
	_require(slots[0 * 4 + SOURCE_DIRECTION] == 1,
		"source connectivity did not open after activation")
	if _finished:
		return
	_stage = 3
	_request_state(_on_handoff_state)


func _request_state(callback: Callable) -> void:
	if _readback != null and is_instance_valid(_readback):
		_readback.queue_free()
	_readback = HydroStateReadback.new()
	add_child(_readback)
	_readback.state_ready.connect(func(_request_id: int, state: PackedFloat32Array):
		callback.call(state))
	_readback.readback_failed.connect(func(_request_id: int, error: Error):
		_fail("state readback failed (%d)" % int(error)))
	if _readback.request_state(_atlas.state_a_rid(), _atlas.total_cell_count()) < 0:
		_fail("state readback rejected")


func _on_handoff_state(state: PackedFloat32Array) -> void:
	var total_mass := _sum_depth(state)
	_require(absf(total_mass - _initial_mass) <= MASS_TOLERANCE,
		"handoff changed total mass delta=%.9g" % (total_mass - _initial_mass))
	var source_cell := _edge_cell(SOURCE_DIRECTION, MID)
	var destination_k := TILE_RES - 1 - MID \
		if int(_link["edge_orientation"]) < 0 else MID
	var destination_cell := _edge_cell(int(_link["destination_direction"]), destination_k)
	var src := _cell(state, 0, source_cell)
	var dst := _cell(state, 1, destination_cell)
	_require(src.x < 1.0 and dst.x > 0.0,
		"handoff did not transfer water src_h=%.9g dst_h=%.9g" % [src.x, dst.x])
	_require(absf(dst.w) <= 1.0e-6,
		"GPU destination terrain was not staged before handoff bed=%.9g" % dst.w)

	var removed_q := _source_q - Vector2(src.y, src.z)
	var destination_q_in_source := HydroEdgeFrame.momentum_across_link(
		Vector2(dst.y, dst.z), SOURCE_DIRECTION, _link)
	_require(destination_q_in_source.distance_to(removed_q) <= MOMENTUM_TOLERANCE,
		"handoff physical momentum mismatch removed=%s destination=%s" % [
			str(removed_q), str(destination_q_in_source)])

	_handoff_destination_mass = _slot_depth_sum(state, 1)
	_require(_handoff_destination_mass > 0.0,
		"destination has no seeded water after handoff")
	if _finished:
		return
	_start_connected_solver()


func _start_connected_solver() -> void:
	_stage = 4
	_solver = SparseHydroStepGPU.new()
	_solver.manning_n = 0.0
	add_child(_solver)
	_solver.initialized.connect(func():
		if _solver.advance(SOLVER_DT, 8, true) < 0:
			_fail("post-handoff sparse advance rejected"))
	_solver.initialization_failed.connect(func(error: Error):
		_fail("post-handoff solver initialization failed (%d)" % int(error)))
	_solver.diagnostics_ready.connect(_on_solver_diagnostics)
	var err := _solver.initialize(_atlas, _connectivity)
	if err != OK:
		_fail("post-handoff solver initialize rejected (%d)" % int(err))


func _on_solver_diagnostics(_step_id: int, d: Dictionary) -> void:
	_require(int(d["post_invalid_cells"]) == 0,
		"post-handoff solver produced invalid cells")
	_require(not bool(d["cfl_clamped"]),
		"post-handoff solver unexpectedly hit CFL cap: %s" % str(d))
	if _finished:
		return
	_stage = 5
	_request_state(_on_final_state)


func _on_final_state(state: PackedFloat32Array) -> void:
	var total_mass := _sum_depth(state)
	_require(absf(total_mass - _initial_mass) <= MASS_TOLERANCE,
		"expanded connected solver changed mass delta=%.9g" % (total_mass - _initial_mass))
	var destination_mass := _slot_depth_sum(state, 1)
	_require(destination_mass > _handoff_destination_mass,
		"newly active destination did not continue receiving water: seed=%.9g final=%.9g" % [
			_handoff_destination_mass, destination_mass])
	if _finished:
		return
	_finished = true
	print("SPARSE_HYDRO_FRONTIER_HANDOFF: PASS seed_mass=",
		_handoff_destination_mass, " final_destination_mass=", destination_mass,
		" total_mass_delta=", total_mass - _initial_mass)
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
	var cells := TILE_RES * TILE_RES
	for i in cells:
		var o := (slot * cells + i) * 4
		state[o] = depth
		state[o + 1] = momentum.x
		state[o + 2] = momentum.y
		state[o + 3] = bed


func _sum_depth(state: PackedFloat32Array) -> float:
	var total := 0.0
	for i in CAPACITY * TILE_RES * TILE_RES:
		total += float(state[i * 4])
	return total


func _slot_depth_sum(state: PackedFloat32Array, slot: int) -> float:
	var total := 0.0
	var base := slot * TILE_RES * TILE_RES
	for i in TILE_RES * TILE_RES:
		total += float(state[(base + i) * 4])
	return total


func _require(condition: bool, message: String) -> void:
	if condition or _finished:
		return
	_fail(message)


func _fail(message: String) -> void:
	if _finished:
		return
	_finished = true
	push_error("SPARSE_HYDRO_FRONTIER_HANDOFF: " + message)
	_cleanup()
	get_tree().quit(1)


func _cleanup() -> void:
	if _readback != null and is_instance_valid(_readback):
		_readback.queue_free()
	if _solver != null and is_instance_valid(_solver):
		_solver.release()
		_solver.queue_free()
	if _activation != null and is_instance_valid(_activation):
		_activation.release()
		_activation.queue_free()
	if _frontier != null and is_instance_valid(_frontier):
		_frontier.release()
		_frontier.queue_free()
	if _activity != null and is_instance_valid(_activity):
		_activity.release()
		_activity.queue_free()
	if _terrain_bed != null and is_instance_valid(_terrain_bed):
		_terrain_bed.release()
		_terrain_bed.queue_free()
	if _connectivity != null and is_instance_valid(_connectivity):
		_connectivity.release()
		_connectivity.queue_free()
	if _identity_bridge != null and is_instance_valid(_identity_bridge):
		_identity_bridge.unbind()
		_identity_bridge.queue_free()
	if _atlas != null and is_instance_valid(_atlas):
		_atlas.release()
		_atlas.queue_free()
	_macro = null
