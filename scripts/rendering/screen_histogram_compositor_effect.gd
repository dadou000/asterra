extends CompositorEffect
## Debug-only GPU luminance histogram of the resolved HDR scene.
##
## This effect intentionally uses a tiny throttled synchronous readback instead of
## an async RenderingDevice callback. The payload is only 272 bytes and capture is
## enabled only while the Exposure debug tab is open, which keeps the diagnostic
## implementation simple and avoids callback/parser compatibility problems.

const SHADER_PATH := "res://shaders/screen_histogram.glsl"
const BIN_COUNT := 64
const BUFFER_UINT_COUNT := BIN_COUNT + 4
const BUFFER_BYTES := BUFFER_UINT_COUNT * 4
const MIDDLE_GREY := 0.18
const MIN_EV := -12.0
const MAX_EV := 8.0
const DEFAULT_SAMPLE_STRIDE := 4
const CAPTURE_INTERVAL_FRAMES := 6

var _rd: RenderingDevice
var _shader: RID = RID()
var _pipeline: RID = RID()
var _histogram_buffer: RID = RID()

var _state_mutex: Mutex = Mutex.new()
var _latest_snapshot: Dictionary = {}
var _frame_counter: int = 0
var _sample_stride: int = DEFAULT_SAMPLE_STRIDE


func _init() -> void:
	effect_callback_type = EFFECT_CALLBACK_TYPE_POST_TRANSPARENT
	access_resolved_color = true
	enabled = false

	_rd = RenderingServer.get_rendering_device()
	if _rd == null:
		return

	var shader_file: RDShaderFile = load(SHADER_PATH) as RDShaderFile
	if shader_file == null:
		push_error("ScreenHistogramCompositorEffect: failed to load %s" % SHADER_PATH)
		return

	var spirv: RDShaderSPIRV = shader_file.get_spirv()
	if spirv == null:
		push_error("ScreenHistogramCompositorEffect: shader has no SPIR-V")
		return

	_shader = _rd.shader_create_from_spirv(spirv)
	if not _shader.is_valid():
		push_error("ScreenHistogramCompositorEffect: failed to create compute shader")
		return

	_pipeline = _rd.compute_pipeline_create(_shader)
	if not _pipeline.is_valid():
		push_error("ScreenHistogramCompositorEffect: failed to create compute pipeline")
		return

	_histogram_buffer = _rd.storage_buffer_create(BUFFER_BYTES, PackedByteArray())
	if not _histogram_buffer.is_valid():
		push_error("ScreenHistogramCompositorEffect: failed to create histogram buffer")


func is_ready() -> bool:
	return _rd != null \
		and _shader.is_valid() \
		and _pipeline.is_valid() \
		and _histogram_buffer.is_valid()


func set_capture_enabled(value: bool) -> void:
	enabled = value and is_ready()
	if enabled:
		_frame_counter = CAPTURE_INTERVAL_FRAMES - 1


func set_sample_stride(value: int) -> void:
	_sample_stride = clampi(value, 1, 16)


func get_snapshot() -> Dictionary:
	_state_mutex.lock()
	var snapshot_copy: Dictionary = _latest_snapshot.duplicate(true)
	_state_mutex.unlock()
	return snapshot_copy


func clear_snapshot() -> void:
	_state_mutex.lock()
	_latest_snapshot.clear()
	_state_mutex.unlock()


func _notification(what: int) -> void:
	if what != NOTIFICATION_PREDELETE or _rd == null:
		return
	if _histogram_buffer.is_valid():
		_rd.free_rid(_histogram_buffer)
	if _pipeline.is_valid():
		_rd.free_rid(_pipeline)
	if _shader.is_valid():
		_rd.free_rid(_shader)


func _render_callback(callback_type: int, render_data: RenderData) -> void:
	if callback_type != EFFECT_CALLBACK_TYPE_POST_TRANSPARENT or not enabled:
		return
	if not is_ready():
		return

	_frame_counter += 1
	if _frame_counter < CAPTURE_INTERVAL_FRAMES:
		return
	_frame_counter = 0

	var buffers: RenderSceneBuffersRD = render_data.get_render_scene_buffers() as RenderSceneBuffersRD
	if buffers == null or buffers.get_view_count() <= 0:
		return

	var size: Vector2i = buffers.get_internal_size()
	if size.x <= 0 or size.y <= 0:
		return

	# Desktop play uses one view. The debug histogram intentionally samples only
	# view zero so stereo/multiview does not need a second merge pass.
	var color_rid: RID = buffers.get_color_layer(0)
	if not color_rid.is_valid():
		return

	var clear_status: int = _rd.buffer_clear(_histogram_buffer, 0, BUFFER_BYTES)
	if clear_status != OK:
		return

	var color_uniform: RDUniform = RDUniform.new()
	color_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_IMAGE
	color_uniform.binding = 0
	color_uniform.add_id(color_rid)

	var histogram_uniform: RDUniform = RDUniform.new()
	histogram_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
	histogram_uniform.binding = 1
	histogram_uniform.add_id(_histogram_buffer)

	var uniforms: Array[RDUniform] = [color_uniform, histogram_uniform]
	var uniform_set: RID = UniformSetCacheRD.get_cache(_shader, 0, uniforms)
	if not uniform_set.is_valid():
		return

	var stride: int = maxi(_sample_stride, 1)
	var sample_width: int = int(ceil(float(size.x) / float(stride)))
	var sample_height: int = int(ceil(float(size.y) / float(stride)))
	var x_groups: int = int(ceil(float(sample_width) / 8.0))
	var y_groups: int = int(ceil(float(sample_height) / 8.0))

	var push: PackedFloat32Array = PackedFloat32Array([
		float(stride), MIN_EV, MAX_EV, MIDDLE_GREY
	])
	var push_bytes: PackedByteArray = push.to_byte_array()

	var compute_list: int = _rd.compute_list_begin()
	_rd.compute_list_bind_compute_pipeline(compute_list, _pipeline)
	_rd.compute_list_bind_uniform_set(compute_list, uniform_set, 0)
	_rd.compute_list_set_push_constant(compute_list, push_bytes, push_bytes.size())
	_rd.compute_list_dispatch(compute_list, x_groups, y_groups, 1)
	_rd.compute_list_end()

	# Debug-only readback. BUFFER_BYTES is 272 bytes, and this runs only every six
	# render frames while the Exposure tab is active.
	var histogram_bytes: PackedByteArray = _rd.buffer_get_data(
		_histogram_buffer, 0, BUFFER_BYTES)
	_consume_histogram(histogram_bytes)


func _consume_histogram(data: PackedByteArray) -> void:
	if data.size() < BUFFER_BYTES:
		return

	var counts: PackedInt32Array = PackedInt32Array()
	counts.resize(BIN_COUNT)
	var total: int = 0
	for i in range(BIN_COUNT):
		var count: int = int(data.decode_u32(i * 4))
		counts[i] = count
		total += count

	var gpu_total: int = int(data.decode_u32(BIN_COUNT * 4))
	var below: int = int(data.decode_u32((BIN_COUNT + 1) * 4))
	var above: int = int(data.decode_u32((BIN_COUNT + 2) * 4))
	var invalid: int = int(data.decode_u32((BIN_COUNT + 3) * 4))
	if gpu_total > 0:
		total = gpu_total
	if total <= 0:
		return

	var max_count: int = 1
	var weighted_ev: float = 0.0
	var weighted_luminance: float = 0.0
	for i in range(BIN_COUNT):
		max_count = maxi(max_count, counts[i])
		var ev: float = _bin_center_ev(i)
		var weight: float = float(counts[i])
		weighted_ev += ev * weight
		weighted_luminance += MIDDLE_GREY * pow(2.0, ev) * weight

	var mean_ev: float = weighted_ev / float(total)
	var snapshot: Dictionary = {
		"bins": counts,
		"bin_count": BIN_COUNT,
		"max_count": max_count,
		"total_count": total,
		"below_count": below,
		"above_count": above,
		"invalid_count": invalid,
		"min_ev": MIN_EV,
		"max_ev": MAX_EV,
		"middle_grey": MIDDLE_GREY,
		"sample_stride": _sample_stride,
		"mean_ev": mean_ev,
		"geometric_mean_luminance": MIDDLE_GREY * pow(2.0, mean_ev),
		"arithmetic_mean_luminance": weighted_luminance / float(total),
		"p01_ev": _percentile_ev(counts, total, 0.01),
		"p50_ev": _percentile_ev(counts, total, 0.50),
		"p95_ev": _percentile_ev(counts, total, 0.95),
		"p99_ev": _percentile_ev(counts, total, 0.99),
		"below_fraction": float(below) / float(total),
		"above_fraction": float(above) / float(total),
	}

	_state_mutex.lock()
	_latest_snapshot = snapshot
	_state_mutex.unlock()


func _percentile_ev(counts: PackedInt32Array, total: int, percentile: float) -> float:
	var target: float = float(total) * clampf(percentile, 0.0, 1.0)
	var accumulated: float = 0.0
	for i in range(BIN_COUNT):
		accumulated += float(counts[i])
		if accumulated >= target:
			return _bin_center_ev(i)
	return MAX_EV


func _bin_center_ev(index: int) -> float:
	var t: float = (float(index) + 0.5) / float(BIN_COUNT)
	return lerpf(MIN_EV, MAX_EV, t)
