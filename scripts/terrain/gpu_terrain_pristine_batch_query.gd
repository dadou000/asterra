class_name GPUPristineTerrainBatchQuery
extends Node
## One-shot asynchronous GPU sampler for Planet Studio's persistent pristine
## terrain envelope.
##
## Each request uploads an arbitrary list of unit directions, samples
## Planet.global_height_texture in one compute dispatch, and returns one float per
## direction through RenderingDevice's asynchronous readback. The queried texture
## contains macro elevation + coast profile only; analytic visible geomorph detail
## is intentionally excluded so persistent Deltas do not bake transient detail.

const SHADER_PATH := "res://shaders/terrain_pristine_batch_query.glsl"
const LOCAL_SIZE_X: int = 64
const PARAM_BYTES: int = 16

var supported: bool = false
var ready_state: bool = false
var failed: bool = false
var _init_requested: bool = false
var _busy: bool = false
var _next_token: int = 1
var _active_token: int = 0
var _active_callback: Callable = Callable()

var _rd_shader := RID()
var _rd_pipeline := RID()
var _rd_sampler := RID()


func _ready() -> void:
	process_priority = -4
	var method: String = RenderingServer.get_current_rendering_method()
	supported = method == "forward_plus" or method == "mobile"
	if supported:
		call_deferred("_try_initialize")


func _process(_delta: float) -> void:
	if supported and not ready_state and not failed:
		_try_initialize()


func is_available() -> bool:
	return supported and ready_state and not failed and not _busy \
		and Planet.ready_state and Planet.global_height_texture != null \
		and Planet.global_height_face_res > 0


func is_busy() -> bool:
	return _busy


func request(directions: Array[Vector3], callback: Callable) -> bool:
	if directions.is_empty() or not callback.is_valid() or not is_available():
		return false
	var count: int = directions.size()
	var packed := PackedFloat32Array()
	packed.resize(count * 4)
	for index: int in count:
		var direction: Vector3 = directions[index]
		if direction.length_squared() <= 1e-12:
			direction = Vector3.UP
		else:
			direction = direction.normalized()
		var base: int = index * 4
		packed[base + 0] = direction.x
		packed[base + 1] = direction.y
		packed[base + 2] = direction.z
		packed[base + 3] = 0.0
	var params := PackedFloat32Array([
		float(Planet.global_height_face_res),
		float(count),
		0.0,
		0.0,
	])
	var texture: Texture2DArray = Planet.global_height_texture
	if texture == null:
		return false
	var token: int = _next_token
	_next_token += 1
	_active_token = token
	_active_callback = callback
	_busy = true
	RenderingServer.call_on_render_thread(_render_dispatch_batch.bind(
		token,
		texture.get_rid(),
		packed.to_byte_array(),
		params.to_byte_array(),
		count,
		_rd_shader,
		_rd_pipeline,
		_rd_sampler))
	return true


## Invalidates the callback for an in-flight request. The render work is allowed to
## complete and frees its own temporary buffers, but its result is ignored.
func cancel_pending() -> void:
	_active_token = 0
	_active_callback = Callable()
	_busy = false


func stats() -> Dictionary:
	return {
		"supported": supported,
		"ready": ready_state,
		"failed": failed,
		"busy": _busy,
		"async_readback": true,
		"persistent_envelope_only": true,
	}


func _try_initialize() -> void:
	if _init_requested or ready_state or failed or not supported:
		return
	var resource: Resource = load(SHADER_PATH)
	if resource == null or not (resource is RDShaderFile):
		return
	var spirv: RDShaderSPIRV = (resource as RDShaderFile).get_spirv()
	if spirv == null or not spirv.compile_error_compute.is_empty() \
			or spirv.bytecode_compute.is_empty():
		failed = true
		push_error("Pristine terrain batch query shader is invalid.")
		return
	_init_requested = true
	RenderingServer.call_on_render_thread(_render_initialize.bind(spirv))


func _render_initialize(spirv: RDShaderSPIRV) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null:
		call_deferred("_on_initialized", false, RID(), RID(), RID())
		return
	var shader: RID = rd.shader_create_from_spirv(spirv, "Asterra pristine terrain batch query")
	if not shader.is_valid():
		call_deferred("_on_initialized", false, RID(), RID(), RID())
		return
	var pipeline: RID = rd.compute_pipeline_create(shader)
	if not pipeline.is_valid() or not rd.compute_pipeline_is_valid(pipeline):
		rd.free_rid(shader)
		call_deferred("_on_initialized", false, RID(), RID(), RID())
		return
	var sampler: RID = rd.sampler_create(RDSamplerState.new())
	if not sampler.is_valid():
		rd.free_rid(pipeline)
		rd.free_rid(shader)
		call_deferred("_on_initialized", false, RID(), RID(), RID())
		return
	call_deferred("_on_initialized", true, shader, pipeline, sampler)


func _on_initialized(success: bool, shader: RID, pipeline: RID, sampler: RID) -> void:
	_init_requested = false
	if not success:
		failed = true
		return
	_rd_shader = shader
	_rd_pipeline = pipeline
	_rd_sampler = sampler
	ready_state = true
	failed = false


func _render_dispatch_batch(token: int, texture_server_rid: RID,
		input_bytes: PackedByteArray, params_bytes: PackedByteArray, count: int,
		shader: RID, pipeline: RID, sampler: RID) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null or count <= 0 or not shader.is_valid() or not pipeline.is_valid() \
			or not sampler.is_valid():
		call_deferred("_accept_failure", token)
		return
	var texture_rd: RID = RenderingServer.texture_get_rd_texture(texture_server_rid, false)
	if not texture_rd.is_valid():
		call_deferred("_accept_failure", token)
		return
	var input_buffer: RID = rd.storage_buffer_create(input_bytes.size(), input_bytes)
	var output_zero := PackedByteArray()
	output_zero.resize(count * 4)
	var output_buffer: RID = rd.storage_buffer_create(output_zero.size(), output_zero)
	var params_buffer: RID = rd.uniform_buffer_create(PARAM_BYTES, params_bytes)
	if not input_buffer.is_valid() or not output_buffer.is_valid() or not params_buffer.is_valid():
		_render_free_request(rd, RID(), input_buffer, output_buffer, params_buffer)
		call_deferred("_accept_failure", token)
		return

	var uniforms: Array[RDUniform] = []
	var texture_uniform := RDUniform.new()
	texture_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_SAMPLER_WITH_TEXTURE
	texture_uniform.binding = 0
	texture_uniform.add_id(sampler)
	texture_uniform.add_id(texture_rd)
	uniforms.append(texture_uniform)
	var input_uniform := RDUniform.new()
	input_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
	input_uniform.binding = 1
	input_uniform.add_id(input_buffer)
	uniforms.append(input_uniform)
	var output_uniform := RDUniform.new()
	output_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
	output_uniform.binding = 2
	output_uniform.add_id(output_buffer)
	uniforms.append(output_uniform)
	var params_uniform := RDUniform.new()
	params_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_UNIFORM_BUFFER
	params_uniform.binding = 3
	params_uniform.add_id(params_buffer)
	uniforms.append(params_uniform)
	var uniform_set: RID = rd.uniform_set_create(uniforms, shader, 0)
	if not uniform_set.is_valid():
		_render_free_request(rd, RID(), input_buffer, output_buffer, params_buffer)
		call_deferred("_accept_failure", token)
		return

	var compute_list: int = rd.compute_list_begin()
	rd.compute_list_bind_compute_pipeline(compute_list, pipeline)
	rd.compute_list_bind_uniform_set(compute_list, uniform_set, 0)
	var groups_x: int = maxi(1, int(ceil(float(count) / float(LOCAL_SIZE_X))))
	rd.compute_list_dispatch(compute_list, groups_x, 1, 1)
	rd.compute_list_end()
	var err: Error = rd.buffer_get_data_async(
		output_buffer,
		_on_render_readback.bind(token, uniform_set, input_buffer, output_buffer, params_buffer),
		0,
		count * 4)
	if err != OK:
		_render_free_request(rd, uniform_set, input_buffer, output_buffer, params_buffer)
		call_deferred("_accept_failure", token)


func _on_render_readback(data: PackedByteArray, token: int, uniform_set: RID,
		input_buffer: RID, output_buffer: RID, params_buffer: RID) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd != null:
		_render_free_request(rd, uniform_set, input_buffer, output_buffer, params_buffer)
	var heights: PackedFloat32Array = data.to_float32_array()
	call_deferred("_accept_result", token, heights)


func _render_free_request(rd: RenderingDevice, uniform_set: RID, input_buffer: RID,
		output_buffer: RID, params_buffer: RID) -> void:
	for rid: RID in [uniform_set, input_buffer, output_buffer, params_buffer]:
		if rid.is_valid():
			rd.free_rid(rid)


func _accept_result(token: int, heights: PackedFloat32Array) -> void:
	if token != _active_token:
		return
	var callback: Callable = _active_callback
	_active_token = 0
	_active_callback = Callable()
	_busy = false
	if callback.is_valid():
		callback.call(true, heights)


func _accept_failure(token: int) -> void:
	if token != _active_token:
		return
	var callback: Callable = _active_callback
	_active_token = 0
	_active_callback = Callable()
	_busy = false
	if callback.is_valid():
		callback.call(false, PackedFloat32Array())
