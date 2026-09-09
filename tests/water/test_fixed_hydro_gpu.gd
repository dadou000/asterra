extends Node
## Requires Forward+/Mobile RenderingDevice. Exercises the fixed-domain GPU
## scheduler: shader creation, reductions, GPU CFL preparation, conditional
## ping-pong substeps, canonicalization and compact async diagnostics.

const W := 16
const H := 16
const TIMEOUT_FRAMES := 360

var _solver: FixedHydroGPU
var _frames := 0
var _front_before := RID()
var _first_id := -1
var _second_id := -1
var _finished := false
var _recorded_ids: Dictionary = {}


func _ready() -> void:
	if RenderingServer.get_rendering_device() == null:
		print("FIXED_HYDRO_GPU: SKIP (no global RenderingDevice)")
		get_tree().quit(0)
		return

	_solver = FixedHydroGPU.new()
	add_child(_solver)
	_solver.initialized.connect(_on_initialized)
	_solver.initialization_failed.connect(_on_initialization_failed)
	_solver.advance_recorded.connect(_on_advance_recorded)
	_solver.diagnostics_ready.connect(_on_diagnostics_ready)

	var state := PackedFloat32Array()
	state.resize(W * H * 4)
	for i in W * H:
		state[i * 4 + 0] = 1.0
		state[i * 4 + 1] = 0.0
		state[i * 4 + 2] = 0.0
		state[i * 4 + 3] = 0.0
	var err := _solver.initialize(W, H, 2.0, state)
	if err != OK:
		_fail("initialize request rejected with error %d" % int(err))


func _process(_delta: float) -> void:
	if _finished:
		return
	_frames += 1
	if _frames > TIMEOUT_FRAMES:
		_fail("timed out waiting for render-thread GPU diagnostics")


func _on_initialized() -> void:
	_front_before = _solver.current_state_rid()
	if not _front_before.is_valid():
		_fail("initial canonical state RID is invalid")
		return

	# h=1 m, dx=2 m gives CFL dt ~0.268 s at cfl=0.42. A 1 s macro
	# advance must therefore select multiple GPU substeps without a CPU readback.
	_first_id = _solver.advance(1.0, 8, true)
	if _first_id < 0:
		_fail("first GPU advance was rejected")


func _on_initialization_failed(error: Error) -> void:
	_fail("pipeline initialization failed with error %d" % int(error))


func _on_advance_recorded(step_id: int) -> void:
	_recorded_ids[step_id] = true
	var front_after := _solver.current_state_rid()
	if not front_after.is_valid():
		_fail("canonical output state RID is invalid")
		return
	if front_after != _front_before:
		_fail("canonical state RID changed after GPU-determined ping-pong sequence")


func _on_diagnostics_ready(step_id: int, d: Dictionary) -> void:
	if _finished:
		return
	if step_id == _first_id:
		if int(d["pre_wet_cells"]) != W * H:
			_fail("pre-step wet-cell reduction is wrong: %s" % d)
			return
		if int(d["pre_invalid_cells"]) != 0 or int(d["post_invalid_cells"]) != 0:
			_fail("invalid state detected in stable uniform-water fixture: %s" % d)
			return
		if int(d["substeps"]) <= 1 or int(d["substeps"]) > 8:
			_fail("GPU CFL scheduler did not choose a valid multi-substep count: %s" % d)
			return
		if bool(d["cfl_clamped"]):
			_fail("ordinary 1 s advance unexpectedly hit substep cap: %s" % d)
			return
		if absf(float(d["advanced_dt_s"]) - 1.0) > 1.0e-4:
			_fail("ordinary advance did not cover requested macro dt: %s" % d)
			return
		call_deferred("_start_clamp_test")
		return

	if step_id == _second_id:
		if not bool(d["cfl_clamped"]):
			_fail("large macro dt should have hit the 2-substep safety cap: %s" % d)
			return
		if int(d["substeps"]) != 2:
			_fail("clamped schedule did not use configured cap: %s" % d)
			return
		if float(d["advanced_dt_s"]) >= float(d["requested_dt_s"]):
			_fail("clamped schedule advanced an unsafe full macro dt: %s" % d)
			return
		if int(d["post_invalid_cells"]) != 0:
			_fail("clamped CFL advance generated invalid cells: %s" % d)
			return
		_finished = true
		print("FIXED_HYDRO_GPU: PASS ", _solver.stats())
		get_tree().quit(0)


func _start_clamp_test() -> void:
	if _finished:
		return
	_second_id = _solver.advance(10.0, 2, true)
	if _second_id < 0:
		_fail("CFL clamp advance was rejected")


func _fail(message: String) -> void:
	if _finished:
		return
	_finished = true
	push_error("FIXED_HYDRO_GPU: " + message)
	get_tree().quit(1)
