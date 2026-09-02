extends Node
## Renderer-mode gate for exact sparse external source accounting.
##
## A uniform shallow tile receives both addition and an oversized sink. With no
## spatial gradients, the requested 0.1 m removal per cell is clipped to the actual
## 0.055 m available after the 0.005 m addition. Diagnostics must report the exact
## gross applied volumes, not the requested sink volume.

const CAPACITY := 1
const TILE_RES := 8
const DX := 1.0
const INITIAL_H := 0.05
const DT := 0.5
const ADD_RATE := 0.01
const REMOVE_RATE := 0.20
const ABS_TOL := 2.0e-5
const TIMEOUT_FRAMES := 900

var _pool: HydroTilePool
var _atlas: SparseHydroAtlasGPU
var _connectivity: SparseHydroConnectivityGPU
var _solver: SparseHydroStepGPU
var _sources: HydroSourceTermsGPU
var _readback: HydroStateReadback
var _frames := 0
var _finished := false


func _ready() -> void:
	if RenderingServer.get_rendering_device() == null:
		print("FINE_EXTERNAL_FLUX_LEDGER: SKIP (no global RenderingDevice)")
		get_tree().quit(0)
		return
	_pool = HydroTilePool.new(CAPACITY)
	var key := HydroTileKey.new(CubeSphere.FACE_PZ, 5, 12, 12)
	_require(_pool.allocate(key, 0) == 0, "tile allocation failed")
	_pool.set_state(key, HydroTilePool.TileState.ACTIVE, "test_bootstrap")
	if _finished:
		return
	var state := PackedFloat32Array()
	state.resize(TILE_RES * TILE_RES * 4)
	for i in TILE_RES * TILE_RES:
		state[i * 4] = INITIAL_H
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
		_fail("timeout")


func _on_atlas_initialized() -> void:
	_require(_atlas.sync_pool(_pool) == OK, "atlas pool sync failed")
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
	_require(_connectivity.sync_pool(_pool) == OK, "connectivity sync failed")
	if _finished:
		return
	_solver = SparseHydroStepGPU.new()
	_solver.manning_n = 0.0
	add_child(_solver)
	_solver.initialized.connect(_on_solver_initialized)
	_solver.initialization_failed.connect(func(error: Error):
		_fail("solver initialization failed (%d)" % int(error)))
	_solver.diagnostics_ready.connect(_on_diagnostics_ready)
	var err := _solver.initialize(_atlas, _connectivity)
	if err != OK:
		_fail("solver initialize rejected (%d)" % int(err))


func _on_solver_initialized() -> void:
	_sources = HydroSourceTermsGPU.new()
	add_child(_sources)
	_sources.initialized.connect(_on_sources_initialized)
	_sources.initialization_failed.connect(func(error: Error):
		_fail("source writer initialization failed (%d)" % int(error)))
	_sources.update_recorded.connect(_on_sources_recorded)
	_sources.update_failed.connect(func(_id: int, error: Error):
		_fail("source update failed (%d)" % int(error)))
	var err := _sources.initialize(_atlas, TILE_RES * TILE_RES)
	if err != OK:
		_fail("source writer initialize rejected (%d)" % int(err))


func _on_sources_initialized() -> void:
	var entries: Array[Dictionary] = []
	for cell in TILE_RES * TILE_RES:
		entries.append({
			"slot": 0,
			"cell": cell,
			"add_depth_rate_mps": ADD_RATE,
			"remove_depth_rate_mps": REMOVE_RATE,
			"hu_rate": 0.0,
			"hv_rate": 0.0,
		})
	if _sources.update_entries(entries) < 0:
		_fail("source update rejected")


func _on_sources_recorded(_request_id: int, _entry_count: int) -> void:
	if _solver.advance(DT, 1, true) < 0:
		_fail("solver advance rejected")


func _on_diagnostics_ready(_step_id: int, d: Dictionary) -> void:
	var cell_count := float(TILE_RES * TILE_RES)
	var cell_area := DX * DX
	var expected_add := ADD_RATE * DT * cell_count * cell_area
	var available_h := INITIAL_H + ADD_RATE * DT
	var expected_remove := minf(REMOVE_RATE * DT, available_h) * cell_count * cell_area
	_require(bool(d.get("external_sink_clipping_exact", false)),
		"diagnostics did not mark sink clipping exact")
	_require_close(float(d.get("external_added_m3", -1.0)), expected_add,
		"gross added volume")
	_require_close(float(d.get("external_removed_m3", -1.0)), expected_remove,
		"clipped removed volume")
	_require_close(float(d.get("external_net_m3", 999.0)), expected_add - expected_remove,
		"net external volume")
	_require(float(d.get("external_removed_m3", 0.0)) \
		< REMOVE_RATE * DT * cell_count * cell_area - 1.0,
		"ledger appears to report requested rather than clipped sink volume")
	if _finished:
		return
	_readback = HydroStateReadback.new()
	add_child(_readback)
	_readback.state_ready.connect(_on_state_ready)
	_readback.readback_failed.connect(func(_id: int, error: Error):
		_fail("state readback failed (%d)" % int(error)))
	if _readback.request_state(_atlas.state_a_rid(), _atlas.total_cell_count()) < 0:
		_fail("state readback rejected")


func _on_state_ready(_request_id: int, state: PackedFloat32Array) -> void:
	var total_h := 0.0
	for i in TILE_RES * TILE_RES:
		total_h += maxf(float(state[i * 4]), 0.0)
	_require_close(total_h * DX * DX, 0.0, "final water volume")
	if _finished:
		return
	_finished = true
	print("FINE_EXTERNAL_FLUX_LEDGER: PASS diagnostics=", _solver.latest_diagnostics())
	_cleanup()
	get_tree().quit(0)


func _require_close(value: float, reference: float, label: String) -> void:
	if absf(value - reference) > ABS_TOL:
		_fail("%s got %.9g expected %.9g" % [label, value, reference])


func _require(condition: bool, message: String) -> void:
	if not condition and not _finished:
		_fail(message)


func _fail(message: String) -> void:
	if _finished:
		return
	_finished = true
	push_error("FINE_EXTERNAL_FLUX_LEDGER: " + message)
	_cleanup()
	get_tree().quit(1)


func _cleanup() -> void:
	if _readback != null and is_instance_valid(_readback):
		_readback.queue_free()
	if _sources != null and is_instance_valid(_sources):
		_sources.release()
		_sources.queue_free()
	if _solver != null and is_instance_valid(_solver):
		_solver.release()
		_solver.queue_free()
	if _connectivity != null and is_instance_valid(_connectivity):
		_connectivity.release()
		_connectivity.queue_free()
	if _atlas != null and is_instance_valid(_atlas):
		_atlas.release()
		_atlas.queue_free()
