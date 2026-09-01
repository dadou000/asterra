extends Node
## Long-running fixed-domain conservation soak.
##
## Production-style rule: the water grid never comes back to CPU. The only state
## read during the run is the four-byte GPU-reduced volume plus the compact
## scheduler health block at sparse checkpoints.

const W := 64
const H := 64
const DX := 2.0
const MACRO_DT := 0.40
const DEFAULT_ADVANCES := 10000
const SAMPLE_INTERVAL := 250
const MAX_SUBSTEPS := 16
const STALL_FRAME_LIMIT := 1200

# Initial release gate. Tighten from measured hardware data; do not loosen to hide
# a systematic trend. For this domain the relative limit dominates.
const MAX_RELATIVE_VOLUME_DRIFT := 1.0e-4
const MAX_ABSOLUTE_VOLUME_DRIFT_M3 := 0.08

var _solver: FixedHydroGPU
var _volume: HydroVolumeDiagnosticsGPU
var _target_advances := DEFAULT_ADVANCES
var _completed_advances := 0
var _initial_volume := NAN
var _latest_volume := NAN
var _max_abs_drift_m3 := 0.0
var _max_rel_drift := 0.0
var _checkpoint_step_id := -1
var _checkpoint_pending := false
var _volume_pending := false
var _stall_frames := 0
var _finished := false


func _ready() -> void:
	if RenderingServer.get_rendering_device() == null:
		print("HYDRO_GPU_SOAK: SKIP (no global RenderingDevice)")
		get_tree().quit(0)
		return

	_target_advances = _parse_advance_count()
	var fixture := _build_closed_basin()

	_solver = FixedHydroGPU.new()
	_solver.name = "FixedHydroGPU_Soak"
	add_child(_solver)
	_solver.initialized.connect(_on_solver_initialized)
	_solver.initialization_failed.connect(_on_solver_init_failed)
	_solver.advance_recorded.connect(_on_advance_recorded)
	_solver.diagnostics_ready.connect(_on_diagnostics_ready)

	_volume = HydroVolumeDiagnosticsGPU.new()
	_volume.name = "HydroVolumeDiagnosticsGPU_Soak"
	add_child(_volume)
	_volume.initialized.connect(_on_volume_initialized)
	_volume.initialization_failed.connect(_on_volume_init_failed)
	_volume.volume_ready.connect(_on_volume_ready)
	_volume.readback_failed.connect(_on_volume_failed)

	var err := _solver.initialize(W, H, DX, fixture)
	if err != OK:
		_fail("solver initialize rejected (%d)" % int(err))


func _process(_delta: float) -> void:
	if _finished:
		return
	_stall_frames += 1
	if _stall_frames > STALL_FRAME_LIMIT:
		_fail("no GPU/readback progress for %d frames at advance %d" % [
			STALL_FRAME_LIMIT, _completed_advances])


func _parse_advance_count() -> int:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--hydro-soak-steps="):
			var value := int(arg.trim_prefix("--hydro-soak-steps="))
			return maxi(value, 100)
	return DEFAULT_ADVANCES


func _build_closed_basin() -> PackedFloat32Array:
	var state := PackedFloat32Array()
	state.resize(W * H * 4)
	var cx := (float(W) - 1.0) * 0.5
	var cy := (float(H) - 1.0) * 0.5
	for y in H:
		for x in W:
			var nx := (float(x) - cx) / cx
			var ny := (float(y) - cy) / cy
			var r2 := nx * nx + ny * ny

			# Bowl + small deterministic roughness. The corners are shallow but wet.
			var bed := 0.82 * r2 \
				+ 0.055 * sin(float(x) * 0.31) * cos(float(y) * 0.27)
			var mound := 0.34 * exp(-r2 / 0.085)
			var trough := 0.12 * exp(-((nx - 0.46) * (nx - 0.46)
				+ (ny + 0.18) * (ny + 0.18)) / 0.055)
			var eta := 1.78 + mound - trough
			var depth := maxf(eta - bed, 0.0)

			# A smooth rotational impulse creates long-lived moving waves without
			# adding/removing water. It decays naturally through Manning friction.
			var impulse := exp(-r2 / 0.32)
			var velocity := Vector2(-ny, nx) * (0.62 * impulse)
			var o := (x + y * W) * 4
			state[o] = depth
			state[o + 1] = depth * velocity.x
			state[o + 2] = depth * velocity.y
			state[o + 3] = bed
	return state


func _on_solver_initialized() -> void:
	_solver.manning_n = 0.018
	var err := _volume.initialize(_solver.current_state_rid(), W, H, DX)
	if err != OK:
		_fail("volume diagnostics initialize rejected (%d)" % int(err))


func _on_solver_init_failed(error: Error) -> void:
	_fail("solver pipeline initialization failed (%d)" % int(error))


func _on_volume_initialized() -> void:
	_request_volume()


func _on_volume_init_failed(error: Error) -> void:
	_fail("volume diagnostics initialization failed (%d)" % int(error))


func _request_volume() -> void:
	if _volume_pending:
		_fail("internal error: overlapping volume request")
		return
	var request_id := _volume.request_volume()
	if request_id < 0:
		_fail("GPU volume request rejected at advance %d" % _completed_advances)
		return
	_volume_pending = true


func _on_volume_ready(_request_id: int, volume_m3: float) -> void:
	_progress()
	_volume_pending = false
	if not is_finite(volume_m3) or volume_m3 <= 0.0:
		_fail("invalid reduced volume %s" % str(volume_m3))
		return

	_latest_volume = volume_m3
	if is_nan(_initial_volume):
		_initial_volume = volume_m3
		print("HYDRO_GPU_SOAK: start advances=", _target_advances,
			" initial_volume_m3=", _initial_volume)
		call_deferred("_advance_one")
		return

	var abs_drift := absf(volume_m3 - _initial_volume)
	var rel_drift := abs_drift / maxf(absf(_initial_volume), 1.0e-9)
	_max_abs_drift_m3 = maxf(_max_abs_drift_m3, abs_drift)
	_max_rel_drift = maxf(_max_rel_drift, rel_drift)

	print("HYDRO_GPU_SOAK: checkpoint ", _completed_advances, "/", _target_advances,
		" t=", snappedf(float(_completed_advances) * MACRO_DT, 0.001), "s",
		" volume=", volume_m3,
		" drift_m3=", abs_drift,
		" rel=", rel_drift)

	if abs_drift > MAX_ABSOLUTE_VOLUME_DRIFT_M3 \
			and rel_drift > MAX_RELATIVE_VOLUME_DRIFT:
		_fail("volume drift exceeded gate: abs=%.9g m3 rel=%.9g" % [
			abs_drift, rel_drift])
		return

	_checkpoint_pending = false
	if _completed_advances >= _target_advances:
		_pass()
	else:
		call_deferred("_advance_one")


func _on_volume_failed(_request_id: int, error: Error) -> void:
	_volume_pending = false
	_fail("volume readback failed (%d)" % int(error))


func _advance_one() -> void:
	if _finished or _checkpoint_pending or _volume_pending:
		return
	if _completed_advances >= _target_advances:
		_checkpoint_pending = true
		_request_volume()
		return

	var next_index := _completed_advances + 1
	var checkpoint := next_index % SAMPLE_INTERVAL == 0 or next_index == _target_advances
	_checkpoint_pending = checkpoint
	var step_id := _solver.advance(MACRO_DT, MAX_SUBSTEPS, checkpoint)
	if step_id < 0:
		_fail("advance rejected at index %d" % next_index)
		return
	if checkpoint:
		_checkpoint_step_id = step_id


func _on_advance_recorded(_step_id: int) -> void:
	_progress()
	_completed_advances += 1
	if not _checkpoint_pending:
		call_deferred("_advance_one")
	# Checkpoints wait for diagnostics_ready, which guarantees the compact health
	# readback has observed this advance before the control buffer is reused.


func _on_diagnostics_ready(step_id: int, d: Dictionary) -> void:
	_progress()
	if not _checkpoint_pending or step_id != _checkpoint_step_id:
		return
	if int(d.get("post_invalid_cells", -1)) != 0:
		_fail("invalid GPU cells at checkpoint %d: %s" % [
			_completed_advances, str(d)])
		return
	if bool(d.get("cfl_clamped", false)):
		_fail("CFL substep cap exhausted at checkpoint %d: remaining_dt=%s" % [
			_completed_advances, str(d.get("remaining_dt_s", NAN))])
		return
	var advanced := float(d.get("advanced_dt_s", 0.0))
	if absf(advanced - MACRO_DT) > 2.0e-5:
		_fail("macro time under/over-advanced at checkpoint %d: %.9g vs %.9g" % [
			_completed_advances, advanced, MACRO_DT])
		return
	_request_volume()


func _progress() -> void:
	_stall_frames = 0


func _pass() -> void:
	if _finished:
		return
	_finished = true
	print("HYDRO_GPU_SOAK: PASS advances=", _completed_advances,
		" simulated_s=", float(_completed_advances) * MACRO_DT,
		" initial_volume_m3=", _initial_volume,
		" final_volume_m3=", _latest_volume,
		" max_abs_drift_m3=", _max_abs_drift_m3,
		" max_rel_drift=", _max_rel_drift)
	_cleanup()
	get_tree().quit(0)


func _fail(message: String) -> void:
	if _finished:
		return
	_finished = true
	push_error("HYDRO_GPU_SOAK: " + message)
	_cleanup()
	get_tree().quit(1)


func _cleanup() -> void:
	if _volume != null and is_instance_valid(_volume):
		_volume.release()
		_volume.queue_free()
	if _solver != null and is_instance_valid(_solver):
		_solver.release()
		_solver.queue_free()
