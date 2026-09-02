extends Node
## Renderer-mode gate for the one-shot coarse -> sparse exact-volume seed.

const R := 16
const DX := 4.0
const REQUESTED_VOLUME_M3 := 123.456
const TIMEOUT_FRAMES := 1200

var _atlas: SparseHydroAtlasGPU
var _seeder: HydroCoarseSeedGPU
var _diag_a: HydroVolumeDiagnosticsGPU
var _diag_b: HydroVolumeDiagnosticsGPU
var _plan: Dictionary = {}
var _frames := 0
var _ready_count := 0
var _request_a := -1
var _request_b := -1
var _volume_a := NAN
var _volume_b := NAN


func _ready() -> void:
	if RenderingServer.get_rendering_device() == null:
		print("HYDRO_COARSE_SEED_GPU: SKIP (no global RenderingDevice)")
		get_tree().quit(0)
		return
	_atlas = SparseHydroAtlasGPU.new()
	add_child(_atlas)
	_atlas.initialized.connect(_on_atlas_initialized)
	_atlas.initialization_failed.connect(func(error: Error): _fail("atlas init %d" % int(error)))
	var err := _atlas.initialize(1, R, DX)
	if err != OK:
		_fail("atlas submit %d" % int(err))


func _process(_delta: float) -> void:
	_frames += 1
	if _frames > TIMEOUT_FRAMES:
		_fail("timeout")


func _on_atlas_initialized() -> void:
	_seeder = HydroCoarseSeedGPU.new()
	add_child(_seeder)
	_seeder.initialized.connect(_on_seeder_initialized)
	_seeder.initialization_failed.connect(func(error: Error): _fail("seeder init %d" % int(error)))
	_seeder.seed_recorded.connect(_on_seed_recorded)
	_seeder.seed_failed.connect(func(_id: int, error: Error): _fail("seed %d" % int(error)))
	var err := _seeder.initialize(_atlas)
	if err != OK:
		_fail("seeder submit %d" % int(err))


func _on_seeder_initialized() -> void:
	_plan = _seeder.plan_volume(REQUESTED_VOLUME_M3)
	if int(_plan.get("error", FAILED)) != OK:
		_fail("seed planning failed")
		return
	var depth := float(_plan.get("depth_m", 0.0))
	var request := _seeder.seed_reserved(0, depth, Vector2(1.25, -0.5))
	if request < 0:
		_fail("seed submit rejected")


func _on_seed_recorded(_request_id: int, slot: int, represented_volume_m3: float) -> void:
	if slot != 0:
		_fail("seed acknowledged wrong slot")
		return
	var expected := float(_plan.get("represented_volume_m3", -1.0))
	if not _close(represented_volume_m3, expected, 1.0e-9, 1.0e-7):
		_fail("seed acknowledgement volume mismatch")
		return
	_diag_a = HydroVolumeDiagnosticsGPU.new()
	_diag_b = HydroVolumeDiagnosticsGPU.new()
	add_child(_diag_a)
	add_child(_diag_b)
	_diag_a.initialized.connect(_on_diag_initialized)
	_diag_b.initialized.connect(_on_diag_initialized)
	_diag_a.initialization_failed.connect(func(error: Error): _fail("diag A init %d" % int(error)))
	_diag_b.initialization_failed.connect(func(error: Error): _fail("diag B init %d" % int(error)))
	_diag_a.volume_ready.connect(_on_volume_a)
	_diag_b.volume_ready.connect(_on_volume_b)
	_diag_a.readback_failed.connect(func(_id: int, error: Error): _fail("diag A readback %d" % int(error)))
	_diag_b.readback_failed.connect(func(_id: int, error: Error): _fail("diag B readback %d" % int(error)))
	var err_a := _diag_a.initialize(_atlas.state_a_rid(), R, R, DX)
	var err_b := _diag_b.initialize(_atlas.state_b_rid(), R, R, DX)
	if err_a != OK or err_b != OK:
		_fail("diagnostic submit A=%d B=%d" % [int(err_a), int(err_b)])


func _on_diag_initialized() -> void:
	_ready_count += 1
	if _ready_count != 2:
		return
	_request_a = _diag_a.request_volume()
	_request_b = _diag_b.request_volume()
	if _request_a < 0 or _request_b < 0:
		_fail("volume request rejected")


func _on_volume_a(request_id: int, volume_m3: float) -> void:
	if request_id != _request_a:
		return
	_volume_a = volume_m3
	_maybe_finish()


func _on_volume_b(request_id: int, volume_m3: float) -> void:
	if request_id != _request_b:
		return
	_volume_b = volume_m3
	_maybe_finish()


func _maybe_finish() -> void:
	if is_nan(_volume_a) or is_nan(_volume_b):
		return
	var expected := float(_plan.get("represented_volume_m3", -1.0))
	var abs_tol := maxf(0.002, absf(expected) * 2.0e-5)
	if not _close(_volume_a, expected, abs_tol, 2.0e-5):
		_fail("state A volume %.9f != %.9f" % [_volume_a, expected])
		return
	if not _close(_volume_b, expected, abs_tol, 2.0e-5):
		_fail("state B volume %.9f != %.9f" % [_volume_b, expected])
		return
	if not _close(_volume_a, _volume_b, abs_tol, 2.0e-5):
		_fail("ping-pong seed volumes differ")
		return
	print("HYDRO_COARSE_SEED_GPU: PASS volume=%.9f m3 depth=%.9f m" % [
		expected, float(_plan.get("depth_m", 0.0))])
	get_tree().quit(0)


func _close(a: float, b: float, abs_tol: float, rel_tol: float) -> bool:
	return absf(a - b) <= maxf(abs_tol, maxf(absf(a), absf(b)) * rel_tol)


func _fail(message: String) -> void:
	push_error("HYDRO_COARSE_SEED_GPU: " + message)
	get_tree().quit(1)
