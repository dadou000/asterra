extends Node
## Renderer-mode smoke test for the zero-copy SWE -> visible-water reconstruction.
## Full texture readback is test-only; production rendering samples the Texture2DRD
## directly and never reads this cache to CPU.

const W := 32
const H := 32
const DX := 8.0
const TIMEOUT_FRAMES := 600

var _solver: FixedHydroGPU
var _reconstructor: HydroSurfaceReconstructionGPU
var _frames := 0
var _finished := false


func _ready() -> void:
	if RenderingServer.get_rendering_device() == null:
		print("HYDRO_SURFACE_RECONSTRUCTION: SKIP (no global RenderingDevice)")
		get_tree().quit(0)
		return
	if WaterSystem.dynamic_surface_available():
		_begin()
	else:
		WaterSystem.dynamic_surface_ready.connect(_begin, CONNECT_ONE_SHOT)


func _process(_delta: float) -> void:
	if _finished:
		return
	_frames += 1
	if _frames > TIMEOUT_FRAMES:
		_fail("timed out")


func _begin() -> void:
	var resources := WaterSystem.surface_resources()
	if resources == null or not resources.available():
		_fail("shared dynamic surface resource unavailable")
		return

	WaterSystem.set_dynamic_surface_center_plane(Vector2.ZERO)
	WaterSystem.set_dynamic_surface_render_enabled(false)

	var state := PackedFloat32Array()
	state.resize(W * H * 4)
	for y in H:
		for x in W:
			var px := (float(x) + 0.5 - float(W) * 0.5) * DX
			var py := (float(y) + 0.5 - float(H) * 0.5) * DX
			var r2 := px * px + py * py
			var envelope := exp(-0.5 * r2 / (48.0 * 48.0))
			var bed := -10.0
			var depth := 10.0 + 2.0 * envelope
			var velocity := Vector2(1.4 * envelope, -0.55 * envelope)
			var o := (x + y * W) * 4
			state[o] = depth
			state[o + 1] = depth * velocity.x
			state[o + 2] = depth * velocity.y
			state[o + 3] = bed

	_solver = FixedHydroGPU.new()
	add_child(_solver)
	_solver.initialized.connect(_on_solver_initialized)
	_solver.initialization_failed.connect(_on_solver_failed)
	var err := _solver.initialize(W, H, DX, state)
	if err != OK:
		_fail("solver initialize rejected (%d)" % int(err))


func _on_solver_initialized() -> void:
	var resources := WaterSystem.surface_resources()
	_reconstructor = HydroSurfaceReconstructionGPU.new()
	add_child(_reconstructor)
	_reconstructor.initialized.connect(_on_reconstructor_initialized)
	_reconstructor.initialization_failed.connect(_on_reconstructor_failed)
	_reconstructor.reconstruction_recorded.connect(_on_reconstruction_recorded)
	_reconstructor.reconstruction_failed.connect(_on_reconstruction_failed)
	var err := _reconstructor.initialize(
		_solver.current_state_rid(), W, H, DX, _solver.dry_eps,
		resources.field_rid(), resources.field_resolution(), resources.field_half_extent_m())
	if err != OK:
		_fail("reconstructor initialize rejected (%d)" % int(err))


func _on_solver_failed(error: Error) -> void:
	_fail("solver initialization failed (%d)" % int(error))


func _on_reconstructor_initialized() -> void:
	# This is the dedicated opt-in validation path. Production remains disabled.
	WaterSystem.set_dynamic_surface_render_enabled(true)
	var request_id := _reconstructor.reconstruct(
		Vector2.ZERO, Vector2.ZERO, 0.0, 1.0)
	if request_id < 0:
		_fail("reconstruction request rejected")


func _on_reconstructor_failed(error: Error) -> void:
	_fail("reconstructor initialization failed (%d)" % int(error))


func _on_reconstruction_failed(_request_id: int, error: Error) -> void:
	_fail("reconstruction dispatch failed (%d)" % int(error))


func _on_reconstruction_recorded(_request_id: int) -> void:
	var rid := WaterSystem.surface_resources().field_rid()
	RenderingServer.call_on_render_thread(
		Callable(self, &"_request_texture_readback_render_thread").bind(rid))


func _request_texture_readback_render_thread(rid: RID) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null or not rid.is_valid() or not rd.texture_is_valid(rid):
		call_deferred("_fail", "dynamic texture invalid during readback")
		return
	var callback := Callable(self, &"_on_texture_bytes")
	var err := rd.texture_get_data_async(rid, 0, callback)
	if err != OK:
		call_deferred("_fail", "texture readback request failed (%d)" % int(err))


func _on_texture_bytes(bytes: PackedByteArray) -> void:
	call_deferred("_verify_texture", bytes)


func _verify_texture(bytes: PackedByteArray) -> void:
	var resources := WaterSystem.surface_resources()
	var res := resources.field_resolution()
	var expected_bytes := res * res * 16
	if bytes.size() != expected_bytes:
		_fail("texture byte count %d != %d" % [bytes.size(), expected_bytes])
		return

	var max_height := 0.0
	var max_speed := 0.0
	var max_activity := 0.0
	var nonzero := 0
	for i in res * res:
		var o := i * 16
		var h := bytes.decode_float(o)
		var vx := bytes.decode_float(o + 4)
		var vy := bytes.decode_float(o + 8)
		var activity := bytes.decode_float(o + 12)
		if not is_finite(h) or not is_finite(vx) or not is_finite(vy) or not is_finite(activity):
			_fail("non-finite reconstructed texel at %d" % i)
			return
		max_height = maxf(max_height, h)
		max_speed = maxf(max_speed, Vector2(vx, vy).length())
		max_activity = maxf(max_activity, activity)
		if absf(h) > 1.0e-4 or absf(vx) > 1.0e-4 or absf(vy) > 1.0e-4:
			nonzero += 1

	if max_height < 1.5 or max_height > 2.1:
		_fail("unexpected reconstructed max height %.6f" % max_height)
		return
	if max_speed < 1.0:
		_fail("reconstructed velocity channel too small: %.6f" % max_speed)
		return
	if max_activity <= 0.0:
		_fail("activity channel remained zero")
		return
	if nonzero <= 0 or nonzero >= res * res:
		_fail("source-domain masking failed: nonzero=%d" % nonzero)
		return

	var ocean := get_node_or_null("/root/OceanSystem")
	if ocean != null and ocean.has_method(&"material"):
		var material_value: Variant = ocean.call(&"material")
		if material_value is ShaderMaterial:
			var material := material_value as ShaderMaterial
			var enabled := float(material.get_shader_parameter("u_dynamic_surface_enabled"))
			if enabled < 0.5:
				_fail("ocean coat did not receive dynamic-surface enable")
				return

	WaterSystem.set_dynamic_surface_render_enabled(false)
	_finished = true
	print("HYDRO_SURFACE_RECONSTRUCTION: PASS height=", max_height,
		" speed=", max_speed, " activity=", max_activity,
		" nonzero_texels=", nonzero)
	_cleanup()
	get_tree().quit(0)


func _cleanup() -> void:
	WaterSystem.set_dynamic_surface_render_enabled(false)
	if _reconstructor != null and is_instance_valid(_reconstructor):
		_reconstructor.release()
		_reconstructor.queue_free()
	if _solver != null and is_instance_valid(_solver):
		_solver.release()
		_solver.queue_free()
	_reconstructor = null
	_solver = null


func _fail(message: String) -> void:
	if _finished:
		return
	_finished = true
	push_error("HYDRO_SURFACE_RECONSTRUCTION: " + message)
	_cleanup()
	get_tree().quit(1)
