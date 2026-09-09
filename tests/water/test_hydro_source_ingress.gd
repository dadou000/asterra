extends Node
## Generic source-ingress gate.
##
## Starts with no resident hydrology tile. Two positive emitters and one sink share
## one world-space point. Source ingress must create the tile through GPU terrain
## staging, aggregate compact source rates, transform injected world velocity into
## the face-local solver frame, then SWE must add/remove mass and momentum exactly.

const CAPACITY := 2
const TILE_RES := 8
const DX := 1.0
const FACE_RES := 16
const LEVEL := 4
const STEP_DT := 0.10
const TOL := 3.0e-4
const TIMEOUT_FRAMES := 1800

var _scheduler: SparseHydroScheduler
var _atlas: SparseHydroAtlasGPU
var _identity: SparseHydroIdentityBridge
var _connectivity: SparseHydroConnectivityGPU
var _terrain: HydroTerrainBedGPU
var _ingress: HydroSourceIngress
var _solver: SparseHydroStepGPU
var _readback: HydroStateReadback
var _macro: Texture2DArray
var _frames := 0
var _stage := 0
var _finished := false


func _ready() -> void:
	if RenderingServer.get_rendering_device() == null:
		print("HYDRO_SOURCE_INGRESS: SKIP (no global RenderingDevice)")
		get_tree().quit(0)
		return

	_macro = _make_macro_texture()
	_require(_macro != null, "synthetic macro texture creation failed")
	if _finished:
		return

	_scheduler = SparseHydroScheduler.new(CAPACITY)
	_scheduler.freeze_wet_tiles = false

	# Atlas slots deliberately contain stale impossible data but remain unoccupied.
	var stale := PackedFloat32Array()
	stale.resize(CAPACITY * TILE_RES * TILE_RES * 4)
	for i in CAPACITY * TILE_RES * TILE_RES:
		var o := i * 4
		stale[o] = 50.0
		stale[o + 1] = 700.0
		stale[o + 2] = -900.0
		stale[o + 3] = -100.0

	_atlas = SparseHydroAtlasGPU.new()
	add_child(_atlas)
	_atlas.initialized.connect(_on_atlas_initialized)
	_atlas.initialization_failed.connect(func(error: Error):
		_fail("atlas initialization failed (%d)" % int(error)))
	var err := _atlas.initialize(CAPACITY, TILE_RES, DX, stale)
	if err != OK:
		_fail("atlas initialize rejected (%d)" % int(err))


func _process(_delta: float) -> void:
	if _finished:
		return
	_frames += 1
	if _frames > TIMEOUT_FRAMES:
		_fail("timed out at stage %d" % _stage)


func _make_macro_texture() -> Texture2DArray:
	var images: Array[Image] = []
	for _face in 6:
		var image := Image.create(FACE_RES, FACE_RES, false, Image.FORMAT_RF)
		image.fill(Color(0.0, 0.0, 0.0, 1.0))
		if image.generate_mipmaps() != OK:
			return null
		images.append(image)
	var texture := Texture2DArray.new()
	return texture if texture.create_from_images(images) == OK else null


func _on_atlas_initialized() -> void:
	_identity = SparseHydroIdentityBridge.new()
	add_child(_identity)
	var err := _identity.bind(_scheduler, _atlas)
	_require(err == OK, "identity bind failed (%d)" % int(err))
	if _finished:
		return

	_connectivity = SparseHydroConnectivityGPU.new()
	add_child(_connectivity)
	_connectivity.initialized.connect(_on_connectivity_initialized)
	_connectivity.initialization_failed.connect(func(error: Error):
		_fail("connectivity initialization failed (%d)" % int(error)))
	err = _connectivity.initialize(CAPACITY)
	if err != OK:
		_fail("connectivity initialize rejected (%d)" % int(err))


func _on_connectivity_initialized() -> void:
	_require(_connectivity.sync_pool(_scheduler.pool) == OK,
		"initial connectivity sync failed")
	if _finished:
		return

	_terrain = HydroTerrainBedGPU.new()
	add_child(_terrain)
	_terrain.initialized.connect(_on_terrain_initialized)
	_terrain.initialization_failed.connect(func(error: Error):
		_fail("terrain initialization failed (%d)" % int(error)))
	var err := _terrain.initialize(_atlas, _macro, FACE_RES, {
		"planet_radius": 1000.0,
		"base_spacing": 1.0,
		"terrain_level": 0,
		"detail_seed": 7,
		"detail_strength": 0.0,
	})
	if err != OK:
		_fail("terrain initialize rejected (%d)" % int(err))


func _on_terrain_initialized() -> void:
	_ingress = HydroSourceIngress.new()
	add_child(_ingress)
	_ingress.initialized.connect(_on_ingress_initialized)
	_ingress.initialization_failed.connect(func(error: Error):
		_fail("ingress initialization failed (%d)" % int(error)))
	_ingress.flush_completed.connect(_on_flush_completed)
	_ingress.flush_failed.connect(func(_request_id: int, error: Error, stage: String):
		_fail("ingress flush failed stage=%s error=%d" % [stage, int(error)]))
	var err := _ingress.initialize(_scheduler, _atlas, _connectivity, _identity, _terrain)
	if err != OK:
		_fail("ingress initialize rejected (%d)" % int(err))


func _on_ingress_initialized() -> void:
	var d := Vector3.RIGHT # +X face centre: local +u=-Z, local +v=+Y
	var velocity_a := Vector3(0.0, -0.5, -1.5) # local (1.5, -0.5)
	var velocity_b := Vector3(0.0, 0.5, 0.5)   # local (-0.5, 0.5)
	_require(_ingress.upsert_point_source("spring_a", d, 2.0, velocity_a, LEVEL) == OK,
		"spring_a rejected")
	_require(_ingress.upsert_point_source("spring_b", d, 1.0, velocity_b, LEVEL) == OK,
		"spring_b rejected")
	_require(_ingress.upsert_point_source("drain", d, -0.5, Vector3.ZERO, LEVEL) == OK,
		"drain rejected")
	if _finished:
		return
	_stage = 1
	if _ingress.flush() < 0:
		_fail("initial source flush rejected")


func _on_flush_completed(_request_id: int, _source_count: int, entry_count: int) -> void:
	if _stage == 1:
		_require(entry_count == 1, "same-cell sources were not aggregated: %d entries" % entry_count)
		_require(_scheduler.pool.allocated_count() == 1,
			"positive source did not create exactly one tile")
		if _finished:
			return
		_stage = 2
		_request_buffer(_atlas.source_rid(), _on_source_terms)
		return
	if _stage == 5:
		_require(entry_count == 0, "source removal did not clear compact entry set")
		_stage = 6
		_request_buffer(_atlas.source_rid(), _on_sources_cleared)


func _request_buffer(rid: RID, callback: Callable) -> void:
	if _readback != null and is_instance_valid(_readback):
		_readback.queue_free()
	_readback = HydroStateReadback.new()
	add_child(_readback)
	_readback.state_ready.connect(func(_request_id: int, values: PackedFloat32Array):
		callback.call(values))
	_readback.readback_failed.connect(func(_request_id: int, error: Error):
		_fail("buffer readback failed (%d)" % int(error)))
	if _readback.request_state(rid, _atlas.total_cell_count()) < 0:
		_fail("buffer readback rejected")


func _on_source_terms(values: PackedFloat32Array) -> void:
	var nonzero := -1
	for i in _atlas.total_cell_count():
		var o := i * 4
		if absf(values[o]) + absf(values[o + 1]) + absf(values[o + 2]) + absf(values[o + 3]) > TOL:
			_require(nonzero < 0, "more than one source cell was written")
			nonzero = i
	_require(nonzero >= 0, "source buffer contains no active entry")
	if _finished:
		return
	var o := nonzero * 4
	_require(absf(values[o] - 3.0) <= TOL,
		"aggregated add depth rate mismatch %.9g" % values[o])
	_require(absf(values[o + 1] - 0.5) <= TOL,
		"aggregated sink depth rate mismatch %.9g" % values[o + 1])
	_require(absf(values[o + 2] - 2.5) <= 2.0e-3,
		"injected hu rate mismatch %.9g" % values[o + 2])
	_require(absf(values[o + 3] + 0.5) <= 2.0e-3,
		"injected hv rate mismatch %.9g" % values[o + 3])
	if _finished:
		return

	_stage = 3
	_solver = SparseHydroStepGPU.new()
	_solver.manning_n = 0.0
	add_child(_solver)
	_solver.initialized.connect(func():
		if _solver.advance(STEP_DT, 1, true) < 0:
			_fail("source SWE advance rejected"))
	_solver.initialization_failed.connect(func(error: Error):
		_fail("solver initialization failed (%d)" % int(error)))
	_solver.diagnostics_ready.connect(_on_solver_diagnostics)
	var err := _solver.initialize(_atlas, _connectivity)
	if err != OK:
		_fail("solver initialize rejected (%d)" % int(err))


func _on_solver_diagnostics(_step_id: int, diagnostics: Dictionary) -> void:
	_require(int(diagnostics.get("post_invalid_cells", 0)) == 0,
		"source step produced invalid cells")
	_require(absf(float(diagnostics.get("advanced_dt_s", 0.0)) - STEP_DT) <= 1.0e-5,
		"source step did not consume exact dt: %s" % str(diagnostics))
	if _finished:
		return
	_stage = 4
	_request_buffer(_atlas.state_a_rid(), _on_state_after_source)


func _on_state_after_source(values: PackedFloat32Array) -> void:
	var wet_cell := -1
	for i in _atlas.total_cell_count():
		if values[i * 4] > TOL:
			_require(wet_cell < 0, "first source step unexpectedly wetted multiple cells")
			wet_cell = i
	_require(wet_cell >= 0, "source step did not add water")
	if _finished:
		return
	var o := wet_cell * 4
	# add 0.30 m with (hu,hv)=(0.25,-0.05), then remove 0.05 m of the
	# local parcel proportionally -> h=.25, momentum *= .25/.30.
	_require(absf(values[o] - 0.25) <= TOL,
		"source/sink depth mismatch %.9g" % values[o])
	_require(absf(values[o + 1] - (0.25 * 2.5 / 3.0)) <= 3.0e-3,
		"source/sink hu mismatch %.9g" % values[o + 1])
	_require(absf(values[o + 2] - (-0.05 * 2.5 / 3.0)) <= 3.0e-3,
		"source/sink hv mismatch %.9g" % values[o + 2])
	if _finished:
		return

	_require(_ingress.remove_source("spring_a"), "remove spring_a failed")
	_require(_ingress.remove_source("spring_b"), "remove spring_b failed")
	_require(_ingress.remove_source("drain"), "remove drain failed")
	_stage = 5
	if _ingress.flush() < 0:
		_fail("source-clear flush rejected")


func _on_sources_cleared(values: PackedFloat32Array) -> void:
	for value in values:
		_require(absf(value) <= TOL, "source buffer did not clear; value=%.9g" % value)
		if _finished:
			return
	_finished = true
	print("HYDRO_SOURCE_INGRESS: PASS sources created tile, aggregated, injected, cleared")
	_cleanup()
	get_tree().quit(0)


func _require(condition: bool, message: String) -> void:
	if condition or _finished:
		return
	_fail(message)


func _fail(message: String) -> void:
	if _finished:
		return
	_finished = true
	push_error("HYDRO_SOURCE_INGRESS: " + message)
	_cleanup()
	get_tree().quit(1)


func _cleanup() -> void:
	if _readback != null and is_instance_valid(_readback):
		_readback.queue_free()
	if _solver != null and is_instance_valid(_solver):
		_solver.release()
		_solver.queue_free()
	if _ingress != null and is_instance_valid(_ingress):
		_ingress.release()
		_ingress.queue_free()
	if _terrain != null and is_instance_valid(_terrain):
		_terrain.release()
		_terrain.queue_free()
	if _connectivity != null and is_instance_valid(_connectivity):
		_connectivity.release()
		_connectivity.queue_free()
	if _identity != null and is_instance_valid(_identity):
		_identity.unbind()
		_identity.queue_free()
	if _atlas != null and is_instance_valid(_atlas):
		_atlas.release()
		_atlas.queue_free()
	_macro = null
