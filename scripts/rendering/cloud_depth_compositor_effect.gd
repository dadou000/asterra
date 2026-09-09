class_name CloudDepthCompositorEffect
extends CompositorEffect
## Depth-aware EVE-style cloud compositor plus a time-sliced local light volume.
## WeatherSystem owns horizontal weather; the light volume amortises indirect cloud
## lighting over 12 frames, matching the Kerbin reference update cadence.

const SHADER_PATH := "res://shaders/cloud_depth_composite.glsl"
const LIGHT_VOLUME_SHADER_PATH := "res://shaders/cloud_light_volume.glsl"
const PUSH_CONSTANT_FLOATS := 32
const PUSH_CONSTANT_BYTES := PUSH_CONSTANT_FLOATS * 4
const LIGHT_PUSH_FLOATS := 16
const LIGHT_PUSH_BYTES := LIGHT_PUSH_FLOATS * 4
const LIGHT_VOLUME_W := 224
const LIGHT_VOLUME_H := 224
const LIGHT_VOLUME_D := 64
const LIGHT_VOLUME_TIME_SLICES := 12

var _rd: RenderingDevice
var _shader := RID()
var _pipeline := RID()
var _light_shader := RID()
var _light_pipeline := RID()
var _light_volume := RID()
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
var _helion_angular_radius_rad := 0.00465475
var _weather_center := Vector3.UP
var _weather_span_m := 422400.0
var _light_volume_phase := 0
var _light_volume_updates := 0


func _init() -> void:
	effect_callback_type = EFFECT_CALLBACK_TYPE_POST_TRANSPARENT
	access_resolved_color = true
	access_resolved_depth = true
	_rd = RenderingServer.get_rendering_device()
	if _rd == null:
		return
	_create_main_pipeline()
	_create_light_pipeline()
	_create_samplers()
	_create_light_volume()


func _validated_compute_spirv(shader_file: RDShaderFile, label: String) -> RDShaderSPIRV:
	if shader_file == null:
		push_error("CloudDepthCompositorEffect: failed to load %s" % label)
		return null
	var spirv: RDShaderSPIRV = shader_file.get_spirv()
	if spirv == null:
		push_error("CloudDepthCompositorEffect: %s has no SPIR-V" % label)
		return null
	var compile_error := spirv.get_stage_compile_error(RenderingDevice.SHADER_STAGE_COMPUTE)
	if not compile_error.is_empty():
		push_error("CloudDepthCompositorEffect: %s compute compile error:\n%s" % [label, compile_error])
		return null
	var bytecode := spirv.get_stage_bytecode(RenderingDevice.SHADER_STAGE_COMPUTE)
	if bytecode.is_empty():
		push_error("CloudDepthCompositorEffect: %s produced empty compute bytecode" % label)
		return null
	return spirv


func _create_main_pipeline() -> void:
	var shader_file := load(SHADER_PATH) as RDShaderFile
	var spirv := _validated_compute_spirv(shader_file, SHADER_PATH)
	if spirv == null:
		return
	_shader = _rd.shader_create_from_spirv(spirv)
	if _shader.is_valid():
		_pipeline = _rd.compute_pipeline_create(_shader)
	else:
		push_error("CloudDepthCompositorEffect: RenderingDevice rejected %s" % SHADER_PATH)


func _create_light_pipeline() -> void:
	var shader_file := load(LIGHT_VOLUME_SHADER_PATH) as RDShaderFile
	var spirv := _validated_compute_spirv(shader_file, LIGHT_VOLUME_SHADER_PATH)
	if spirv == null:
		return
	_light_shader = _rd.shader_create_from_spirv(spirv)
	if _light_shader.is_valid():
		_light_pipeline = _rd.compute_pipeline_create(_light_shader)
	else:
		push_error("CloudDepthCompositorEffect: RenderingDevice rejected %s" % LIGHT_VOLUME_SHADER_PATH)


func _create_samplers() -> void:
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
	clamp_state.repeat_w = RenderingDevice.SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE
	_linear_clamp_sampler = _rd.sampler_create(clamp_state)

	var depth_state := RDSamplerState.new()
	depth_state.mag_filter = RenderingDevice.SAMPLER_FILTER_NEAREST
	depth_state.min_filter = RenderingDevice.SAMPLER_FILTER_NEAREST
	depth_state.mip_filter = RenderingDevice.SAMPLER_FILTER_NEAREST
	depth_state.repeat_u = RenderingDevice.SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE
	depth_state.repeat_v = RenderingDevice.SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE
	_depth_sampler = _rd.sampler_create(depth_state)


func _create_light_volume() -> void:
	if _rd == null:
		return
	var format := RDTextureFormat.new()
	format.texture_type = RenderingDevice.TEXTURE_TYPE_3D
	format.format = RenderingDevice.DATA_FORMAT_R16G16B16A16_SFLOAT
	format.width = LIGHT_VOLUME_W
	format.height = LIGHT_VOLUME_H
	format.depth = LIGHT_VOLUME_D
	format.array_layers = 1
	format.mipmaps = 1
	format.usage_bits = RenderingDevice.TEXTURE_USAGE_SAMPLING_BIT \
		| RenderingDevice.TEXTURE_USAGE_STORAGE_BIT
	_light_volume = _rd.texture_create(format, RDTextureView.new(), [])


func is_ready() -> bool:
	return _rd != null and _shader.is_valid() and _pipeline.is_valid() \
		and _light_shader.is_valid() and _light_pipeline.is_valid() \
		and _light_volume.is_valid() and _linear_repeat_sampler.is_valid() \
		and _linear_clamp_sampler.is_valid() and _depth_sampler.is_valid()


func _notification(what: int) -> void:
	if what != NOTIFICATION_PREDELETE or _rd == null:
		return
	for rid in [_pipeline, _shader, _light_pipeline, _light_shader, _light_volume,
			_linear_repeat_sampler, _linear_clamp_sampler, _depth_sampler]:
		if rid.is_valid():
			_rd.free_rid(rid)


func set_cloud_textures(shape_texture: Texture3D, detail_texture: Texture3D) -> void:
	_shape_texture_rs = shape_texture.get_rid() if shape_texture != null else RID()
	_detail_texture_rs = detail_texture.get_rid() if detail_texture != null else RID()


func set_weather_textures(global_weather: Texture2D, local_weather: Texture2D) -> void:
	_global_weather_rs = global_weather.get_rid() if global_weather != null else RID()
	_local_weather_rs = local_weather.get_rid() if local_weather != null else RID()


func set_weather_basis(center: Vector3, _east: Vector3, _north: Vector3, span_m: float) -> void:
	_state_mutex.lock()
	_weather_center = center.normalized()
	_weather_span_m = maxf(span_m, 1000.0)
	_state_mutex.unlock()


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
	if callback_type != EFFECT_CALLBACK_TYPE_POST_TRANSPARENT or not is_ready():
		return
	if not _shape_texture_rs.is_valid() or not _detail_texture_rs.is_valid() \
			or not _global_weather_rs.is_valid() or not _local_weather_rs.is_valid():
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
	var helion_angular_radius_rad := _helion_angular_radius_rad
	var weather_center := _weather_center
	var weather_span_m := _weather_span_m
	_state_mutex.unlock()

	_dispatch_light_volume(shape_rd, global_weather_rd, local_weather_rd,
		planet_radius, sun_dir, sun_intensity, wind_offset, weather_center, weather_span_m)

	var cam_transform: Transform3D = scene_data.get_cam_transform()
	var camera_planet := cam_transform.origin + floating_origin
	var view_count := buffers.get_view_count()
	var x_groups := (size.x - 1) / 8 + 1
	var y_groups := (size.y - 1) / 8 + 1

	for view in range(view_count):
		var color_rid := buffers.get_color_layer(view)
		var depth_rid := buffers.get_depth_layer(view)
		if not color_rid.is_valid() or not depth_rid.is_valid():
			continue

		var uniforms: Array[RDUniform] = []
		uniforms.append(_image_uniform(0, color_rid))
		uniforms.append(_sampled_uniform(1, _depth_sampler, depth_rid))
		uniforms.append(_sampled_uniform(2, _linear_repeat_sampler, shape_rd))
		uniforms.append(_sampled_uniform(3, _linear_repeat_sampler, detail_rd))
		uniforms.append(_sampled_uniform(4, _linear_clamp_sampler, global_weather_rd))
		uniforms.append(_sampled_uniform(5, _linear_clamp_sampler, local_weather_rd))
		uniforms.append(_sampled_uniform(6, _linear_clamp_sampler, _light_volume))

		var uniform_set := UniformSetCacheRD.get_cache(_shader, 0, uniforms)
		if not uniform_set.is_valid():
			continue

		var inv_projection: Projection = scene_data.get_view_projection(view).inverse()
		var push := PackedFloat32Array()
		_append_vec4(push, Vector4(camera_planet.x, camera_planet.y,
			camera_planet.z, planet_radius))
		_append_vec4(push, Vector4(sun_dir.x, sun_dir.y, sun_dir.z, sun_intensity))
		var packed_steps_helion := float(primary_steps) \
			+ clampf(helion_angular_radius_rad, 1.0e-7, 0.499999)
		_append_vec4(push, Vector4(wind_offset.x, wind_offset.y, wind_offset.z,
			packed_steps_helion))
		# Negative span means the volume is still warming up; the shader uses the
		# analytic path until all 12 time slices have been written at least once.
		var signed_span := weather_span_m if _light_volume_updates >= LIGHT_VOLUME_TIME_SLICES \
			else -weather_span_m
		_append_vec4(push, Vector4(weather_center.x, weather_center.y,
			weather_center.z, signed_span))
		_append_rotated_projection_column(push, inv_projection.x, cam_transform.basis)
		_append_rotated_projection_column(push, inv_projection.y, cam_transform.basis)
		_append_rotated_projection_column(push, inv_projection.z, cam_transform.basis)
		_append_rotated_projection_column(push, inv_projection.w, cam_transform.basis)
		if push.size() != PUSH_CONSTANT_FLOATS:
			continue

		var compute_list := _rd.compute_list_begin()
		_rd.compute_list_bind_compute_pipeline(compute_list, _pipeline)
		_rd.compute_list_bind_uniform_set(compute_list, uniform_set, 0)
		_rd.compute_list_set_push_constant(compute_list, push.to_byte_array(), PUSH_CONSTANT_BYTES)
		_rd.compute_list_dispatch(compute_list, x_groups, y_groups, 1)
		_rd.compute_list_end()


func _dispatch_light_volume(shape_rd: RID, global_weather_rd: RID, local_weather_rd: RID,
		planet_radius: float, sun_dir: Vector3, sun_intensity: float,
		wind_offset: Vector3, weather_center: Vector3, weather_span_m: float) -> void:
	var uniforms: Array[RDUniform] = []
	uniforms.append(_image_uniform(0, _light_volume))
	uniforms.append(_sampled_uniform(1, _linear_repeat_sampler, shape_rd))
	uniforms.append(_sampled_uniform(2, _linear_clamp_sampler, global_weather_rd))
	uniforms.append(_sampled_uniform(3, _linear_clamp_sampler, local_weather_rd))
	var uniform_set := UniformSetCacheRD.get_cache(_light_shader, 0, uniforms)
	if not uniform_set.is_valid():
		return

	var push := PackedFloat32Array()
	_append_vec4(push, Vector4(weather_center.x, weather_center.y, weather_center.z, weather_span_m))
	_append_vec4(push, Vector4(sun_dir.x, sun_dir.y, sun_dir.z, sun_intensity))
	_append_vec4(push, Vector4(wind_offset.x, wind_offset.y, wind_offset.z, planet_radius))
	_append_vec4(push, Vector4(float(LIGHT_VOLUME_W), float(LIGHT_VOLUME_H),
		float(LIGHT_VOLUME_D), float(_light_volume_phase)))
	if push.size() != LIGHT_PUSH_FLOATS:
		return

	var compute_list := _rd.compute_list_begin()
	_rd.compute_list_bind_compute_pipeline(compute_list, _light_pipeline)
	_rd.compute_list_bind_uniform_set(compute_list, uniform_set, 0)
	_rd.compute_list_set_push_constant(compute_list, push.to_byte_array(), LIGHT_PUSH_BYTES)
	_rd.compute_list_dispatch(compute_list,
		(LIGHT_VOLUME_W + 7) / 8, (LIGHT_VOLUME_H + 7) / 8, 1)
	_rd.compute_list_end()
	_light_volume_phase = (_light_volume_phase + 1) % LIGHT_VOLUME_TIME_SLICES
	_light_volume_updates += 1


static func _image_uniform(binding: int, rid: RID) -> RDUniform:
	var u := RDUniform.new()
	u.uniform_type = RenderingDevice.UNIFORM_TYPE_IMAGE
	u.binding = binding
	u.add_id(rid)
	return u


static func _sampled_uniform(binding: int, sampler: RID, texture: RID) -> RDUniform:
	var u := RDUniform.new()
	u.uniform_type = RenderingDevice.UNIFORM_TYPE_SAMPLER_WITH_TEXTURE
	u.binding = binding
	u.add_id(sampler)
	u.add_id(texture)
	return u


static func _append_vec4(array: PackedFloat32Array, value: Vector4) -> void:
	array.append(value.x)
	array.append(value.y)
	array.append(value.z)
	array.append(value.w)


static func _append_rotated_projection_column(array: PackedFloat32Array,
		column: Vector4, camera_basis: Basis) -> void:
	var rotated := camera_basis * Vector3(column.x, column.y, column.z)
	_append_vec4(array, Vector4(rotated.x, rotated.y, rotated.z, column.w))
