class_name ScreenHistogramCompositorEffect
extends CompositorEffect
## Debug-only GPU luminance histogram of the resolved HDR scene.
##
## The effect is disabled by default and intended to be enabled only while the
## Exposure debug tab is visible. It samples one pixel every few source pixels,
## accumulates 64 logarithmic luminance bins on the GPU, then asynchronously
## reads back only 272 bytes. No full-frame texture ever crosses to the CPU.

const SHADER_PATH := "res://shaders/screen_histogram.glsl"
const BIN_COUNT := 64
const BUFFER_UINT_COUNT := BIN_COUNT + 4
const BUFFER_BYTES := BUFFER_UINT_COUNT * 4
const MIDDLE_GREY := 0.18
const MIN_EV := -12.0
const MAX_EV := 8.0
const DEFAULT_SAMPLE_STRIDE := 4
const CAPTURE_INTERVAL_FRAMES := 4

var _rd: RenderingDevice
var _shader := RID()
var _pipeline := RID()
var _histogram_buffer := RID()

var _state_mutex := Mutex.new()
var _latest_snapshot: Dictionary = {}
var _readback_pending := false
var _frame_counter := 0
var _sample_stride := DEFAULT_SAMPLE_STRIDE


func _init() -> void:
	effect_callback_type = EFFECT_CALLBACK_TYPE_POST_TRANSPARENT
	access_resolved_color = true
	enabled = false

	_rd = RenderingServer.get_rendering_device()
	if _rd == null:
		return

	var shader_file := load(SHADER_PATH) as RDShaderFile
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

	_histogram_buffer = _rd.storage_buffer_create(BUFFER_BYTES)
	if not _histogram_buffer.is_valid():
		push_error("ScreenHistogramCompositorEffect: failed to create histogram buffer")


func is_ready() -> bool:
	return _rd != null and _shader.is_valid() and _pipeline.is_valid() \
		and _histogram_buffer.is_valid()


func set_capture_enabled(value: bool) -> void:
	enabled = value and is_ready()
	if enabled:
		_frame_counter = CAPTURE_INTERVAL_FRAMES - 1


func set_sample_stride(value: int) -> void:
	_sample_stride = clampi(value, 1, 16)


func get_snapshot() -> Dictionary:
	_state_mutex.lock()
	var copy := _latest_snapshot.duplicate(true)
	_state_mutex.unlock()
	return copy


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

	var buffers := render_data.get_render_scene_buffers() as RenderSceneBuffersRD
	if buffers == null or buffers.get_view_count() <= 0:
		return
	var size := buffers.get_internal_size()
	if size.x <= 0 or size.y <= 0:
		return

	# Debug histogram uses the first eye/view. Normal desktop play has one view;
	# avoiding a merge step also keeps stereo debug capture predictable and cheap.
	var color_rid := buffers.get_color_layer(0)
	if not color_rid.is_valid():
		return

	# buffer_clear must happen outside an active compute list. Godot inserts the
	# required synchronization before the following dispatch.
	if _rd.buffer_clear(_histogram_buffer, 0, BUFFER_BYTES) != OK:
		return

	var color_uniform := RDUniform.new()
	color_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_IMAGE
	color_uniform.binding = 0
	color_uniform.add_id(color_rid)

	var histogram_uniform := RDUniform.new()
	histogram_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
	histogram_uniform.binding = 1
	histogram_uniform.add_id(_histogram_buffer)

	var uniform_set := UniformSetCacheRD.get_cache(_shader, 0,
		[color_uniform, histogram_uniform])
	if not uniform_set.is_valid():
		return

	var stride := max(_sample_stride, 1)
	var sample_width := int(ceil(float(size.x) / float(stride)))
	var sample_height := int(ceil(float(size.y) / float(stride)))
	var x_groups := int(ceil(float(sample_width) / 8.0))
	var y_groups := int(ceil(float(sample_height) / 8.0))

	var push := PackedFloat32Array([
		float(stride), MIN_EV, MAX_EV, MIDDLE_GREY
	])

	var compute_list := _rd.compute_list_begin()
	_rd.compute_list_bind_compute_pipeline(compute_list, _pipeline)
	_rd.compute_list_bind_uniform_set(compute_list, uniform_set, 0)
	_rd.compute_list_set_push_constant(compute_list, push.to_byte_array(), 16)
	_rd.compute_list_dispatch(compute_list, x_groups, y_groups, 1)
	_rd.compute_list_end()

	# Tiny asynchronous readback: 64 bins plus four counters = 272 bytes. Skip a
	# request if the previous one is still queued instead of ever stalling rendering.
	if not _readback_pending:
		var err := _rd.buffer_get_data_async(_histogram_buffer,
			Callable(self, "_on_histogram_readback"), 0, BUFFER_BYTES)
		_readback_pending = err == OK


func _on_histogram_readback(bytes: PackedByteArray) -> void:
	_readback_pending = false
	if bytes.size() < BUFFER_BYTES:
		return

	var counts := PackedInt32Array()
	counts.resize(BIN_COUNT)
	var total := 0
	for i in BIN_COUNT:
		var count := int(bytes.decode_u32(i * 4))
		counts[i] = count
		total += count

	# Use the explicit GPU counter as a consistency check. The bin sum should equal
	# it because out-of-range values are folded into the edge bins.
	var gpu_total := int(bytes.decode_u32(BIN_COUNT * 4))
	var below := int(bytes.decode_u32((BIN_COUNT + 1) * 4))
	var above := int(bytes.decode_u32((BIN_COUNT + 2) * 4))
	var invalid := int(bytes.decode_u32((BIN_COUNT + 3) * 4))
	if gpu_total > 0:
		total = gpu_total
	if total <= 0:
		return

	var max_count := 1
	var weighted_ev := 0.0
	var weighted_luminance := 0.0
	for i in BIN_COUNT:
		max_count = maxi(max_count, counts[i])
		var ev := _bin_center_ev(i)
		var weight := float(counts[i])
		weighted_ev += ev * weight
		weighted_luminance += MIDDLE_GREY * pow(2.0, ev) * weight

	var mean_ev := weighted_ev / float(total)
	var snapshot := {
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
	var target := float(total) * clampf(percentile, 0.0, 1.0)
	var accumulated := 0.0
	for i in BIN_COUNT:
		accumulated += float(counts[i])
		if accumulated >= target:
			return _bin_center_ev(i)
	return MAX_EV


func _bin_center_ev(index: int) -> float:
	var t := (float(index) + 0.5) / float(BIN_COUNT)
	return lerpf(MIN_EV, MAX_EV, t)
