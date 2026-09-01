extends Node
## Renderer-mode numerical parity harness for FixedHydroGPU.
## Runs several macro advances for lake-at-rest, wet/dry dam break and rainfall,
## then compares the canonical GPU h/hu/hv state cell-by-cell with the CPU oracle.

const TIMEOUT_FRAMES := 2400
const MAX_SUBSTEPS := 32

var _solver: FixedHydroGPU
var _readback: HydroStateReadback
var _volume: HydroVolumeDiagnosticsGPU
var _reference: HydroReferenceSolver
var _cfg: Dictionary
var _fixture_index := -1
var _macro_index := 0
var _frames := 0
var _finished := false
var _volume_phase := 0 # 1=initial, 2=final
var _final_state := PackedFloat32Array()
var _final_volume := 0.0
var _have_state := false
var _have_volume := false


func _ready() -> void:
	if RenderingServer.get_rendering_device() == null:
		print("HYDRO_GPU_PARITY: SKIP (no global RenderingDevice)")
		get_tree().quit(0)
		return
	_start_next_fixture()


func _process(_delta: float) -> void:
	if _finished:
		return
	_frames += 1
	if _frames > TIMEOUT_FRAMES:
		_fail("timed out")


func _start_next_fixture() -> void:
	_cleanup_fixture()
	_fixture_index += 1
	if _fixture_index >= 3:
		_finished = true
		print("HYDRO_GPU_PARITY: PASS")
		get_tree().quit(0)
		return

	_cfg = _build_fixture(_fixture_index)
	var ref_value: Variant = _cfg["reference"]
	_reference = ref_value as HydroReferenceSolver
	if _reference == null:
		_fail("fixture did not provide a HydroReferenceSolver")
		return
	_macro_index = 0
	_have_state = false
	_have_volume = false
	_final_state = PackedFloat32Array()
	_final_volume = 0.0

	_solver = FixedHydroGPU.new()
	_solver.name = "FixedHydroGPU_%s" % String(_cfg["name"])
	add_child(_solver)
	_solver.initialized.connect(_on_solver_initialized)
	_solver.initialization_failed.connect(_on_solver_init_failed)
	_solver.advance_recorded.connect(_on_advance_recorded)

	_readback = HydroStateReadback.new()
	add_child(_readback)
	_readback.state_ready.connect(_on_state_ready)
	_readback.readback_failed.connect(_on_state_readback_failed)

	_volume = HydroVolumeDiagnosticsGPU.new()
	add_child(_volume)
	_volume.initialized.connect(_on_volume_initialized)
	_volume.initialization_failed.connect(_on_volume_init_failed)
	_volume.volume_ready.connect(_on_volume_ready)
	_volume.readback_failed.connect(_on_volume_readback_failed)

	var state: PackedFloat32Array = _cfg["state"]
	var sources: PackedFloat32Array = _cfg["sources"]
	var err := _solver.initialize(
		int(_cfg["w"]), int(_cfg["h"]), float(_cfg["dx"]), state, sources)
	if err != OK:
		_fail("%s: solver initialize rejected (%d)" % [_cfg["name"], int(err)])


func _on_solver_initialized() -> void:
	var err := _volume.initialize(_solver.current_state_rid(),
		int(_cfg["w"]), int(_cfg["h"]), float(_cfg["dx"]))
	if err != OK:
		_fail("%s: volume diagnostic initialize rejected (%d)" % [_cfg["name"], int(err)])


func _on_solver_init_failed(error: Error) -> void:
	_fail("%s: solver pipeline initialization failed (%d)" % [_cfg["name"], int(error)])


func _on_volume_initialized() -> void:
	_volume_phase = 1
	if _volume.request_volume() < 0:
		_fail("%s: initial GPU volume request rejected" % _cfg["name"])


func _on_volume_init_failed(error: Error) -> void:
	_fail("%s: volume pipeline initialization failed (%d)" % [_cfg["name"], int(error)])


func _on_volume_ready(_request_id: int, volume_m3: float) -> void:
	if _volume_phase == 1:
		var expected := _reference.total_volume_m3()
		if not _volume_close(volume_m3, expected):
			_fail("%s: initial GPU volume %.8f != reference %.8f" % [
				_cfg["name"], volume_m3, expected])
			return
		_volume_phase = 0
		_advance_macro()
		return

	if _volume_phase == 2:
		_final_volume = volume_m3
		_have_volume = true
		_maybe_finish_fixture()


func _advance_macro() -> void:
	_reference.advance(float(_cfg["dt"]))
	var step_id := _solver.advance(float(_cfg["dt"]), MAX_SUBSTEPS, false)
	if step_id < 0:
		_fail("%s: GPU advance rejected at macro %d" % [_cfg["name"], _macro_index])


func _on_advance_recorded(_step_id: int) -> void:
	_macro_index += 1
	if _macro_index < int(_cfg["repeats"]):
		call_deferred("_advance_macro")
		return

	var canonical := _solver.current_state_rid()
	if not canonical.is_valid():
		_fail("%s: canonical state RID invalid" % _cfg["name"])
		return
	if _readback.request_state(canonical, _solver.cell_count()) < 0:
		_fail("%s: state readback request rejected" % _cfg["name"])
		return
	_volume_phase = 2
	if _volume.request_volume() < 0:
		_fail("%s: final volume request rejected" % _cfg["name"])


func _on_state_ready(_request_id: int, state: PackedFloat32Array) -> void:
	_final_state = state
	_have_state = true
	_maybe_finish_fixture()


func _on_state_readback_failed(_request_id: int, error: Error) -> void:
	_fail("%s: state readback failed (%d)" % [_cfg["name"], int(error)])


func _on_volume_readback_failed(_request_id: int, error: Error) -> void:
	_fail("%s: volume readback failed (%d)" % [_cfg["name"], int(error)])


func _maybe_finish_fixture() -> void:
	if not _have_state or not _have_volume:
		return
	var expected_size := int(_cfg["w"]) * int(_cfg["h"]) * 4
	if _final_state.size() != expected_size:
		_fail("%s: state readback size %d != %d" % [
			_cfg["name"], _final_state.size(), expected_size])
		return

	var max_h := 0.0
	var max_hu := 0.0
	var max_hv := 0.0
	var max_bed := 0.0
	var gpu_volume_from_state := 0.0
	var area := float(_cfg["dx"]) * float(_cfg["dx"])
	for i in _reference.h.size():
		var o := i * 4
		max_h = maxf(max_h, absf(_final_state[o] - _reference.h[i]))
		max_hu = maxf(max_hu, absf(_final_state[o + 1] - _reference.hu[i]))
		max_hv = maxf(max_hv, absf(_final_state[o + 2] - _reference.hv[i]))
		max_bed = maxf(max_bed, absf(_final_state[o + 3] - _reference.bed[i]))
		gpu_volume_from_state += maxf(_final_state[o], 0.0) * area

	if max_h > float(_cfg["h_tol"]):
		_fail("%s: depth parity error %.9g > %.9g" % [
			_cfg["name"], max_h, float(_cfg["h_tol"])])
		return
	var max_momentum := maxf(max_hu, max_hv)
	if max_momentum > float(_cfg["momentum_tol"]):
		_fail("%s: momentum parity error %.9g > %.9g" % [
			_cfg["name"], max_momentum, float(_cfg["momentum_tol"])])
		return
	if max_bed > 1.0e-6:
		_fail("%s: bed changed on GPU (max error %.9g)" % [_cfg["name"], max_bed])
		return

	var reference_volume := _reference.total_volume_m3()
	if not _volume_close(_final_volume, reference_volume):
		_fail("%s: GPU reduced volume %.8f != reference %.8f" % [
			_cfg["name"], _final_volume, reference_volume])
		return
	if not _volume_close(_final_volume, gpu_volume_from_state):
		_fail("%s: GPU reduced volume %.8f != readback-derived %.8f" % [
			_cfg["name"], _final_volume, gpu_volume_from_state])
		return

	print("HYDRO_GPU_PARITY: ", _cfg["name"], " PASS h=", max_h,
		" hu=", max_hu, " hv=", max_hv, " volume=", _final_volume)
	call_deferred("_start_next_fixture")


func _volume_close(a: float, b: float) -> bool:
	var tolerance := maxf(float(_cfg.get("volume_abs_tol", 0.002)),
		absf(b) * float(_cfg.get("volume_rel_tol", 4.0e-6)))
	return absf(a - b) <= tolerance


func _build_fixture(which: int) -> Dictionary:
	if which == 0:
		return _build_lake_fixture()
	if which == 1:
		return _build_dam_fixture()
	return _build_rain_fixture()


func _build_lake_fixture() -> Dictionary:
	var w := 20
	var hgt := 12
	var dx := 2.0
	var ref := HydroReferenceSolver.new(w, hgt, dx)
	var state := PackedFloat32Array()
	state.resize(w * hgt * 4)
	var sources := PackedFloat32Array()
	sources.resize(w * hgt * 4)
	var eta := 2.6
	for y in hgt:
		for x in w:
			var z := 0.28 * sin(float(x) * 0.41) + 0.19 * cos(float(y) * 0.53)
			var depth := eta - z
			ref.set_bed(x, y, z)
			ref.set_state(x, y, depth)
			_pack_state(state, w, x, y, depth, Vector2.ZERO, z)
	return {
		"name": "lake_at_rest", "w": w, "h": hgt, "dx": dx,
		"dt": 0.35, "repeats": 4, "h_tol": 5.0e-5,
		"momentum_tol": 8.0e-5, "volume_rel_tol": 3.0e-6,
		"reference": ref, "state": state, "sources": sources,
	}


func _build_dam_fixture() -> Dictionary:
	var w := 28
	var hgt := 8
	var dx := 2.0
	var ref := HydroReferenceSolver.new(w, hgt, dx)
	var state := PackedFloat32Array()
	state.resize(w * hgt * 4)
	var sources := PackedFloat32Array()
	sources.resize(w * hgt * 4)
	for y in hgt:
		for x in w:
			var z := 0.04 * sin(float(y) * 0.7)
			var depth := 2.0 if x < (w / 2) else 0.0
			ref.set_bed(x, y, z)
			ref.set_state(x, y, depth)
			_pack_state(state, w, x, y, depth, Vector2.ZERO, z)
	return {
		"name": "wet_dry_dam_break", "w": w, "h": hgt, "dx": dx,
		"dt": 0.28, "repeats": 5, "h_tol": 8.0e-4,
		"momentum_tol": 1.8e-3, "volume_rel_tol": 8.0e-6,
		"reference": ref, "state": state, "sources": sources,
	}


func _build_rain_fixture() -> Dictionary:
	var w := 16
	var hgt := 10
	var dx := 2.0
	var ref := HydroReferenceSolver.new(w, hgt, dx)
	var state := PackedFloat32Array()
	state.resize(w * hgt * 4)
	var sources := PackedFloat32Array()
	sources.resize(w * hgt * 4)
	var rain := 1.5e-4
	var infiltration := 4.0e-5
	for y in hgt:
		for x in w:
			ref.set_bed(x, y, 0.0)
			ref.set_state(x, y, 0.55)
			ref.set_rain_rate(x, y, rain)
			ref.set_infiltration_rate(x, y, infiltration)
			_pack_state(state, w, x, y, 0.55, Vector2.ZERO, 0.0)
			var o := (x + y * w) * 4
			sources[o] = rain
			sources[o + 1] = infiltration
	return {
		"name": "uniform_rain", "w": w, "h": hgt, "dx": dx,
		"dt": 0.5, "repeats": 4, "h_tol": 6.0e-5,
		"momentum_tol": 6.0e-5, "volume_rel_tol": 4.0e-6,
		"reference": ref, "state": state, "sources": sources,
	}


func _pack_state(state: PackedFloat32Array, w: int, x: int, y: int,
		depth: float, velocity: Vector2, bed: float) -> void:
	var o := (x + y * w) * 4
	state[o] = depth
	state[o + 1] = depth * velocity.x
	state[o + 2] = depth * velocity.y
	state[o + 3] = bed


func _cleanup_fixture() -> void:
	for node in [_volume, _solver, _readback]:
		if node != null and is_instance_valid(node):
			if node.has_method(&"release"):
				node.call(&"release")
			node.queue_free()
	_volume = null
	_solver = null
	_readback = null
	_reference = null


func _fail(message: String) -> void:
	if _finished:
		return
	_finished = true
	push_error("HYDRO_GPU_PARITY: " + message)
	_cleanup_fixture()
	get_tree().quit(1)
