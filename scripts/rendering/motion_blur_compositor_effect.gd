class_name MotionBlurCompositorEffect
extends CompositorEffect
## Camera-reprojection motion blur against the resolved scene depth.
##
## Reconstructs each pixel's world-adjacent render-space point from resolved depth
## and reprojects it into the PREVIOUS frame's clip space using this frame's and
## last frame's camera transform/projection. The screen-space delta is the blur
## vector. Purely camera-driven: correct for the dominant case here (the camera
## flying/walking through mostly-static terrain/ocean/scatter) without needing any
## per-object velocity buffer or changes to other shaders.
##
## `_prev_cam_transform` is kept in the CURRENT render-space epoch by translating it
## on every Frames.origin_shifted rebase (see on_origin_shifted), the same
## correction every other floating-origin consumer in this codebase applies to its
## own stored render-space state.

const SHADER_PATH := "res://shaders/motion_blur.glsl"
const REPROJECT_BUFFER_FLOATS := 20 # mat4 (16) + has_prev + 3 pad
const REPROJECT_BUFFER_BYTES := REPROJECT_BUFFER_FLOATS * 4

var _rd: RenderingDevice
var _shader: RID = RID()
var _pipeline: RID = RID()
var _color_sampler: RID = RID()
var _depth_sampler: RID = RID()
var _reproject_buffer: RID = RID()
var _scratch_texture: RID = RID()
var _scratch_size := Vector2i.ZERO

var _state_mutex := Mutex.new()
var _prev_cam_transform := Transform3D.IDENTITY
var _prev_projection := Projection.IDENTITY
var _has_prev := false

var strength: float = 0.55
var max_blur_px: float = 32.0
var sample_count: float = 12.0


func _init() -> void:
	effect_callback_type = EFFECT_CALLBACK_TYPE_POST_TRANSPARENT
	access_resolved_color = true
	access_resolved_depth = true
	enabled = true

	_rd = RenderingServer.get_rendering_device()
	if _rd == null:
		return

	var shader_file: RDShaderFile = load(SHADER_PATH) as RDShaderFile
	if shader_file == null:
		push_error("MotionBlurCompositorEffect: failed to load %s" % SHADER_PATH)
		return
	var spirv: RDShaderSPIRV = shader_file.get_spirv()
	if spirv == null:
		push_error("MotionBlurCompositorEffect: shader has no SPIR-V")
		return
	_shader = _rd.shader_create_from_spirv(spirv)
	if not _shader.is_valid():
		push_error("MotionBlurCompositorEffect: failed to create compute shader")
		return
	_pipeline = _rd.compute_pipeline_create(_shader)
	if not _pipeline.is_valid():
		push_error("MotionBlurCompositorEffect: failed to create compute pipeline")
		return

	var color_state := RDSamplerState.new()
	color_state.mag_filter = RenderingDevice.SAMPLER_FILTER_LINEAR
	color_state.min_filter = RenderingDevice.SAMPLER_FILTER_LINEAR
	color_state.repeat_u = RenderingDevice.SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE
	color_state.repeat_v = RenderingDevice.SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE
	_color_sampler = _rd.sampler_create(color_state)

	var depth_state := RDSamplerState.new()
	depth_state.mag_filter = RenderingDevice.SAMPLER_FILTER_NEAREST
	depth_state.min_filter = RenderingDevice.SAMPLER_FILTER_NEAREST
	depth_state.repeat_u = RenderingDevice.SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE
	depth_state.repeat_v = RenderingDevice.SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE
	_depth_sampler = _rd.sampler_create(depth_state)

	_reproject_buffer = _rd.storage_buffer_create(REPROJECT_BUFFER_BYTES, PackedByteArray())


func is_ready() -> bool:
	return _rd != null and _shader.is_valid() and _pipeline.is_valid() \
		and _color_sampler.is_valid() and _depth_sampler.is_valid() \
		and _reproject_buffer.is_valid()


## Frames.origin_shifted correction. Keeps the stored previous-frame camera pose in
## the same render-space epoch as the camera transform the next callback receives,
## so a floating-origin rebase never reads as a frame of camera motion.
func on_origin_shifted(delta_render: Vector3) -> void:
	_state_mutex.lock()
	_prev_cam_transform.origin += delta_render
	_state_mutex.unlock()


## Discards the stored previous frame so the next callback renders unblurred
## instead of reprojecting against a pose from before a hard cut (e.g. respawn).
func reset_history() -> void:
	_state_mutex.lock()
	_has_prev = false
	_state_mutex.unlock()


func _notification(what: int) -> void:
	if what != NOTIFICATION_PREDELETE or _rd == null:
		return
	for rid: RID in [_scratch_texture, _reproject_buffer, _depth_sampler,
			_color_sampler, _pipeline, _shader]:
		if rid.is_valid():
			_rd.free_rid(rid)


func _render_callback(callback_type: int, render_data: RenderData) -> void:
	if callback_type != EFFECT_CALLBACK_TYPE_POST_TRANSPARENT or not enabled or not is_ready():
		return

	var buffers: RenderSceneBuffersRD = render_data.get_render_scene_buffers() as RenderSceneBuffersRD
	var scene_data := render_data.get_render_scene_data()
	if buffers == null or scene_data == null or buffers.get_view_count() <= 0:
		return
	var size: Vector2i = buffers.get_internal_size()
	if size.x <= 0 or size.y <= 0:
		return

	# Single-view desktop camera path only, matching TerrainOcclusionCompositorEffect.
	var color_rid: RID = buffers.get_color_layer(0)
	var depth_rid: RID = buffers.get_depth_layer(0)
	if not color_rid.is_valid() or not depth_rid.is_valid():
		return

	var cam_transform: Transform3D = scene_data.get_cam_transform()
	var projection: Projection = scene_data.get_view_projection(0)
	var inv_projection: Projection = projection.inverse()
	var render_from_clip: Projection = Projection(cam_transform) * inv_projection

	_state_mutex.lock()
	var prev_cam_transform := _prev_cam_transform
	var prev_projection := _prev_projection
	var had_prev := _has_prev
	_state_mutex.unlock()

	var reproject_floats := PackedFloat32Array()
	reproject_floats.resize(REPROJECT_BUFFER_FLOATS)
	if had_prev:
		var prev_clip_from_render: Projection = prev_projection \
			* Projection(prev_cam_transform.affine_inverse())
		var prev_clip_from_current_clip: Projection = prev_clip_from_render * render_from_clip
		_write_projection(reproject_floats, prev_clip_from_current_clip)
		reproject_floats[16] = 1.0
	else:
		reproject_floats[16] = 0.0
	var reproject_bytes := reproject_floats.to_byte_array()
	if _rd.buffer_update(_reproject_buffer, 0, reproject_bytes.size(), reproject_bytes) != OK:
		return

	if not _ensure_scratch_texture(size):
		return

	var color_image_uniform := RDUniform.new()
	color_image_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_IMAGE
	color_image_uniform.binding = 0
	color_image_uniform.add_id(color_rid)

	var depth_uniform := RDUniform.new()
	depth_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_SAMPLER_WITH_TEXTURE
	depth_uniform.binding = 1
	depth_uniform.add_id(_depth_sampler)
	depth_uniform.add_id(depth_rid)

	var scratch_image_uniform := RDUniform.new()
	scratch_image_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_IMAGE
	scratch_image_uniform.binding = 2
	scratch_image_uniform.add_id(_scratch_texture)

	var scratch_sampler_uniform := RDUniform.new()
	scratch_sampler_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_SAMPLER_WITH_TEXTURE
	scratch_sampler_uniform.binding = 3
	scratch_sampler_uniform.add_id(_color_sampler)
	scratch_sampler_uniform.add_id(_scratch_texture)

	var reproject_uniform := RDUniform.new()
	reproject_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
	reproject_uniform.binding = 4
	reproject_uniform.add_id(_reproject_buffer)

	var uniform_set: RID = UniformSetCacheRD.get_cache(_shader, 0, [
		color_image_uniform, depth_uniform, scratch_image_uniform,
		scratch_sampler_uniform, reproject_uniform,
	])
	if not uniform_set.is_valid():
		return

	var x_groups := (size.x + 7) / 8
	var y_groups := (size.y + 7) / 8

	# Godot's resolved color render target has STORAGE_BIT + SAMPLING_BIT but not
	# the copy usage bits texture_copy() needs, so pass 0 duplicates it into the
	# scratch texture via imageLoad/imageStore instead of a driver blit. Pass 1 then
	# samples that untouched copy for the blur taps -- sampling neighbours while
	# writing the same image in one dispatch would otherwise be a read/write race.
	var compute_list := _rd.compute_list_begin()
	_rd.compute_list_bind_compute_pipeline(compute_list, _pipeline)
	_rd.compute_list_bind_uniform_set(compute_list, uniform_set, 0)

	_rd.compute_list_set_push_constant(compute_list, _build_push(size, 0.0), 32)
	_rd.compute_list_dispatch(compute_list, x_groups, y_groups, 1)
	_rd.compute_list_add_barrier(compute_list)

	_rd.compute_list_set_push_constant(compute_list, _build_push(size, 1.0), 32)
	_rd.compute_list_dispatch(compute_list, x_groups, y_groups, 1)
	_rd.compute_list_end()

	_state_mutex.lock()
	_prev_cam_transform = cam_transform
	_prev_projection = projection
	_has_prev = true
	_state_mutex.unlock()


func _build_push(size: Vector2i, pass_id: float) -> PackedByteArray:
	var push := PackedFloat32Array([
		float(size.x), float(size.y), strength, max_blur_px, sample_count, pass_id, 0.0, 0.0
	])
	return push.to_byte_array()


func _ensure_scratch_texture(size: Vector2i) -> bool:
	if _scratch_texture.is_valid() and _scratch_size == size:
		return true
	if _scratch_texture.is_valid():
		_rd.free_rid(_scratch_texture)
		_scratch_texture = RID()
	var fmt := RDTextureFormat.new()
	fmt.format = RenderingDevice.DATA_FORMAT_R16G16B16A16_SFLOAT
	fmt.width = size.x
	fmt.height = size.y
	fmt.usage_bits = RenderingDevice.TEXTURE_USAGE_SAMPLING_BIT \
		| RenderingDevice.TEXTURE_USAGE_STORAGE_BIT
	_scratch_texture = _rd.texture_create(fmt, RDTextureView.new())
	_scratch_size = size if _scratch_texture.is_valid() else Vector2i.ZERO
	return _scratch_texture.is_valid()


static func _write_projection(out: PackedFloat32Array, p: Projection) -> void:
	var columns: Array[Vector4] = [p.x, p.y, p.z, p.w]
	for c in 4:
		var col: Vector4 = columns[c]
		out[c * 4 + 0] = col.x
		out[c * 4 + 1] = col.y
		out[c * 4 + 2] = col.z
		out[c * 4 + 3] = col.w
