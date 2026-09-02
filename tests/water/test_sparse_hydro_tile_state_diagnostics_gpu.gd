extends Node
## Renderer-mode numerical gate for SparseHydroTileStateDiagnosticsGPU.

const TILE_RES := 8
const DX := 2.0
const VELOCITY := Vector2(0.60, -0.20)
const ABS_TOL := 2.0e-4
const TIMEOUT_FRAMES := 1200

var _atlas: SparseHydroAtlasGPU
var _scheduler: SparseHydroScheduler
var _identity: SparseHydroIdentityBridge
var _diag: SparseHydroTileStateDiagnosticsGPU
var _key := HydroTileKey.new(CubeSphere.FACE_PX, 3, 2, 3)
var _expected_volume := 0.0
var _expected_max_depth := 0.0
var _frames := 0
var _finished := false


func _ready() -> void:
	if RenderingServer.get_rendering_device() == null:
		print("SPARSE_HYDRO_TILE_STATE_DIAGNOSTICS: SKIP (RenderingDevice unavailable)")
		get_tree().quit(0)
		return
	var state := PackedFloat32Array()
	state.resize(TILE_RES * TILE_RES * 4)
	for i in TILE_RES * TILE_RES:
		var h := 0.10 + float(i) * 0.002
		state[i * 4] = h
		state[i * 4 + 1] = h * VELOCITY.x
		state[i * 4 + 2] = h * VELOCITY.y
		state[i * 4 + 3] = 100.0
		_expected_volume += h * DX * DX
		_expected_max_depth = maxf(_expected_max_depth, h)

	_scheduler = SparseHydroScheduler.new(1)
	_atlas = SparseHydroAtlasGPU.new()
	add_child(_atlas)
	_atlas.initialized.connect(_on_atlas_initialized)
	_atlas.initialization_failed.connect(func(error: Error):
		_fail("atlas initialization failed (%d)" % int(error)))
	var err := _atlas.initialize(1, TILE_RES, DX, state)
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
	if _identity.bind(_scheduler, _atlas) != OK:
		_fail("identity bridge bind failed")
		return
	if _scheduler.wake(_key, 0, "tile_state_diagnostic") != 0:
		_fail("test tile did not receive slot zero")
		return
	_diag = SparseHydroTileStateDiagnosticsGPU.new()
	add_child(_diag)
	_diag.initialized.connect(_on_diag_initialized)
	_diag.initialization_failed.connect(func(error: Error):
		_fail("diagnostic initialization failed (%d)" % int(error)))
	_diag.state_ready.connect(_on_state_ready)
	_diag.readback_failed.connect(func(_id: int, _slot: int, error: Error):
		_fail("diagnostic readback failed (%d)" % int(error)))
	var err := _diag.initialize_from_atlas(_atlas)
	if err != OK:
		_fail("diagnostic initialize rejected (%d)" % int(err))


func _on_diag_initialized() -> void:
	if _diag.request_state(0) < 0:
		_fail("state request rejected")


func _on_state_ready(_request_id: int, slot: int, state: Dictionary) -> void:
	if slot != 0:
		_fail("diagnostic returned wrong slot")
		return
	var volume := float(state.get("volume_m3", -1.0))
	var momentum_u := float(state.get("momentum_u_integral_m4s", NAN))
	var momentum_v := float(state.get("momentum_v_integral_m4s", NAN))
	var mean_velocity: Vector2 = state.get("mean_velocity_local_mps", Vector2.ZERO)
	var max_depth := float(state.get("max_depth_m", -1.0))
	_expect_close(volume, _expected_volume, ABS_TOL, "volume")
	_expect_close(momentum_u, _expected_volume * VELOCITY.x,
		ABS_TOL, "integrated hu")
	_expect_close(momentum_v, _expected_volume * VELOCITY.y,
		ABS_TOL, "integrated hv")
	_expect_close(mean_velocity.x, VELOCITY.x, 1.0e-5, "mean velocity u")
	_expect_close(mean_velocity.y, VELOCITY.y, 1.0e-5, "mean velocity v")
	_expect_close(max_depth, _expected_max_depth, 1.0e-5, "max depth")
	if _finished:
		return
	_finished = true
	print("SPARSE_HYDRO_TILE_STATE_DIAGNOSTICS: PASS volume=", volume,
		" mean_velocity=", mean_velocity)
	_cleanup()
	get_tree().quit(0)


func _expect_close(value: float, expected: float, tolerance: float,
		label: String) -> void:
	if _finished:
		return
	if not is_finite(value) or absf(value - expected) > tolerance:
		_fail("%s mismatch got %.9g expected %.9g tol %.3g" % [
			label, value, expected, tolerance])


func _fail(message: String) -> void:
	if _finished:
		return
	_finished = true
	push_error("SPARSE_HYDRO_TILE_STATE_DIAGNOSTICS: " + message)
	_cleanup()
	get_tree().quit(1)


func _cleanup() -> void:
	if _diag != null and is_instance_valid(_diag):
		_diag.release()
	if _identity != null and is_instance_valid(_identity):
		_identity.unbind()
	if _atlas != null and is_instance_valid(_atlas):
		_atlas.release()
