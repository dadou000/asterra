class_name CloudDepthCompositorEffect
extends CompositorEffect
## Depth-aware volumetric cloud compositor driven by the AVX2 weather model.

const SHADER_PATH := "res://shaders/cloud_depth_composite.glsl"

var _rd: RenderingDevice
var _shader := RID()
var _pipeline := RID()
var _linear_repeat_sampler := RID()
var _linear_clamp_sampler := RID()
var _depth_sampler := RID()
var _shape_texture_rs := RID()
var _detail_texture_rs := RID()
var _global_weather_rs := RID()
var _local_weather_rs := RID()

var _state_mutex := Mutex.new()
var _floating_origin := Vector3.ZERO
var _planet_radius := 1000000.0
var _sun_dir := Vector3(1.0, 0.0, 0.0)
var _sun_intensity := 5.0265
var _wind_offset := Vector3.ZERO
var _primary_steps := 16
var _weather_center := Vector3.UP
var _weather_east := Vector3.RIGHT
var _weather_north := Vector3.FORWARD
var _weather_span_m := 422400.0


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

	var clamp_state := RDSamplerState.new()
	clamp_state.mag_filter = RenderingDevice.SAMPLER_FILTER_LINEAR
	clamp_state.min_filter = RenderingDevice.SAMPLER_FILTER_LINEAR
	clamp_state.mip_filter = RenderingDevice.SAMPLER_FILTER_LINEAR
	clamp_state.repeat_u = RenderingDevice.SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE
	clamp_state.repeat_v = RenderingDevice.SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE
	_linear_clamp_sampler = _rd.sampler_create(clamp_state)

	var depth_state := RDSamplerState.new()
	depth_state.mag_filter = RenderingDevice.SAMPLER_FILTER_NEAREST
	depth_state.min_filter = RenderingDevice.SAMPLER_FILTER_NEAREST
	depth_state.mip_filter = RenderingDevice.SAMPLER_FILTER_NEAREST
	depth_state.repeat_u = RenderingDevice.SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE
	depth_state.repeat_v = RenderingDevice.SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE
	_depth_sampler = _rd.sampler_create(depth_state)


func is_ready() -> bool:
	return _rd != null and _shader.is_valid() and _pipeline.is_valid() \
		and _linear_repeat_sampler.is_valid() and _linear_clamp_sampler.is_valid() \
		and _depth_sampler.is_valid()


func _notification(what: int) -> void:
	if what != NOTIFICATION_PREDELETE or _rd == null:
		return
	for rid in [_shader, _linear_repeat_sampler, _linear_clamp_sampler, _depth_sampler]:
		if rid.is_valid():
			_rd.free_rid(rid)


func set_cloud_textures(shape_texture: Texture3D, detail_texture: Texture3D) -> void:
	_shape_texture_rs = shape_texture.get_rid() if shape_texture != null else RID()
	_detail_texture_rs = detail_texture.get_rid() if detail_texture != null else RID()


func set_weather_textures(global_weather: Texture2D, local_weather: Texture2D) -> void:
	_global_weather_rs = global_weather.get_rid() if global_weather != null else RID()
	_local_weather_rs = local_weather.get_rid() if local_weather != null else RID()


func set_weather_basis(center: Vector3, east: Vector3, north: Vector3, span_m: float) -> void:
	_state_mutex.lock()
	_weather_center = center.normalized()
	_weather_east = east.normalized()
	_weather_north = north.normalized()
	_weather_span_m = maxf(span_m, 1000.0)
	_state_mutex.unlock()


func set_runtime_state(floating_origin: Vector3, planet_radius: float,
		sun_dir: Vector3, sun_intensity: float, wind_offset: Vector3,
		primary_steps: int) -> void:
	_state_mutex.lock()
	_floating_origin = floating_origin
	_planet_radius = planet_radius
	_sun_dir = sun_dir.normalized()
	_sun_intensity = sun_intensity
	_wind_offset = wind_offset
	_primary_steps = clampi(primary_steps, 6, 28)
	_state_mutex.unlock()


func _render_callback(callback_type: int, render_data: RenderData) -> void:
	if callback_type != EFFECT_CALLBACK_TYPE_POST_TRANSPARENT or not is_ready():
		return
	if not _shape_texture_rs.is_valid() or not _detail_texture_rs.is_valid():
		return
	if not _global_weather_rs.is_valid() or not _local_weather_rs.is_valid():
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
	var global_weather_rd := RenderingServer.texture_get_rd_texture(_global_weather_rs)
	var local_weather_rd := RenderingServer.texture_get_rd_texture(_local_weather_rs)
	if not shape_rd.is_valid() or not detail_rd.is_valid() \
			or not global_weather_rd.is_valid() or not local_weather_rd.is_valid():
		return

	_state_mutex.lock()
	var floating_origin := _floating_origin
	var planet_radius := _planet_radius
	var sun_dir := _sun_dir
	var sun_intensity := _sun_intensity
	var wind_offset := _wind_offset
	var primary_steps := _primary_steps
	var weather_center := _weather_center
	var weather_east := _weather_east
	var weather_north := _weather_north
	var weather_span_m := _weather_span_m
	_state_mutex.unlock()

	var cam_transform: Transform3D = scene_data.get_cam_transform()
	var camera_planet := cam_transform.origin + floating_origin
	var camera_q: Quaternion = cam_transform.basis.get_rotation_quaternion()
	var view_count := buffers.get_view_count()
	var x_groups := (size.x - 1) / 8 + 1
	var y_groups := (size.y - 1) / 8 + 1

	for view in range(view_count):
		var color_rid := buffers.get_color_layer(view)
		var depth_rid := buffers.get_depth_layer(view)
		if not color_rid.is_valid() or not depth_rid.is_valid():
			continue

		var uniforms: Array[RDUniform] = []
		var color_uniform := RDUniform.new()
		color_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_IMAGE
		color_uniform.binding = 0
		color_uniform.add_id(color_rid)
		uniforms.append(color_uniform)

		var depth_uniform := RDUniform.new()
		depth_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_SAMPLER_WITH_TEXTURE
		depth_uniform.binding = 1
		depth_uniform.add_id(_depth_sampler)
		depth_uniform.add_id(depth_rid)
		uniforms.append(depth_uniform)

		var shape_uniform := RDUniform.new()
		shape_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_SAMPLER_WITH_TEXTURE
		shape_uniform.binding = 2
		shape_uniform.add_id(_linear_repeat_sampler)
		shape_uniform.add_id(shape_rd)
		uniforms.append(shape_uniform)

		var detail_uniform := RDUniform.new()
		detail_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_SAMPLER_WITH_TEXTURE
		detail_uniform.binding = 3
		detail_uniform.add_id(_linear_repeat_sampler)
		detail_uniform.add_id(detail_rd)
		uniforms.append(detail_uniform)

		for binding_and_rid in [[4, global_weather_rd], [5, local_weather_rd]]:
			var u := RDUniform.new()
			u.uniform_type = RenderingDevice.UNIFORM_TYPE_SAMPLER_WITH_TEXTURE
			u.binding = binding_and_rid[0]
			u.add_id(_linear_clamp_sampler)
			u.add_id(binding_and_rid[1])
			uniforms.append(u)

		var uniform_set := UniformSetCacheRD.get_cache(_shader, 0, uniforms)
		if not uniform_set.is_valid():
			continue

		var inv_projection: Projection = scene_data.get_view_projection(view).inverse()
		var push := PackedFloat32Array()
		_append_vec4(push, Vector4(camera_planet.x, camera_planet.y, camera_planet.z, planet_radius))
		_append_vec4(push, Vector4(camera_q.x, camera_q.y, camera_q.z, camera_q.w))
		_append_vec4(push, Vector4(sun_dir.x, sun_dir.y, sun_dir.z, sun_intensity))
		_append_vec4(push, Vector4(wind_offset.x, wind_offset.y, wind_offset.z, float(primary_steps)))
		_append_vec4(push, Vector4(weather_center.x, weather_center.y, weather_center.z, weather_span_m))
		_append_vec4(push, Vector4(weather_east.x, weather_east.y, weather_east.z, 0.0))
		_append_vec4(push, Vector4(weather_north.x, weather_north.y, weather_north.z, 0.0))
		_append_vec4(push, inv_projection.x)
		_append_vec4(push, inv_projection.y)
		_append_vec4(push, inv_projection.z)
		_append_vec4(push, inv_projection.w)

		var compute_list := _rd.compute_list_begin()
		_rd.compute_list_bind_compute_pipeline(compute_list, _pipeline)
		_rd.compute_list_bind_uniform_set(compute_list, uniform_set, 0)
		_rd.compute_list_set_push_constant(compute_list, push.to_byte_array(), 176)
		_rd.compute_list_dispatch(compute_list, x_groups, y_groups, 1)
		_rd.compute_list_end()


static func _append_vec4(array: PackedFloat32Array, value: Vector4) -> void:
	array.append(value.x)
	array.append(value.y)
	array.append(value.z)
	array.append(value.w)
