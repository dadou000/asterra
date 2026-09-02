extends Node
## Renderer-mode Phase 3 smoke test for sparse slot storage, compact GPU
## summaries, stable slot metadata and the summary -> active-frontier queue.

const CAPACITY := 4
const TILE_RES := 8
const DX := 2.0
const FRONTIER_THRESHOLD_M3S := 0.5
const TIMEOUT_FRAMES := 1200

var _atlas: SparseHydroAtlasGPU
var _activity: HydroTileActivityGPU
var _frontier: HydroFrontierCandidatesGPU
var _frames := 0
var _finished := false
var _keys: Array[HydroTileKey] = []


func _ready() -> void:
	if RenderingServer.get_rendering_device() == null:
		print("SPARSE_HYDRO_GPU: SKIP (no global RenderingDevice)")
		get_tree().quit(0)
		return

	var state := PackedFloat32Array()
	state.resize(CAPACITY * TILE_RES * TILE_RES * 4)
	_fill_uniform_tile(state, 0, 2.0, Vector2(0.5, 0.0), -2.0)
	# Slot 1 intentionally contains water but is unoccupied; summary/frontier must
	# ignore stale data and stale metadata from a recycled GPU slot.
	_fill_uniform_tile(state, 1, 5.0, Vector2(3.0, 2.0), -5.0)
	_fill_uniform_tile(state, 2, 1.0, Vector2(0.0, -0.25), -1.0)
	_fill_uniform_tile(state, 3, 0.0, Vector2.ZERO, 0.0)
	var occupancy := PackedInt32Array([1, 0, 1, 1])

	_keys = [
		HydroTileKey.new(CubeSphere.FACE_PX, 6, 63, 23),
		HydroTileKey.new(CubeSphere.FACE_NX, 5, 7, 9), # intentionally inactive
		HydroTileKey.new(CubeSphere.FACE_PZ, 7, 41, 0),
		HydroTileKey.new(CubeSphere.FACE_PY, 4, 3, 6),
	]
	var metadata := PackedInt32Array()
	metadata.resize(CAPACITY * 4)
	for slot in CAPACITY:
		var key := _keys[slot]
		var o := slot * 4
		metadata[o] = key.face
		metadata[o + 1] = key.level
		metadata[o + 2] = key.x
		metadata[o + 3] = key.y

	_atlas = SparseHydroAtlasGPU.new()
	add_child(_atlas)
	_atlas.initialized.connect(_on_atlas_initialized)
	_atlas.initialization_failed.connect(func(error: Error):
		_fail("atlas initialization failed (%d)" % int(error)))
	var err := _atlas.initialize(CAPACITY, TILE_RES, DX, state, occupancy, metadata)
	if err != OK:
		_fail("atlas initialize rejected (%d)" % int(err))


func _process(_delta: float) -> void:
	if _finished:
		return
	_frames += 1
	if _frames > TIMEOUT_FRAMES:
		_fail("timed out")


func _on_atlas_initialized() -> void:
	_activity = HydroTileActivityGPU.new()
	add_child(_activity)
	_activity.initialized.connect(_on_activity_initialized)
	_activity.initialization_failed.connect(func(error: Error):
		_fail("activity pipeline initialization failed (%d)" % int(error)))
	_activity.summaries_ready.connect(_on_summaries_ready)
	_activity.classification_failed.connect(func(_request_id: int, error: Error):
		_fail("activity classification failed (%d)" % int(error)))
	var err := _activity.initialize(_atlas.state_a_rid(), _atlas.occupancy_rid(),
		CAPACITY, TILE_RES, DX)
	if err != OK:
		_fail("activity initialize rejected (%d)" % int(err))


func _on_activity_initialized() -> void:
	if _activity.classify(true) < 0:
		_fail("classification request rejected")


func _on_summaries_ready(_request_id: int, summaries: Array[Dictionary]) -> void:
	if summaries.size() != CAPACITY:
		_fail("summary count mismatch")
		return

	var s0 := summaries[0]
	_require(bool(s0["active"]), "slot 0 should be active")
	_require_close(float(s0["max_depth_m"]), 2.0, 1.0e-5, "slot0 depth")
	_require_close(float(s0["max_velocity_mps"]), 0.5, 1.0e-5, "slot0 velocity")
	_require_close(float(s0["kinetic_energy_proxy"]), 64.0, 2.0e-4, "slot0 energy")
	_require_close(float(s0["flux_east_m3s"]), 16.0, 2.0e-4, "slot0 east flux")
	_require_close(float(s0["flux_west_m3s"]), 0.0, 1.0e-6, "slot0 west flux")
	_require(int(s0["wet_cells"]) == TILE_RES * TILE_RES, "slot0 wet count")
	_require(int(s0["invalid_cells"]) == 0, "slot0 invalid cells")

	var s1 := summaries[1]
	_require(not bool(s1["active"]), "unoccupied slot 1 reported active")
	_require_close(float(s1["max_depth_m"]), 0.0, 1.0e-6, "inactive slot depth")
	_require(int(s1["wet_cells"]) == 0, "inactive slot wet count")

	var s2 := summaries[2]
	_require(bool(s2["active"]), "slot 2 should be active")
	_require_close(float(s2["max_depth_m"]), 1.0, 1.0e-5, "slot2 depth")
	_require_close(float(s2["max_velocity_mps"]), 0.25, 1.0e-5, "slot2 velocity")
	_require_close(float(s2["flux_south_m3s"]), 4.0, 2.0e-4, "slot2 south flux")
	_require_close(float(s2["flux_north_m3s"]), 0.0, 1.0e-6, "slot2 north flux")

	var s3 := summaries[3]
	_require(bool(s3["active"]), "occupied dry slot should still report active ownership")
	_require(int(s3["wet_cells"]) == 0, "dry slot wet count")
	_require_close(float(s3["max_depth_m"]), 0.0, 1.0e-6, "dry slot depth")
	if _finished:
		return

	# The frontier stage consumes GPU summary + metadata RIDs directly.
	_frontier = HydroFrontierCandidatesGPU.new()
	add_child(_frontier)
	_frontier.initialized.connect(_on_frontier_initialized)
	_frontier.initialization_failed.connect(func(error: Error):
		_fail("frontier pipeline initialization failed (%d)" % int(error)))
	_frontier.candidates_ready.connect(_on_candidates_ready)
	_frontier.queue_failed.connect(func(_request_id: int, error: Error):
		_fail("frontier queue failed (%d)" % int(error)))
	var err := _frontier.initialize(_activity.summary_rid(), _atlas.tile_metadata_rid(),
		CAPACITY, FRONTIER_THRESHOLD_M3S)
	if err != OK:
		_fail("frontier initialize rejected (%d)" % int(err))


func _on_frontier_initialized() -> void:
	if _frontier.generate(true) < 0:
		_fail("frontier generation request rejected")


func _on_candidates_ready(_request_id: int, candidates: Array[Dictionary],
		overflow: bool) -> void:
	_require(not overflow, "frontier queue overflowed")
	_require(candidates.size() == 2,
		"expected exactly 2 frontier candidates, got %d: %s" % [
			candidates.size(), str(candidates)])
	if _finished:
		return

	var found_east := false
	var found_south := false
	for c in candidates:
		var slot := int(c.get("slot", -1))
		var direction := int(c.get("direction", -1))
		var flux := float(c.get("flux_m3s", NAN))
		_require(slot != 1, "inactive stale slot generated a frontier candidate")
		_require(slot != 3, "dry occupied slot generated a frontier candidate")
		var key := _keys[slot]
		_require(int(c.get("face", -1)) == key.face
			and int(c.get("level", -1)) == key.level
			and int(c.get("x", -1)) == key.x
			and int(c.get("y", -1)) == key.y,
			"frontier stable identity mismatch slot=%d candidate=%s expected=%s" % [
				slot, str(c), str(key)])
		if slot == 0 and direction == SparseHydroScheduler.DIR_EAST:
			_require_close(flux, 16.0, 2.0e-4, "frontier slot0 east flux")
			found_east = true
		elif slot == 2 and direction == SparseHydroScheduler.DIR_SOUTH:
			_require_close(flux, 4.0, 2.0e-4, "frontier slot2 south flux")
			found_south = true
		else:
			_fail("unexpected frontier candidate: %s" % str(c))
			return

	_require(found_east, "missing slot0 east frontier candidate")
	_require(found_south, "missing slot2 south frontier candidate")
	if _finished:
		return
	_finished = true
	print("SPARSE_HYDRO_GPU: PASS frontier=", candidates)
	_cleanup()
	get_tree().quit(0)


func _fill_uniform_tile(state: PackedFloat32Array, slot: int, depth: float,
		velocity: Vector2, bed: float) -> void:
	var cells_per_tile := TILE_RES * TILE_RES
	for i in cells_per_tile:
		var o := (slot * cells_per_tile + i) * 4
		state[o] = depth
		state[o + 1] = depth * velocity.x
		state[o + 2] = depth * velocity.y
		state[o + 3] = bed


func _require_close(actual: float, expected: float, tolerance: float, label: String) -> void:
	_require(is_finite(actual) and absf(actual - expected) <= tolerance,
		"%s %.9g != %.9g (tol %.3g)" % [label, actual, expected, tolerance])


func _require(condition: bool, message: String) -> void:
	if condition or _finished:
		return
	_fail(message)


func _fail(message: String) -> void:
	if _finished:
		return
	_finished = true
	push_error("SPARSE_HYDRO_GPU: " + message)
	_cleanup()
	get_tree().quit(1)


func _cleanup() -> void:
	if _frontier != null and is_instance_valid(_frontier):
		_frontier.release()
		_frontier.queue_free()
	if _activity != null and is_instance_valid(_activity):
		_activity.release()
		_activity.queue_free()
	if _atlas != null and is_instance_valid(_atlas):
		_atlas.release()
		_atlas.queue_free()
