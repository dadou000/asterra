extends Node
## Renderer-mode reachability gate. Uses the normal compact activity/frontier path
## (no direct state binding on HydroFrontierCandidatesGPU) to prove source edge
## free-surface elevation survives GPU summary -> queue -> resolver policy.

const CAPACITY := 2
const TILE_RES := 8
const DX := 1.0
const TOL := 2.0e-4
const TIMEOUT_FRAMES := 1200

var _scheduler: SparseHydroScheduler
var _atlas: SparseHydroAtlasGPU
var _identity: SparseHydroIdentityBridge
var _activity: HydroTileActivityGPU
var _frontier: HydroFrontierCandidatesGPU
var _service: HydroReachabilityService
var _resolver: HydroFrontierResolver
var _source: HydroTileKey
var _terrain_crest := 11.0
var _structure_crest := NAN
var _frames := 0
var _finished := false


func _ready() -> void:
	if RenderingServer.get_rendering_device() == null:
		print("HYDRO_REACHABILITY: SKIP (no global RenderingDevice)")
		get_tree().quit(0)
		return

	_source = HydroTileKey.new(CubeSphere.FACE_PX, 5, 10, 10)
	_scheduler = SparseHydroScheduler.new(CAPACITY)
	_scheduler.wake_flux_threshold_m3s = 0.01
	_require(_scheduler.wake(_source, 0, "reachability_seed") == 0,
		"source did not allocate slot zero")
	if _finished:
		return

	var state := PackedFloat32Array()
	state.resize(CAPACITY * TILE_RES * TILE_RES * 4)
	# Source free surface is exactly eta=12 m everywhere. Slot 1 contains stale
	# impossible state but is unoccupied and must not participate.
	for i in TILE_RES * TILE_RES:
		var o := i * 4
		state[o] = 2.0
		state[o + 3] = 10.0
	for i in TILE_RES * TILE_RES:
		var o := (TILE_RES * TILE_RES + i) * 4
		state[o] = 90.0
		state[o + 1] = 500.0
		state[o + 2] = -700.0
		state[o + 3] = -100.0

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
	_identity = SparseHydroIdentityBridge.new()
	add_child(_identity)
	var err := _identity.bind(_scheduler, _atlas)
	_require(err == OK, "identity bind failed (%d)" % int(err))
	if _finished:
		return

	_activity = HydroTileActivityGPU.new()
	add_child(_activity)
	_activity.initialized.connect(_on_activity_initialized)
	_activity.initialization_failed.connect(func(error: Error):
		_fail("activity initialization failed (%d)" % int(error)))
	_activity.classification_recorded.connect(_on_classification_recorded)
	err = _activity.initialize(_atlas.state_a_rid(), _atlas.occupancy_rid(),
		CAPACITY, TILE_RES, DX)
	if err != OK:
		_fail("activity initialize rejected (%d)" % int(err))


func _on_activity_initialized() -> void:
	_frontier = HydroFrontierCandidatesGPU.new()
	add_child(_frontier)
	_frontier.initialized.connect(_on_frontier_initialized)
	_frontier.initialization_failed.connect(func(error: Error):
		_fail("frontier initialization failed (%d)" % int(error)))
	_frontier.candidates_ready.connect(_on_candidates)
	_frontier.queue_failed.connect(func(_request_id: int, error: Error):
		_fail("frontier queue failed (%d)" % int(error)))
	# Deliberately use the old/runtime call shape: edge eta must come through the
	# compact activity summary, not through the optional direct-state cross-check.
	var err := _frontier.initialize(_activity.summary_rid(), _atlas.tile_metadata_rid(),
		CAPACITY, _scheduler.wake_flux_threshold_m3s)
	if err != OK:
		_fail("frontier initialize rejected (%d)" % int(err))


func _on_frontier_initialized() -> void:
	if _activity.classify(false) < 0:
		_fail("activity classify rejected")


func _on_classification_recorded(_request_id: int) -> void:
	if _frontier.generate(true) < 0:
		_fail("frontier generation rejected")


func _on_candidates(_request_id: int, candidates: Array[Dictionary], overflow: bool) -> void:
	_require(not overflow, "frontier queue overflowed")
	var east: Dictionary = {}
	for candidate in candidates:
		if int(candidate.get("slot", -1)) == 0 \
				and int(candidate.get("direction", -1)) == HydroTileTopology.DIR_EAST:
			east = candidate
			break
	_require(not east.is_empty(), "east predictive frontier candidate missing")
	if _finished:
		return
	_require(absf(float(east.get("source_surface_m", -INF)) - 12.0) <= TOL,
		"GPU frontier head mismatch: %s" % str(east))
	_require(bool(east.get("predictive_wetting", false)),
		"stationary wet edge was not marked predictive")
	if _finished:
		return

	_service = HydroReachabilityService.new()
	var err := _service.initialize(_atlas, Callable(self, &"_terrain_height"),
		Callable(self, &"_structure_height"))
	_require(err == OK, "reachability initialize failed (%d)" % int(err))
	if _finished:
		return
	_service.minimum_overtop_head_m = 0.01
	_resolver = HydroFrontierResolver.new(_scheduler)

	# Terrain crest 11 < eta 12: must reserve destination.
	_terrain_crest = 11.0
	_structure_crest = NAN
	var result := _resolve_one(east)
	_require(bool(result.get("accepted", false)) and bool(result.get("reserved", false)),
		"low terrain did not accept frontier: %s" % str(result))
	var destination := HydroTileKey.unpack(int(result.get("destination_tile_id", -1)))
	_require(destination != null and _scheduler.cancel_reserved(destination, "reachability_test"),
		"could not release accepted test destination")
	if _finished:
		return

	# Terrain itself above the source free surface: must fail closed.
	_terrain_crest = 12.5
	result = _resolve_one(east)
	_require(not bool(result.get("accepted", false)) and not bool(result.get("reachable", true)),
		"high bank incorrectly accepted frontier: %s" % str(result))
	_require(_service.last_evaluation().get("reason", "") == "insufficient_head",
		"high-bank rejection reason mismatch: %s" % str(_service.last_evaluation()))
	if _finished:
		return

	# Low terrain plus a 13 m levee/wall must still block.
	_terrain_crest = 11.0
	_structure_crest = 13.0
	result = _resolve_one(east)
	_require(not bool(result.get("accepted", false)),
		"structure crest did not block frontier")
	var eval := _service.last_evaluation()
	_require(absf(float(eval.get("minimum_crest_m", 0.0)) - 13.0) <= TOL,
		"structure crest was not included in policy: %s" % str(eval))
	if _finished:
		return

	_finished = true
	print("HYDRO_REACHABILITY: PASS eta=12 terrain/structure gates verified")
	_cleanup()
	get_tree().quit(0)


func _resolve_one(candidate: Dictionary) -> Dictionary:
	var results := _resolver.resolve_candidates([candidate],
		Callable(_service, &"can_enter"), true)
	return {} if results.is_empty() else results[0]


func _terrain_height(_direction: Vector3, _level: int) -> float:
	return _terrain_crest


func _structure_height(_direction: Vector3, _source_key: HydroTileKey,
		_destination_key: HydroTileKey, _destination_direction: int,
		_link: Dictionary) -> float:
	return _structure_crest


func _require(condition: bool, message: String) -> void:
	if condition or _finished:
		return
	_fail(message)


func _fail(message: String) -> void:
	if _finished:
		return
	_finished = true
	push_error("HYDRO_REACHABILITY: " + message)
	_cleanup()
	get_tree().quit(1)


func _cleanup() -> void:
	if _frontier != null and is_instance_valid(_frontier):
		_frontier.release()
		_frontier.queue_free()
	if _activity != null and is_instance_valid(_activity):
		_activity.release()
		_activity.queue_free()
	if _identity != null and is_instance_valid(_identity):
		_identity.unbind()
		_identity.queue_free()
	if _atlas != null and is_instance_valid(_atlas):
		_atlas.release()
		_atlas.queue_free()
