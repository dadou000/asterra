class_name HydroWeatherForcingGPU
extends Node
## Distributed precipitation writer for SparseHydroStepGPU's atmospheric layer.
##
## The published WeatherSystem global ImageTexture is sampled directly through its
## RenderingDevice RID. No weather-grid readback or duplicate texture upload occurs.
## Only occupied sparse hydrology cells receive writes; unoccupied slots are zeroed.

signal initialized
signal initialization_failed(error: Error)
signal update_recorded(request_id: int)
signal update_failed(request_id: int, error: Error)
signal released

const LOCAL_X := 8
const LOCAL_Y := 8
const PARAM_FLOATS := 8

var maximum_precipitation_mm_h := 30.0
var land_infiltration_capacity_mm_h := 4.0
var weather_gain := 1.0
var land_bed_threshold_m := 0.0
var min_output_mm_h := 0.001

var _atlas: SparseHydroAtlasGPU
var _target := RID()
var _weather_texture: Texture2D
var _shader := RID()
var _pipeline := RID()
var _sampler := RID()
var _params := RID()
var _uniform_set := RID()
var _initialized := false
var _init_pending := false
var _update_pending := false
var _next_request_id := 1


func initialize(atlas: SparseHydroAtlasGPU, target_source_rid: RID,
		weather_texture: Texture2D) -> Error:
	if _initialized or _init_pending or _update_pending:
		return ERR_BUSY
	if atlas == null or not atlas.initialized_ok() or not target_source_rid.is_valid() \
			or weather_texture == null:
		return ERR_INVALID_PARAMETER
	if RenderingServer.get_rendering_device() == null:
		return ERR_UNAVAILABLE
	var shader_file: RDShaderFile = load("res://shaders/water/hydro_weather_forcing.glsl")
	if shader_file == null:
		return ERR_CANT_OPEN
	var spirv := shader_file.get_spirv()
	if spirv == null:
		return ERR_CANT_CREATE

	_atlas = atlas
	_target = target_source_rid
	_weather_texture = weather_texture
	_init_pending = true
	RenderingServer.call_on_render_thread(
		Callable(self, &"_init_render_thread").bind(spirv))
	return OK


func initialized_ok() -> bool:
	return _initialized


func pending() -> bool:
	return _update_pending


func request_update() -> int:
	if not _initialized or _update_pending or _atlas == null \
			or not _atlas.initialized_ok() or not _target.is_valid():
		return -1
	var request_id := _next_request_id
	_next_request_id += 1
	_update_pending = true
	var mm_h_to_mps := 1.0e-3 / 3600.0
	var values := PackedFloat32Array([
		float(_atlas.tile_resolution), float(_atlas.capacity),
		maxf(maximum_precipitation_mm_h, 0.0) * mm_h_to_mps,
		maxf(land_infiltration_capacity_mm_h, 0.0) * mm_h_to_mps,
		maxf(weather_gain, 0.0), land_bed_threshold_m,
		maxf(min_output_mm_h, 0.0) * mm_h_to_mps, 0.0,
	])
	RenderingServer.call_on_render_thread(
		Callable(self, &"_update_render_thread").bind(request_id, values.to_byte_array()))
	return request_id


func stats() -> Dictionary:
	return {
		"initialized": _initialized,
		"pending": _update_pending,
		"maximum_precipitation_mm_h": maximum_precipitation_mm_h,
		"land_infiltration_capacity_mm_h": land_infiltration_capacity_mm_h,
		"weather_gain": weather_gain,
		"land_bed_threshold_m": land_bed_threshold_m,
		"min_output_mm_h": min_output_mm_h,
		"cells_per_dispatch": 0 if _atlas == null else _atlas.total_cell_count(),
	}


func _init_render_thread(spirv: RDShaderSPIRV) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null or _weather_texture == null:
		call_deferred("_finish_init", ERR_UNAVAILABLE, {})
		return
	var weather_rd := RenderingServer.texture_get_rd_texture(_weather_texture.get_rid(), false)
	if not weather_rd.is_valid():
		call_deferred("_finish_init", ERR_UNAVAILABLE, {})
		return

	var shader := rd.shader_create_from_spirv(spirv)
	if not shader.is_valid():
		call_deferred("_finish_init", ERR_CANT_CREATE, {})
		return
	var pipeline := rd.compute_pipeline_create(shader)
	if not pipeline.is_valid():
		rd.free_rid(shader)
		call_deferred("_finish_init", ERR_CANT_CREATE, {})
		return

	var sampler_state := RDSamplerState.new()
	sampler_state.mag_filter = RenderingDevice.SAMPLER_FILTER_LINEAR
	sampler_state.min_filter = RenderingDevice.SAMPLER_FILTER_LINEAR
	sampler_state.mip_filter = RenderingDevice.SAMPLER_FILTER_NEAREST
	sampler_state.repeat_u = RenderingDevice.SAMPLER_REPEAT_MODE_REPEAT
	sampler_state.repeat_v = RenderingDevice.SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE
	sampler_state.repeat_w = RenderingDevice.SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE
	var sampler := rd.sampler_create(sampler_state)
	var zero_params := PackedByteArray()
	zero_params.resize(PARAM_FLOATS * 4)
	var params := rd.storage_buffer_create(zero_params.size(), zero_params)
	if not sampler.is_valid() or not params.is_valid():
		_free_many(rd, [params, sampler, pipeline, shader])
		call_deferred("_finish_init", ERR_CANT_CREATE, {})
		return

	var weather_uniform := RDUniform.new()
	weather_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_SAMPLER_WITH_TEXTURE
	weather_uniform.binding = 5
	weather_uniform.add_id(sampler)
	weather_uniform.add_id(weather_rd)
	var set_rid := rd.uniform_set_create([
		_storage_uniform(0, _atlas.state_a_rid()),
		_storage_uniform(1, _atlas.occupancy_rid()),
		_storage_uniform(2, _atlas.tile_metadata_rid()),
		_storage_uniform(3, _target),
		_storage_uniform(4, params),
		weather_uniform,
	], shader, 0)
	if not set_rid.is_valid():
		_free_many(rd, [params, sampler, pipeline, shader])
		call_deferred("_finish_init", ERR_CANT_CREATE, {})
		return
	call_deferred("_finish_init", OK, {
		"shader": shader, "pipeline": pipeline, "sampler": sampler,
		"params": params, "set": set_rid,
	})


func _finish_init(error: Error, bundle: Dictionary) -> void:
	_init_pending = false
	if error != OK:
		initialization_failed.emit(error)
		return
	_shader = bundle["shader"]
	_pipeline = bundle["pipeline"]
	_sampler = bundle["sampler"]
	_params = bundle["params"]
	_uniform_set = bundle["set"]
	_initialized = true
	initialized.emit()


func _update_render_thread(request_id: int, param_bytes: PackedByteArray) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null or not _pipeline.is_valid() or not _uniform_set.is_valid() \
			or not _params.is_valid() or _atlas == null or not _target.is_valid():
		call_deferred("_finish_update", request_id, ERR_UNAVAILABLE)
		return
	var err := rd.buffer_update(_params, 0, param_bytes.size(), param_bytes)
	if err != OK:
		call_deferred("_finish_update", request_id, err)
		return
	var compute := rd.compute_list_begin()
	rd.compute_list_bind_compute_pipeline(compute, _pipeline)
	rd.compute_list_bind_uniform_set(compute, _uniform_set, 0)
	rd.compute_list_dispatch(compute,
		int(ceil(float(_atlas.tile_resolution) / float(LOCAL_X))),
		int(ceil(float(_atlas.tile_resolution) / float(LOCAL_Y))),
		_atlas.capacity)
	rd.compute_list_end()
	call_deferred("_finish_update", request_id, OK)


func _finish_update(request_id: int, error: Error) -> void:
	_update_pending = false
	if error != OK:
		update_failed.emit(request_id, error)
	else:
		update_recorded.emit(request_id)


func _storage_uniform(binding: int, rid: RID) -> RDUniform:
	var uniform := RDUniform.new()
	uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
	uniform.binding = binding
	uniform.add_id(rid)
	return uniform


func _free_many(rd: RenderingDevice, values: Array) -> void:
	for value in values:
		if value is RID:
			var rid: RID = value
			if rid.is_valid():
				rd.free_rid(rid)


func release() -> void:
	if not _initialized and not _shader.is_valid():
		return
	var rids := [_uniform_set, _params, _sampler, _pipeline, _shader]
	_initialized = false
	_init_pending = false
	_update_pending = false
	_uniform_set = RID(); _params = RID(); _sampler = RID()
	_pipeline = RID(); _shader = RID()
	_atlas = null
	_target = RID()
	_weather_texture = null
	RenderingServer.call_on_render_thread(
		Callable(self, &"_release_render_thread").bind(rids))
	released.emit()


func _release_render_thread(rids: Array) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd != null:
		_free_many(rd, rids)


func _exit_tree() -> void:
	release()
