extends Node
## Requires Forward+/Mobile RenderingDevice. This is a pipeline smoke test, not a
## numerical parity test: shader import, pipeline creation, SSBOs and one dispatch.

const W := 16
const H := 16
const TIMEOUT_FRAMES := 240

var _solver: FixedHydroGPU
var _frames := 0
var _front_before := RID()
var _finished := false


func _ready() -> void:
	if RenderingServer.get_rendering_device() == null:
		print("FIXED_HYDRO_GPU: SKIP (no global RenderingDevice)")
		get_tree().quit(0)
		return

	_solver = FixedHydroGPU.new()
	add_child(_solver)
	_solver.initialized.connect(_on_initialized)
	_solver.initialization_failed.connect(_on_initialization_failed)
	_solver.step_recorded.connect(_on_step_recorded)

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
		_fail("timed out waiting for render-thread pipeline")


func _on_initialized() -> void:
	_front_before = _solver.current_state_rid()
	if not _front_before.is_valid():
		_fail("initial front state RID is invalid")
		return
	var step_id := _solver.step(0.01)
	if step_id < 0:
		_fail("compute step was rejected")


func _on_initialization_failed(error: Error) -> void:
	_fail("pipeline initialization failed with error %d" % int(error))


func _on_step_recorded(_step_id: int) -> void:
	var front_after := _solver.current_state_rid()
	if not front_after.is_valid():
		_fail("output front state RID is invalid")
		return
	if front_after == _front_before:
		_fail("ping-pong state did not advance")
		return
	_finished = true
	print("FIXED_HYDRO_GPU: PASS ", _solver.stats())
	get_tree().quit(0)


func _fail(message: String) -> void:
	if _finished:
		return
	_finished = true
	push_error("FIXED_HYDRO_GPU: " + message)
	get_tree().quit(1)
