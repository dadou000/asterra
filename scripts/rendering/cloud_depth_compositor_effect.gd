class_name CloudDepthCompositorEffect
extends CompositorEffect
## Post-transparent volumetric-cloud compositor.
##
## Visible clouds cannot live in a Sky shader if they must pass in front of distant
## terrain: sky is background by definition. This effect reruns the cloud volume
## after scene rendering, reads the resolved depth buffer, and clips each cloud ray
## against the actual scene surface before compositing.

const SHADER_PATH := "res://shaders/cloud_depth_composite.glsl"

var _rd: RenderingDevice
var _shader := RID()
var _pipeline := RID()
var _linear_repeat_sampler := RID()
var _depth_sampler := RID()

var _shape_texture_rs := RID()
var _detail_texture_rs := RID()

var _state_mutex := Mutex.new()
var _floating_origin := Vector3.ZERO
var _planet_radius := 1000000.0
var _sun_dir := Vector3(1.0, 0.0, 0.0)
var _sun_intensity := 5.0265
var _wind_offset := Vector3.ZERO
var _primary_steps := 16
var _helion_angular_radius_rad := 0.00465475


func _init() -> void:
	effect_callback_type = EFFECT_CALLBACK_TYPE_POST_TRANSPARENT
	access_resolved_color = true
	access_resolved_depth = true
	_rd = RenderingServer.get_rendering_device()
	if _rd == null:
		return

	var shader_file := load(SHADER_PATH) as RDShaderFile
	if shader_file == null:
		push_error("CloudDepthCompositorEffect: failed to load %s" % SHADER_PATH)
		return
	var spirv: RDShaderSPIRV = shader_file.get_spirv()
	if spirv == null:
		push_error("CloudDepthCompositorEffect: shader has no SPIR-V")
		return
	_shader = _rd.shader_create_from_spirv(spirv)
	if not _shader.is_valid():
		push_error("CloudDepthCompositorEffect: failed to create compute shader")
		return
	_pipeline = _rd.compute_pipeline_create(_shader)

	var repeat_state := RDSamplerState.new()
	repeat_state.mag_filter = RenderingDevice.SAMPLER_FILTER_LINEAR
	repeat_state.min_filter = RenderingDevice.SAMPLER_FILTER_LINEAR
	repeat_state.mip_filter = RenderingDevice.SAMPLER_FILTER_LINEAR
	repeat_state.repeat_u = RenderingDevice.SAMPLER_REPEAT_MODE_REPEAT
	repeat_state.repeat_v = RenderingDevice.SAMPLER_REPEAT_MODE_REPEAT
	repeat_state.repeat_w = RenderingDevice.SAMPLER_REPEAT_MODE_REPEAT
	_linear_repeat_sampler = _rd.sampler_create(repeat_state)

	var depth_state := RDSamplerState.new()
	depth_state.mag_filter = RenderingDevice.SAMPLER_FILTER_NEAREST
	depth_state.min_filter = RenderingDevice.SAMPLER_FILTER_NEAREST
	depth_state.mip_filter = RenderingDevice.SAMPLER_FILTER_NEAREST
	depth_state.repeat_u = RenderingDevice.SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE
	depth_state.repeat_v = RenderingDevice.SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE
	_depth_sampler = _rd.sampler_create(depth_state)


func is_ready() -> bool:
	return _rd != null and _shader.is_valid() and _pipeline.is_valid() \
		and _linear_repeat_sampler.is_valid() and _depth_sampler.is_valid()


func _notification(what: int) -> void:
	if what != NOTIFICATION_PREDELETE or _rd == null:
		return
	if _shader.is_valid():
		_rd.free_rid(_shader)
	if _linear_repeat_sampler.is_valid():
		_rd.free_rid(_linear_repeat_sampler)
	if _depth_sampler.is_valid():
		_rd.free_rid(_depth_sampler)


func set_cloud_textures(shape_texture: Texture3D, detail_texture: Texture3D) -> void:
	# Keep RenderingServer RIDs. NoiseTexture3D populates asynchronously; resolving
	# to its current RD texture inside the render callback follows that resource
	# without touching the Resource object from the rendering thread.
	_shape_texture_rs = shape_texture.get_rid() if shape_texture != null else RID()
	_detail_texture_rs = detail_texture.get_rid() if detail_texture != null else RID()


func set_runtime_state(floating_origin: Vector3, planet_radius: float,
		sun_dir: Vector3, sun_intensity: float, wind_offset: Vector3,
		primary_steps: int, helion_angular_radius_rad: float) -> void:
	_state_mutex.lock()
	_floating_origin = floating_origin
	_planet_radius = planet_radius
	_sun_dir = sun_dir.normalized()
	_sun_intensity = sun_intensity
	_wind_offset = wind_offset
	_primary_steps = clampi(primary_steps, 6, 28)
	_helion_angular_radius_rad = maxf(helion_angular_radius_rad, 1.0e-7)
	_state_mutex.unlock()


func _render_callback(callback_type: int, render_data: RenderData) -> void:
	if callback_type != EFFECT_CALLBACK_TYPE_POST_TRANSPARENT:
		return
	if not is_ready():
		return
	if not _shape_texture_rs.is_valid() or not _detail_texture_rs.is_valid():
		return

	var buffers := render_data.get_render_scene_buffers() as RenderSceneBuffersRD
	var scene_data := render_data.get_render_scene_data()
	if buffers == null or scene_data == null:
		return
	var size := buffers.get_internal_size()
	if size.x <= 0 or size.y <= 0:
		return

	var shape_rd := RenderingServer.texture_get_rd_texture(_shape_texture_rs)
	var detail_rd := RenderingServer.texture_get_rd_texture(_detail_texture_rs)
	if not shape_rd.is_valid() or not detail_rd.is_valid():
		return

	_state_mutex.lock()
	var floating_origin := _floating_origin
	var planet_radius := _planet_radius
	var sun_dir := _sun_dir
	var sun_intensity := _sun_intensity
	var wind_offset := _wind_offset
	var primary_steps := _primary_steps
	var helion_angular_radius_rad := _helion_angular_radius_rad
	_state_mutex.unlock()

	var cam_transform: Transform3D = scene_data.get_cam_transform()
	var camera_planet := cam_transform.origin + floating_origin
	var camera_q: Quaternion = cam_transform.basis.get_rotation_quaternion()
	var view_count := buffers.get_view_count()
	# Same ceil-to-workgroup calculation used by Godot's compositor examples.
	var x_groups := (size.x - 1) / 8 + 1
	var y_groups := (size.y - 1) / 8 + 1

	for view in range(view_count):
		var color_rid := buffers.get_color_layer(view)
		var depth_rid := buffers.get_depth_layer(view)
		if not color_rid.is_valid() or not depth_rid.is_valid():
			continue

		var color_uniform := RDUniform.new()
		color_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_IMAGE
		color_uniform.binding = 0
		color_uniform.add_id(color_rid)

		var depth_uniform := RDUniform.new()
		depth_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_SAMPLER_WITH_TEXTURE
		depth_uniform.binding = 1
		depth_uniform.add_id(_depth_sampler)
		depth_uniform.add_id(depth_rid)

		var shape_uniform := RDUniform.new()
		shape_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_SAMPLER_WITH_TEXTURE
		shape_uniform.binding = 2
		shape_uniform.add_id(_linear_repeat_sampler)
		shape_uniform.add_id(shape_rd)

		var detail_uniform := RDUniform.new()
		detail_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_SAMPLER_WITH_TEXTURE
		detail_uniform.binding = 3
		detail_uniform.add_id(_linear_repeat_sampler)
		detail_uniform.add_id(detail_rd)

		var uniform_set := UniformSetCacheRD.get_cache(_shader, 0,
			[color_uniform, depth_uniform, shape_uniform, detail_uniform])
		if not uniform_set.is_valid():
			continue

		var inv_projection: Projection = scene_data.get_view_projection(view).inverse()
		var push := PackedFloat32Array()
		_append_vec4(push, Vector4(camera_planet.x, camera_planet.y,
			camera_planet.z, planet_radius))
		_append_vec4(push, Vector4(camera_q.x, camera_q.y, camera_q.z, camera_q.w))
		_append_vec4(push, Vector4(sun_dir.x, sun_dir.y, sun_dir.z, sun_intensity))

		# Godot caps push constants at 128 bytes for broad compatibility. Keep the
		# four vec4 + mat4 layout exactly at that limit by packing the integer step
		# count in the whole-number part and Helion's (< 0.5 rad) angular radius in
		# the fractional part. GLSL recovers them with floor() and fract().
		var packed_steps_helion := float(primary_steps) \
			+ clampf(helion_angular_radius_rad, 1.0e-7, 0.499999)
		_append_vec4(push, Vector4(wind_offset.x, wind_offset.y, wind_offset.z,
			packed_steps_helion))
		_append_vec4(push, inv_projection.x)
		_append_vec4(push, inv_projection.y)
		_append_vec4(push, inv_projection.z)
		_append_vec4(push, inv_projection.w)

		var compute_list := _rd.compute_list_begin()
		_rd.compute_list_bind_compute_pipeline(compute_list, _pipeline)
		_rd.compute_list_bind_uniform_set(compute_list, uniform_set, 0)
		_rd.compute_list_set_push_constant(compute_list, push.to_byte_array(), 128)
		_rd.compute_list_dispatch(compute_list, x_groups, y_groups, 1)
		_rd.compute_list_end()


static func _append_vec4(array: PackedFloat32Array, value: Vector4) -> void:
	array.append(value.x)
	array.append(value.y)
	array.append(value.z)
	array.append(value.w)
