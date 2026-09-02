extends Node
## Persistent-runtime gate: one source tile must advance, hit an intentional CFL
## cap, carry its time remainder, generate a frontier candidate, initialize/seed a
## destination, publish connectivity and continue until the queued macro time is
## consumed without test-side orchestration of individual GPU passes.

const CAPACITY := 3
const TILE_RES := 8
const DX := 1.0
const REQUEST_DT := 0.20
const TIMEOUT_FRAMES := 2400
const MASS_TOLERANCE := 5.0e-3

var _scheduler: SparseHydroScheduler
var _atlas: SparseHydroAtlasGPU
var _identity_bridge: SparseHydroIdentityBridge
var _connectivity: SparseHydroConnectivityGPU
var _runtime: SparseHydrologyRuntime
var _readback: HydroStateReadback

var _source: HydroTileKey
var _destination: HydroTileKey
var _source_direction := HydroTileTopology.DIR_EAST
var _initial_mass := 0.0
var _cycles := 0
var _saw_cfl_remainder := false
var _saw_destination_active := false
var _frames := 0
var _finished := false


func _ready() -> void:
	if RenderingServer.get_rendering_device() == null:
		print("SPARSE_HYDRO_RUNTIME: SKIP (no global RenderingDevice)")
		get_tree().quit(0)
		return

	const LEVEL := 5
	_source = HydroTileKey.new(CubeSphere.FACE_PX, LEVEL, 10, 11)
	var link := HydroTileTopology.neighbor(_source, _source_direction)
	_require(not link.is_empty(), "source east neighbor did not resolve")
	if _finished:
		return
	_destination = link["key"] as HydroTileKey

	_scheduler = SparseHydroScheduler.new(CAPACITY)
	_scheduler.wake_flux_threshold_m3s = 0.01
	_scheduler.freeze_wet_tiles = false
	_require(_scheduler.wake(_source, 0, "runtime_seed") == 0,
		"source did not receive slot zero")
	if _finished:
		return

	var state := PackedFloat32Array()
	state.resize(CAPACITY * TILE_RES * TILE_RES * 4)
	_fill_tile(state, 0, 1.0, Vector2(0.30, 0.0), 0.0)
	# Recycled destinations start with intentionally absurd values. The runtime's
	# destination initializer must overwrite the reserved slot before handoff.
	_fill_tile(state, 1, 30.0, Vector2(400.0, -300.0), -20.0)
	_fill_tile(state, 2, 40.0, Vector2(-500.0, 700.0), 80.0)
	_initial_mass = float(TILE_RES * TILE_RES)

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
		_fail("timed out cycles=%d debt=%.9g runtime=%s" % [
			_cycles, 0.0 if _runtime == null else _runtime.time_debt_s(),
			"none" if _runtime == null else _runtime.phase_name()])


func _on_atlas_initialized() -> void:
	_identity_bridge = SparseHydroIdentityBridge.new()
	add_child(_identity_bridge)
	var err := _identity_bridge.bind(_scheduler, _atlas)
	_require(err == OK, "identity bridge bind failed (%d)" % int(err))
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

	_runtime = SparseHydrologyRuntime.new()
	_runtime.name = "SparseHydrologyRuntime"
	_runtime.auto_run = false
	_runtime.macro_dt_s = REQUEST_DT
	_runtime.max_time_debt_s = REQUEST_DT
	_runtime.max_gpu_substeps = 1 # deliberately force CFL remainder
	_runtime.seed_dt_s = 0.02
	_runtime.max_seed_fraction = 0.12
	add_child(_runtime)
	_runtime.initialized.connect(_on_runtime_initialized)
	_runtime.initialization_failed.connect(func(error: Error, component: String):
		_fail("runtime init failed component=%s error=%d" % [component, int(error)]))
	_runtime.runtime_failed.connect(func(error: Error, stage: String):
		_fail("runtime failed stage=%s error=%d" % [stage, int(error)]))
	_runtime.cycle_completed.connect(_on_cycle_completed)

	var err := _runtime.initialize(_scheduler, _atlas, _connectivity,
		_identity_bridge, Callable(self, &"_reachability"),
		Callable(self, &"_destination_state"))
	if err != OK:
		_fail("runtime initialize rejected (%d)" % int(err))


func _on_runtime_initialized() -> void:
	var err := _runtime.advance_time(REQUEST_DT)
	if err != OK:
		_fail("runtime rejected queued time (%d)" % int(err))


func _reachability(source_key: HydroTileKey, direction: int,
		destination_key: HydroTileKey, _flux_m3s: float, _link: Dictionary) -> bool:
	return source_key.equals(_source) and direction == _source_direction \
		and destination_key.equals(_destination)


func _destination_state(destination_key: HydroTileKey,
		destination_slot: int) -> PackedFloat32Array:
	_require(destination_key.equals(_destination), "initializer received wrong destination")
	_require(destination_slot == 1, "destination did not reserve expected slot 1")
	var state := PackedFloat32Array()
	state.resize(TILE_RES * TILE_RES * 4)
	# Flat dry terrain is deliberate here: HydroTerrainBedGPUSmoke separately gates
	# the production terrain reconstruction path; this scene isolates orchestration.
	for i in TILE_RES * TILE_RES:
		state[i * 4 + 3] = 0.0
	return state


func _on_cycle_completed(_cycle_id: int, report: Dictionary) -> void:
	_cycles += 1
	if bool(report.get("cfl_clamped", false)) \
			and float(report.get("remaining_time_debt_s", 0.0)) > 1.0e-5:
		_saw_cfl_remainder = true

	var destination_record := _scheduler.pool.record(_destination)
	if not destination_record.is_empty() \
			and int(destination_record.get("state", -1)) in [
				HydroTilePool.TileState.ACTIVE, HydroTilePool.TileState.SETTLING]:
		_saw_destination_active = true

	if _runtime.time_debt_s() > _runtime.min_cycle_dt_s:
		return

	_require(_cycles >= 2, "CFL-limited request was not split across runtime cycles")
	_require(_saw_cfl_remainder, "runtime never exposed/carried a CFL remainder")
	_require(_saw_destination_active, "runtime never activated the frontier destination")
	if _finished:
		return

	var arrays := SparseHydroConnectivityGPU.build_arrays(_scheduler.pool)
	var neighbor_slots := arrays["neighbor_slots"] as PackedInt32Array
	_require(neighbor_slots[_source_direction] == 1,
		"runtime did not publish source->destination connectivity")
	if _finished:
		return
	_request_state()


func _request_state() -> void:
	_readback = HydroStateReadback.new()
	add_child(_readback)
	_readback.state_ready.connect(_on_state_ready)
	_readback.readback_failed.connect(func(_request_id: int, error: Error):
		_fail("state readback failed (%d)" % int(error)))
	if _readback.request_state(_atlas.state_a_rid(), _atlas.total_cell_count()) < 0:
		_fail("state readback rejected")


func _on_state_ready(_request_id: int, state: PackedFloat32Array) -> void:
	var mass := 0.0
	for slot in CAPACITY:
		# Only occupied/currently owned slots count. Deliberate stale storage in the
		# never-used third slot must not be interpreted as physical water.
		if _scheduler.pool.id_for_slot(slot) < 0:
			continue
		var base := slot * TILE_RES * TILE_RES
		for i in TILE_RES * TILE_RES:
			mass += float(state[(base + i) * 4])
	_require(absf(mass - _initial_mass) <= MASS_TOLERANCE,
		"runtime changed conserved mass delta=%.9g" % (mass - _initial_mass))
	if _finished:
		return
	_finished = true
	print("SPARSE_HYDRO_RUNTIME: PASS cycles=", _cycles,
		" cfl_remainder=", _saw_cfl_remainder,
		" destination_active=", _saw_destination_active,
		" mass_delta=", mass - _initial_mass)
	_cleanup()
	get_tree().quit(0)


func _fill_tile(state: PackedFloat32Array, slot: int, depth: float,
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
	push_error("SPARSE_HYDRO_RUNTIME: " + message)
	_cleanup()
	get_tree().quit(1)


func _cleanup() -> void:
	if _readback != null and is_instance_valid(_readback):
		_readback.queue_free()
	if _runtime != null and is_instance_valid(_runtime):
		_runtime.release()
		_runtime.queue_free()
	if _connectivity != null and is_instance_valid(_connectivity):
		_connectivity.release()
		_connectivity.queue_free()
	if _identity_bridge != null and is_instance_valid(_identity_bridge):
		_identity_bridge.unbind()
		_identity_bridge.queue_free()
	if _atlas != null and is_instance_valid(_atlas):
		_atlas.release()
		_atlas.queue_free()
