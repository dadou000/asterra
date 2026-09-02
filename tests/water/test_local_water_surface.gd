extends Node
## Lightweight renderer-contract gate. This does not claim visual correctness; it
## verifies the production autoload, dense-grid budget and explicit opt-in gate.

const TIMEOUT_FRAMES := 300
var _frames := 0
var _finished := false


func _ready() -> void:
	if RenderingServer.get_rendering_device() == null:
		print("LOCAL_WATER_SURFACE: SKIP (no global RenderingDevice)")
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
	_require(LocalWaterSurface != null, "LocalWaterSurface autoload missing")
	_require(not LocalWaterSurface.is_render_enabled(),
		"local water must default disabled before renderer validation")
	var stats := LocalWaterSurface.stats()
	_require(int(stats.get("grid_cells", 0)) == 256, "unexpected local-water grid")
	_require(int(stats.get("triangles", 0)) == 131072,
		"unexpected local-water triangle budget")
	var material := LocalWaterSurface.material()
	_require(material != null and material.shader != null, "local water material/shader missing")
	if _finished:
		return

	LocalWaterSurface.set_render_enabled(true)
	_require(LocalWaterSurface.is_render_enabled(), "enable gate did not latch")
	var enabled_value: Variant = material.get_shader_parameter("u_render_enabled")
	_require(enabled_value is float and float(enabled_value) > 0.5,
		"shader did not receive enable gate")
	LocalWaterSurface.set_render_enabled(false)
	_require(not LocalWaterSurface.is_render_enabled(), "disable gate did not latch")
	if _finished:
		return

	_finished = true
	print("LOCAL_WATER_SURFACE: PASS grid=256 triangles=131072 default_gate=off")
	get_tree().quit(0)


func _require(condition: bool, message: String) -> void:
	if condition or _finished:
		return
	_fail(message)


func _fail(message: String) -> void:
	if _finished:
		return
	_finished = true
	LocalWaterSurface.set_render_enabled(false)
	push_error("LOCAL_WATER_SURFACE: " + message)
	get_tree().quit(1)
