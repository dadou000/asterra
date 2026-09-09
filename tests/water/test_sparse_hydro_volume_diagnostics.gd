extends Node
## Renderer-mode gate for SparseHydroVolumeDiagnosticsGPU.
## Verifies that only currently occupied slots contribute to global fine-water
## volume and that changing occupancy immediately changes authoritative volume
## without touching stale state bytes.

const CAPACITY := 3
const TILE_RES := 8
const DX_M := 2.0
const TIMEOUT_FRAMES := 900
const REL_TOL := 8.0e-6
const ABS_TOL_M3 := 2.0e-3

var _atlas: SparseHydroAtlasGPU
var _diag: SparseHydroVolumeDiagnosticsGPU
var _failures: Array[String] = []
var _phase := 0
var _frames := 0


func _ready() -> void:
	if RenderingServer.get_rendering_device() == null:
		print("SPARSE_HYDRO_VOLUME: SKIP (RenderingDevice unavailable)")
		get_tree().quit(0)
		return

	var cells_per_tile := TILE_RES * TILE_RES
	var state := PackedFloat32Array()
	state.resize(CAPACITY * cells_per_tile * SparseHydroAtlasGPU.STATE_FLOATS)
	for slot in CAPACITY:
		var depth := [0.5, 99.0, 1.25][slot]
		for local_i in cells_per_tile:
			var o := (slot * cells_per_tile + local_i) * SparseHydroAtlasGPU.STATE_FLOATS
			state[o] = depth
			state[o + 1] = 0.0
			state[o + 2] = 0.0
			state[o + 3] = 0.0
	var occupancy := PackedInt32Array([1, 0, 1])

	_atlas = SparseHydroAtlasGPU.new()
	_atlas.name = "SparseHydroAtlasGPU"
	add_child(_atlas)
	_atlas.initialized.connect(_on_atlas_initialized)
	_atlas.initialization_failed.connect(func(error: Error):
		_fail("atlas initialization failed (%d)" % int(error)); _finish())
	var err := _atlas.initialize(CAPACITY, TILE_RES, DX_M, state, occupancy)
	if err != OK:
		_fail("atlas initialization submit failed (%d)" % int(err))
		_finish()


func _process(_delta: float) -> void:
	_frames += 1
	if _frames > TIMEOUT_FRAMES:
		_fail("timeout in phase %d" % _phase)
		_finish()


func _on_atlas_initialized() -> void:
	_diag = SparseHydroVolumeDiagnosticsGPU.new()
	_diag.name = "SparseHydroVolumeDiagnosticsGPU"
	add_child(_diag)
	_diag.initialized.connect(_on_diag_initialized)
	_diag.initialization_failed.connect(func(error: Error):
		_fail("diagnostic initialization failed (%d)" % int(error)); _finish())
	_diag.volume_ready.connect(_on_volume_ready)
	_diag.readback_failed.connect(func(_request_id: int, error: Error):
		_fail("diagnostic readback failed (%d)" % int(error)); _finish())
	var err := _diag.initialize_from_atlas(_atlas)
	if err != OK:
		_fail("diagnostic initialization submit failed (%d)" % int(err))
		_finish()


func _on_diag_initialized() -> void:
	_phase = 1
	if _diag.request_volume() < 0:
		_fail("first volume request rejected")
		_finish()


func _on_volume_ready(_request_id: int, volume_m3: float) -> void:
	var tile_area := float(TILE_RES * TILE_RES) * DX_M * DX_M
	if _phase == 1:
		var expected := (0.5 + 1.25) * tile_area
		_expect_close(volume_m3, expected,
			"occupied slots 0+2 volume")
		# Slot 1 contains deliberately enormous stale water. Making it the only
		# occupied slot must switch the authoritative result to that slot alone.
		var err := _atlas.set_occupancy(PackedInt32Array([0, 1, 0]))
		if err != OK:
			_fail("occupancy update 2 failed (%d)" % int(err)); _finish(); return
		_phase = 2
		if _diag.request_volume() < 0:
			_fail("second volume request rejected"); _finish()
		return

	if _phase == 2:
		var expected := 99.0 * tile_area
		_expect_close(volume_m3, expected,
			"single occupied stale-state slot volume")
		var err := _atlas.set_occupancy(PackedInt32Array([0, 0, 0]))
		if err != OK:
			_fail("occupancy update 3 failed (%d)" % int(err)); _finish(); return
		_phase = 3
		if _diag.request_volume() < 0:
			_fail("third volume request rejected"); _finish()
		return

	if _phase == 3:
		_expect_abs(volume_m3, 0.0, ABS_TOL_M3,
			"zero occupancy did not produce zero authoritative fine volume")
		_finish()


func _expect_close(value: float, reference: float, label: String) -> void:
	var abs_error := absf(value - reference)
	var rel_error := abs_error / maxf(absf(reference), 1.0)
	if abs_error > ABS_TOL_M3 and rel_error > REL_TOL:
		_fail("%s: got %.9g expected %.9g (abs %.6g rel %.6g)" % [
			label, value, reference, abs_error, rel_error])


func _expect_abs(value: float, reference: float, tolerance: float,
		label: String) -> void:
	var error := absf(value - reference)
	if error > tolerance:
		_fail("%s: got %.9g expected %.9g (abs %.6g > %.6g)" % [
			label, value, reference, error, tolerance])


func _fail(message: String) -> void:
	if not _failures.has(message):
		_failures.append(message)


func _finish() -> void:
	if _phase == 99:
		return
	_phase = 99
	# Free diagnostic uniform sets before the externally-owned atlas buffers.
	if _diag != null and is_instance_valid(_diag):
		_diag.release()
	if _atlas != null and is_instance_valid(_atlas):
		_atlas.release()
	if _failures.is_empty():
		print("SPARSE_HYDRO_VOLUME: PASS")
		get_tree().quit(0)
	else:
		for failure in _failures:
			push_error("SPARSE_HYDRO_VOLUME: " + failure)
		get_tree().quit(1)
