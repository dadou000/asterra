extends Node
## Asynchronous GPU query for precise runtime ground contact.
##
## The CPU never evaluates procedural terrain detail. It submits one planet
## direction to a tiny compute shader and consumes the last completed float a few
## frames later. A coarse resident macro value is used only while the first GPU
## result is pending or if RenderingDevice is unavailable.

const SHADER_PATH := "res://shaders/terrain_height_query.glsl"
const PARAM_BYTES := 32
const RESULT_BYTES := 16
const MAX_CACHE_DISTANCE_M := 48.0
const MAX_CACHE_AGE_S := 0.40

var supported := false
var ready_state := false
var failed := false
var _init_requested := false
var _bindings_ready := false
var _bindings_building := false
var _binding_generation := -1
var _binding_macro_rid := RID()

var _rd_shader := RID()
var _rd_pipeline := RID()
var _rd_sampler := RID()
var _rd_param_buffer := RID()
var _rd_result_buffer := RID()
var _rd_uniform_set := RID()

var _requested_dir := Vector3(1.0, 0.0, 0.0)
var _request_pending := false
var _request_in_flight := false
var _request_token := 0

var _latest_valid := false
var _latest_dir := Vector3(1.0, 0.0, 0.0)
var _latest_height := 0.0
var _latest_received_s := -1.0


func _ready() -> void:
	process_priority = -5
	var method := RenderingServer.get_current_rendering_method()
	supported = method == "forward_plus" or method == "mobile"
	if Planet.has_signal("world_ready"):
		Planet.world_ready.connect(_on_world_ready)
	if supported:
		call_deferred("_try_initialize")


func _process(_dt: float) -> void:
	if not supported or failed:
		return
	if not ready_state:
		_try_initialize()
		return
	if not _ensure_bindings():
		return
	if _request_pending and not _request_in_flight:
		_dispatch_requested_height()


func _on_world_ready(_fields: PlanetFields) -> void:
	_bindings_ready = false
	_binding_generation = -1
	_binding_macro_rid = RID()
	_latest_valid = false
	_request_pending = false
	_request_in_flight = false


func request_height(direction: Vector3) -> void:
	if direction.length_squared() <= 1e-12:
		return
	_requested_dir = direction.normalized()
	_request_pending = true


func height_for_direction(direction: Vector3, fallback: float) -> float:
	if not _latest_valid or Planet.cfg == null:
		return fallback
	var d := direction.normalized()
	var angular := acos(clampf(d.dot(_latest_dir), -1.0, 1.0))
	var distance_m := angular * Planet.cfg.planet_radius
	var age := Time.get_ticks_msec() * 0.001 - _latest_received_s
	if distance_m > MAX_CACHE_DISTANCE_M or age > MAX_CACHE_AGE_S:
		return fallback
	# Player deltas remain CPU-authored state, not procedural terrain synthesis.
	return _latest_height + Deltas.offset_at(d)


func has_fresh_height(direction: Vector3) -> bool:
	if not _latest_valid or Planet.cfg == null:
		return false
	var d := direction.normalized()
	var distance_m := acos(clampf(d.dot(_latest_dir), -1.0, 1.0)) * Planet.cfg.planet_radius
	var age := Time.get_ticks_msec() * 0.001 - _latest_received_s
	return distance_m <= MAX_CACHE_DISTANCE_M and age <= MAX_CACHE_AGE_S


func _try_initialize() -> void:
	if _init_requested or ready_state or failed or not supported:
		return
	var resource: Resource = load(SHADER_PATH)
	if resource == null or not (resource is RDShaderFile):
		return
	var spirv: RDShaderSPIRV = (resource as RDShaderFile).get_spirv()
	if spirv == null or not spirv.compile_error_compute.is_empty() or spirv.bytecode_compute.is_empty():
		failed = true
		push_error("GPU terrain height query shader is invalid.")
		return
	_init_requested = true
	RenderingServer.call_on_render_thread(_render_initialize.bind(spirv))


func _render_initialize(spirv: RDShaderSPIRV) -> void:
	var rd: RenderingDevice = RenderingServer.get_rendering_device()
	if rd == null:
		call_deferred("_on_initialized", false, RID(), RID(), RID(), RID(), RID())
		return
	var shader := rd.shader_create_from_spirv(spirv, "Asterra terrain height query")
	if not shader.is_valid():
		call_deferred("_on_initialized", false, RID(), RID(), RID(), RID(), RID())
		return
	var pipeline := rd.compute_pipeline_create(shader)
	if not pipeline.is_valid() or not rd.compute_pipeline_is_valid(pipeline):
		rd.free_rid(shader)
		call_deferred("_on_initialized", false, RID(), RID(), RID(), RID(), RID())
		return
	var sampler := rd.sampler_create(RDSamplerState.new())
	var param_zero := PackedByteArray()
	param_zero.resize(PARAM_BYTES)
	var result_zero := PackedByteArray()
	result_zero.resize(RESULT_BYTES)
	var param_buffer := rd.uniform_buffer_create(PARAM_BYTES, param_zero)
	var result_buffer := rd.storage_buffer_create(RESULT_BYTES, result_zero)
	var ok := sampler.is_valid() and param_buffer.is_valid() and result_buffer.is_valid()
	call_deferred("_on_initialized", ok, shader, pipeline, sampler, param_buffer, result_buffer)


func _on_initialized(success: bool, shader: RID, pipeline: RID, sampler: RID,
		param_buffer: RID, result_buffer: RID) -> void:
	if not success:
		failed = true
		ready_state = false
		return
	_rd_shader = shader
	_rd_pipeline = pipeline
	_rd_sampler = sampler
	_rd_param_buffer = param_buffer
	_rd_result_buffer = result_buffer
	ready_state = true
	failed = false
	_bindings_ready = false


func _ensure_bindings() -> bool:
	if not ready_state or Planet.cfg == null or not Planet.ready_state:
		return false
	var context: Node = get_node_or_null("/root/PlanetContext")
	if context == null or not bool(context.get("ready_state")):
		return false
	var macro: Texture2DArray = Planet.global_height_texture
	if macro == null:
		return false
	var generation := int(context.get("generation"))
	var macro_rid := macro.get_rid()
	if _bindings_ready and generation == _binding_generation and macro_rid == _binding_macro_rid:
		return true
	if _bindings_building:
		return false
	var textures: Array = [
		macro,
		context.get("soil_texture"), context.get("surface_texture"),
		context.get("geology_texture"), context.get("structure_texture"),
		context.get("climate_texture"), context.get("hydrology_texture"),
	]
	var server_rids: Array[RID] = []
	for value: Variant in textures:
		if not (value is Texture2DArray):
			return false
		server_rids.append((value as Texture2DArray).get_rid())
	_bindings_building = true
	RenderingServer.call_on_render_thread(_render_build_uniform_set.bind(
		generation, macro_rid, server_rids, _rd_shader, _rd_sampler,
		_rd_result_buffer, _rd_param_buffer))
	return false


func _render_build_uniform_set(generation: int, macro_rid: RID, server_rids: Array,
		shader: RID, sampler: RID, result_buffer: RID, param_buffer: RID) -> void:
	var rd: RenderingDevice = RenderingServer.get_rendering_device()
	if rd == null:
		call_deferred("_on_uniform_set_built", false, generation, macro_rid, RID())
		return
	var uniforms: Array[RDUniform] = []
	for binding: int in 7:
		var rd_tex := RenderingServer.texture_get_rd_texture(server_rids[binding], false)
		if not rd_tex.is_valid():
			call_deferred("_on_uniform_set_built", false, generation, macro_rid, RID())
			return
		var u := RDUniform.new()
		u.uniform_type = RenderingDevice.UNIFORM_TYPE_SAMPLER_WITH_TEXTURE
		u.binding = binding
		u.add_id(sampler)
		u.add_id(rd_tex)
		uniforms.append(u)
	var result_u := RDUniform.new()
	result_u.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
	result_u.binding = 7
	result_u.add_id(result_buffer)
	uniforms.append(result_u)
	var params_u := RDUniform.new()
	params_u.uniform_type = RenderingDevice.UNIFORM_TYPE_UNIFORM_BUFFER
	params_u.binding = 8
	params_u.add_id(param_buffer)
	uniforms.append(params_u)
	var set := rd.uniform_set_create(uniforms, shader, 0)
	call_deferred("_on_uniform_set_built", set.is_valid(), generation, macro_rid, set)


func _on_uniform_set_built(success: bool, generation: int, macro_rid: RID,
		uniform_set: RID) -> void:
	_bindings_building = false
	if not success:
		_bindings_ready = false
		return
	_rd_uniform_set = uniform_set
	_binding_generation = generation
	_binding_macro_rid = macro_rid
	_bindings_ready = true


func _dispatch_requested_height() -> void:
	var context: Node = get_node_or_null("/root/PlanetContext")
	if context == null:
		return
	var d := _requested_dir
	_request_pending = false
	_request_in_flight = true
	_request_token += 1
	var token := _request_token
	var seed := Planet.cfg.stream_seed("gpu_visual_detail") & 0x00ffffff
	var values := PackedFloat32Array([
		d.x, d.y, d.z, Planet.cfg.planet_radius,
		float(Planet.global_height_face_res), float(context.get("face_res")),
		float(maxi(seed, 1)), 0.75,
	])
	RenderingServer.call_on_render_thread(_render_dispatch.bind(
		token, d, values.to_byte_array(), _rd_pipeline, _rd_uniform_set,
		_rd_param_buffer, _rd_result_buffer))


func _render_dispatch(token: int, direction: Vector3, params: PackedByteArray,
		pipeline: RID, uniform_set: RID, param_buffer: RID, result_buffer: RID) -> void:
	var rd: RenderingDevice = RenderingServer.get_rendering_device()
	if rd == null or not pipeline.is_valid() or not uniform_set.is_valid():
		call_deferred("_on_dispatch_failed", token)
		return
	if rd.buffer_update(param_buffer, 0, PARAM_BYTES, params) != OK:
		call_deferred("_on_dispatch_failed", token)
		return
	var list := rd.compute_list_begin()
	rd.compute_list_bind_compute_pipeline(list, pipeline)
	rd.compute_list_bind_uniform_set(list, uniform_set, 0)
	rd.compute_list_dispatch(list, 1, 1, 1)
	rd.compute_list_end()
	var err := rd.buffer_get_data_async(result_buffer,
		_on_render_readback.bind(token, direction), 0, RESULT_BYTES)
	if err != OK:
		call_deferred("_on_dispatch_failed", token)


func _on_render_readback(data: PackedByteArray, token: int, direction: Vector3) -> void:
	var h := data.decode_float(0) if data.size() >= 4 else NAN
	call_deferred("_accept_height", token, direction, h)


func _accept_height(token: int, direction: Vector3, height: float) -> void:
	if token != _request_token:
		return
	_request_in_flight = false
	if not is_finite(height):
		return
	_latest_height = height
	_latest_dir = direction.normalized()
	_latest_received_s = Time.get_ticks_msec() * 0.001
	_latest_valid = true


func _on_dispatch_failed(token: int) -> void:
	if token == _request_token:
		_request_in_flight = false


func stats() -> Dictionary:
	return {
		"supported": supported,
		"ready": ready_state,
		"failed": failed,
		"bindings_ready": _bindings_ready,
		"in_flight": _request_in_flight,
		"latest_valid": _latest_valid,
		"cpu_procedural_detail": false,
		"async_readback": true,
	}