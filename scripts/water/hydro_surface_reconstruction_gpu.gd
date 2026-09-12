class_name HydroSurfaceReconstructionGPU
extends Node
## Writes conservative SWE state directly into WaterSurfaceResources' RGBA32F
## texture on the global RenderingDevice. Source and target RIDs are externally
## owned; this object owns only its shader/pipeline/params/uniform-set resources.

signal initialized
signal initialization_failed(error: Error)
signal reconstruction_recorded(request_id: int)
signal reconstruction_failed(request_id: int, error: Error)
signal released

const LOCAL_X := 8
const LOCAL_Y := 8
const PARAM_FLOATS := 12

var _state := RID()
var _target := RID()
var _shader := RID()
var _pipeline := RID()
var _params := RID()
var _uniform_set := RID()
var _source_width := 0
var _source_height := 0
var _source_dx := 1.0
var _dry_eps := 1.0e-5
var _target_resolution := 0
var _target_half_extent_m := 1.0
var _initialized := false
var _init_pending := false
var _dispatch_pending := false
var _next_request_id := 1


func initialize(state_rid: RID, source_width: int, source_height: int,
		source_dx: float, dry_eps: float, target_texture_rid: RID,
		target_resolution: int, target_half_extent_m: float) -> Error:
	if _init_pending or _dispatch_pending:
		return ERR_BUSY
	if not state_rid.is_valid() or not target_texture_rid.is_valid():
		return ERR_INVALID_PARAMETER
	if source_width <= 0 or source_height <= 0 or source_dx <= 0.0 \
			or target_resolution <= 0 or target_half_extent_m <= 0.0:
		return ERR_INVALID_PARAMETER
	if RenderingServer.get_rendering_device() == null:
		return ERR_UNAVAILABLE
	var shader_file: RDShaderFile = load("res://shaders/water/hydro_surface_reconstruct.glsl")
	if shader_file == null:
		return ERR_CANT_OPEN
	var spirv := shader_file.get_spirv()
	if spirv == null:
		return ERR_CANT_CREATE

	_state = state_rid
	_target = target_texture_rid
	_source_width = source_width
	_source_height = source_height
	_source_dx = source_dx
	_dry_eps = maxf(dry_eps, 1.0e-8)
	_target_resolution = target_resolution
	_target_half_extent_m = target_half_extent_m
	_init_pending = true
	RenderingServer.call_on_render_thread(
		Callable(self, &"_init_render_thread").bind(spirv))
	return OK


func initialized_ok() -> bool:
	return _initialized


func pending() -> bool:
	return _dispatch_pending


func reconstruct(source_center_plane: Vector2, target_center_plane: Vector2,
		reference_surface_m: float = 0.0, activity_gain: float = 1.0) -> int:
	if not _initialized or _dispatch_pending:
		return -1
	var request_id := _next_request_id
	_next_request_id += 1
	_dispatch_pending = true
	var values := PackedFloat32Array([
		float(_source_width), float(_source_height), _source_dx, _dry_eps,
		source_center_plane.x, source_center_plane.y, reference_surface_m, maxf(activity_gain, 0.0),
		float(_target_resolution), _target_half_extent_m, target_center_plane.x, target_center_plane.y,
	])
	RenderingServer.call_on_render_thread(
		Callable(self, &"_reconstruct_render_thread").bind(values.to_byte_array(), request_id))
	return request_id


func _init_render_thread(spirv: RDShaderSPIRV) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null:
		call_deferred("_finish_init", ERR_UNAVAILABLE, {})
		return
	if not rd.texture_is_valid(_target):
		call_deferred("_finish_init", ERR_INVALID_PARAMETER, {})
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
	var zero := PackedByteArray()
	zero.resize(PARAM_FLOATS * 4)
	var params := rd.storage_buffer_create(zero.size(), zero)
	if not params.is_valid():
		rd.free_rid(pipeline); rd.free_rid(shader)
		call_deferred("_finish_init", ERR_CANT_CREATE, {})
		return
	var uniforms: Array[RDUniform] = [
		_storage_uniform(0, _state),
		_image_uniform(1, _target),
		_storage_uniform(2, params),
	]
	var set_rid := rd.uniform_set_create(uniforms, shader, 0)
	if not set_rid.is_valid():
		rd.free_rid(params); rd.free_rid(pipeline); rd.free_rid(shader)
		call_deferred("_finish_init", ERR_CANT_CREATE, {})
		return
	call_deferred("_finish_init", OK, {
		"shader": shader, "pipeline": pipeline, "params": params, "set": set_rid,
	})


func _finish_init(error: Error, bundle: Dictionary) -> void:
	_init_pending = false
	if error != OK:
		initialization_failed.emit(error)
		return
	_shader = bundle["shader"]
	_pipeline = bundle["pipeline"]
	_params = bundle["params"]
	_uniform_set = bundle["set"]
	_initialized = true
	initialized.emit()


func _reconstruct_render_thread(param_bytes: PackedByteArray, request_id: int) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null or not _pipeline.is_valid() or not _params.is_valid() \
			or not _uniform_set.is_valid() or not rd.texture_is_valid(_target):
		call_deferred("_finish_reconstruct", request_id, ERR_UNAVAILABLE)
		return
	var err := rd.buffer_update(_params, 0, param_bytes.size(), param_bytes)
	if err != OK:
		call_deferred("_finish_reconstruct", request_id, err)
		return
	var compute := rd.compute_list_begin()
	rd.compute_list_bind_compute_pipeline(compute, _pipeline)
	rd.compute_list_bind_uniform_set(compute, _uniform_set, 0)
	rd.compute_list_dispatch(compute,
		int(ceil(float(_target_resolution) / float(LOCAL_X))),
		int(ceil(float(_target_resolution) / float(LOCAL_Y))), 1)
	rd.compute_list_end()
	call_deferred("_finish_reconstruct", request_id, OK)


func _finish_reconstruct(request_id: int, error: Error) -> void:
	_dispatch_pending = false
	if error != OK:
		reconstruction_failed.emit(request_id, error)
		return
	reconstruction_recorded.emit(request_id)


func _storage_uniform(binding: int, rid: RID) -> RDUniform:
	var u := RDUniform.new()
	u.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
	u.binding = binding
	u.add_id(rid)
	return u


func _image_uniform(binding: int, rid: RID) -> RDUniform:
	var u := RDUniform.new()
	u.uniform_type = RenderingDevice.UNIFORM_TYPE_IMAGE
	u.binding = binding
	u.add_id(rid)
	return u


func release() -> void:
	if not _initialized and not _shader.is_valid():
		return
	var owned := [_uniform_set, _params, _pipeline, _shader]
	_initialized = false
	_dispatch_pending = false
	_uniform_set = RID(); _params = RID(); _pipeline = RID(); _shader = RID()
	RenderingServer.call_on_render_thread(
		Callable(self, &"_release_render_thread").bind(owned))
	released.emit()


func _release_render_thread(rids: Array) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null:
		return
	for value in rids:
		if value is RID:
			var rid: RID = value
			if rid.is_valid():
				rd.free_rid(rid)


func _exit_tree() -> void:
	release()
