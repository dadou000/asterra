extends Node
## Tiny asynchronous query service for the active GPU deformation tile.
##
## The old path copied the entire 256x256 RGBA32F active texture back to the CPU
## many times per second. This service instead sends only the local positions that
## physics/gameplay actually asks for, samples them on the GPU, and reads back a
## few vec4 values. Cached values are allowed to be one or two GPU updates old so
## no physics frame ever waits on the renderer.

const SHADER_PATH := "res://shaders/terrain_deformation_sample.glsl"
const MAX_QUERIES := 256
const QUERY_FLOATS := 4
const BYTES_PER_QUERY := QUERY_FLOATS * 4
const QUERY_BUFFER_BYTES := MAX_QUERIES * BYTES_PER_QUERY
const RESULT_BUFFER_BYTES := MAX_QUERIES * BYTES_PER_QUERY
const PARAM_BYTES := 16
const CACHE_LIMIT := 4096
const CACHE_QUANTIZATION_PER_M := 16.0

var ready_state := false
var failed := false

var _init_requested := false
var _dispatch_in_flight := false
var _dispatch_token := 0
var _last_window_generation := -1
var _queries_dispatched := 0
var _readbacks := 0
var _dropped_queries := 0
var _latest_result_time_s := 0.0

var _rd_shader := RID()
var _rd_pipeline := RID()
var _rd_query_buffer := RID()
var _rd_result_buffer := RID()
var _rd_param_buffer := RID()
var _uniform_sets: Array[RID] = []

var _pending_records: Array[PackedFloat32Array] = []
var _pending_keys := PackedInt64Array()
var _pending_key_set: Dictionary = {}
var _cache: Dictionary = {}
var _cache_generation: Dictionary = {}


func _ready() -> void:
	process_priority = -1
	call_deferred("_try_initialize")


func _process(_dt: float) -> void:
	if failed:
		return
	if not ready_state:
		_try_initialize()
		return
	var window_generation: int = TerrainDeformationGPU.window_generation()
	if window_generation != _last_window_generation:
		_reset_cache_for_window(window_generation)
	if not _pending_records.is_empty() and not _dispatch_in_flight:
		_dispatch_pending()


func active_height_offset(direction: Vector3) -> float:
	return _sample_state(direction).x


func active_state(direction: Vector3) -> Vector2:
	var state: Vector4 = _sample_state(direction)
	return Vector2(state.y, state.z)


func stats() -> Dictionary:
	var age_s: float = INF
	if _latest_result_time_s > 0.0:
		age_s = maxf(Time.get_ticks_msec() * 0.001 - _latest_result_time_s, 0.0)
	return {
		"ready": ready_state,
		"failed": failed,
		"pending": _pending_records.size(),
		"in_flight": _dispatch_in_flight,
		"cache_entries": _cache.size(),
		"queries_dispatched": _queries_dispatched,
		"readbacks": _readbacks,
		"dropped_queries": _dropped_queries,
		"latest_age_s": age_s,
	}


func _sample_state(direction: Vector3) -> Vector4:
	if not ready_state or failed or not TerrainDeformationGPU.ready_state:
		return Vector4.ZERO
	if not bool(TerrainDeformationGPU.get("_has_active_content")):
		return Vector4.ZERO
	if direction.length_squared() <= 1e-12:
		return Vector4.ZERO
	var local_m: Vector2 = TerrainDeformationGPU.project_local(direction.normalized())
	if not is_finite(local_m.x) or not is_finite(local_m.y):
		return Vector4.ZERO
	var half_extent_m: float = float(TerrainDeformationGPU.HALF_EXTENT_M)
	if absf(local_m.x) >= half_extent_m or absf(local_m.y) >= half_extent_m:
		return Vector4.ZERO
	var key: int = _sample_key(local_m)
	var field_generation: int = TerrainDeformationGPU.field_generation()
	if _cache.has(key):
		var cached_value: Variant = _cache[key]
		var cached_generation: int = int(_cache_generation.get(key, -1))
		if cached_generation < field_generation:
			_queue_sample(local_m, key)
		if cached_value is Vector4:
			return cached_value as Vector4
	_queue_sample(local_m, key)
	return Vector4.ZERO


func _sample_key(local_m: Vector2) -> int:
	var qx: int = int(round(local_m.x * CACHE_QUANTIZATION_PER_M))
	var qy: int = int(round(local_m.y * CACHE_QUANTIZATION_PER_M))
	return (qy + 2048) * 8192 + (qx + 2048)


func _queue_sample(local_m: Vector2, key: int) -> void:
	if _pending_key_set.has(key):
		return
	if _pending_records.size() >= MAX_QUERIES:
		_dropped_queries += 1
		return
	_pending_records.append(PackedFloat32Array([local_m.x, local_m.y, 0.0, 0.0]))
	_pending_keys.append(key)
	_pending_key_set[key] = true


func _reset_cache_for_window(window_generation: int) -> void:
	_last_window_generation = window_generation
	_pending_records.clear()
	_pending_keys.clear()
	_pending_key_set.clear()
	_cache.clear()
	_cache_generation.clear()


func _dispatch_pending() -> void:
	if _pending_records.is_empty() or _uniform_sets.size() != 2:
		return
	var count: int = mini(_pending_records.size(), MAX_QUERIES)
	var packed := PackedFloat32Array()
	packed.resize(MAX_QUERIES * QUERY_FLOATS)
	for query_index in count:
		var record: PackedFloat32Array = _pending_records[query_index]
		for component in QUERY_FLOATS:
			packed[query_index * QUERY_FLOATS + component] = record[component]
	var keys := PackedInt64Array()
	keys.resize(count)
	for query_index in count:
		keys[query_index] = _pending_keys[query_index]
	_pending_records.clear()
	_pending_keys.clear()
	_pending_key_set.clear()
	var params := PackedFloat32Array([
		float(count), float(TerrainDeformationGPU.RESOLUTION),
		float(TerrainDeformationGPU.SAMPLE_SPACING_M),
		float(TerrainDeformationGPU.HALF_EXTENT_M),
	])
	var source_index: int = TerrainDeformationGPU.active_texture_index()
	if source_index < 0 or source_index >= _uniform_sets.size():
		return
	_dispatch_token += 1
	var token: int = _dispatch_token
	var window_generation: int = TerrainDeformationGPU.window_generation()
	var field_generation: int = TerrainDeformationGPU.field_generation()
	_dispatch_in_flight = true
	_queries_dispatched += count
	RenderingServer.call_on_render_thread(_render_dispatch.bind(
		token, window_generation, field_generation, count, keys,
		packed.to_byte_array(), params.to_byte_array(), _rd_pipeline,
		_uniform_sets[source_index], _rd_query_buffer, _rd_result_buffer,
		_rd_param_buffer))


func _render_dispatch(token: int, window_generation: int, field_generation: int,
		count: int, keys: PackedInt64Array, query_bytes: PackedByteArray,
		param_bytes: PackedByteArray, pipeline: RID, uniform_set: RID,
		query_buffer: RID, result_buffer: RID, param_buffer: RID) -> void:
	var rd: RenderingDevice = RenderingServer.get_rendering_device()
	if rd == null or not pipeline.is_valid() or not uniform_set.is_valid():
		call_deferred("_on_dispatch_failed", token)
		return
	if rd.buffer_update(query_buffer, 0, QUERY_BUFFER_BYTES, query_bytes) != OK:
		call_deferred("_on_dispatch_failed", token)
		return
	if rd.buffer_update(param_buffer, 0, PARAM_BYTES, param_bytes) != OK:
		call_deferred("_on_dispatch_failed", token)
		return
	var compute_list: int = rd.compute_list_begin()
	rd.compute_list_bind_compute_pipeline(compute_list, pipeline)
	rd.compute_list_bind_uniform_set(compute_list, uniform_set, 0)
	var groups: int = int(ceil(float(count) / 64.0))
	rd.compute_list_dispatch(compute_list, maxi(groups, 1), 1, 1)
	rd.compute_list_end()
	var result_bytes: int = count * BYTES_PER_QUERY
	var readback_error: Error = rd.buffer_get_data_async(result_buffer,
		_on_render_readback.bind(token, window_generation, field_generation, count, keys),
		0, result_bytes)
	if readback_error != OK:
		call_deferred("_on_dispatch_failed", token)


func _on_render_readback(data: PackedByteArray, token: int,
		window_generation: int, field_generation: int, count: int,
		keys: PackedInt64Array) -> void:
	call_deferred("_accept_readback", data, token, window_generation,
		field_generation, count, keys)


func _accept_readback(data: PackedByteArray, token: int,
		window_generation: int, field_generation: int, count: int,
		keys: PackedInt64Array) -> void:
	if token != _dispatch_token:
		return
	_dispatch_in_flight = false
	if window_generation != TerrainDeformationGPU.window_generation():
		return
	if data.size() < count * BYTES_PER_QUERY or keys.size() < count:
		return
	for query_index in count:
		var offset: int = query_index * BYTES_PER_QUERY
		var state: Vector4 = Vector4(
			data.decode_float(offset + 0), data.decode_float(offset + 4),
			data.decode_float(offset + 8), data.decode_float(offset + 12))
		var key: int = int(keys[query_index])
		_cache[key] = state
		_cache_generation[key] = field_generation
	if _cache.size() > CACHE_LIMIT:
		_cache.clear()
		_cache_generation.clear()
	_latest_result_time_s = Time.get_ticks_msec() * 0.001
	_readbacks += 1


func _on_dispatch_failed(token: int) -> void:
	if token == _dispatch_token:
		_dispatch_in_flight = false
	ready_state = false
	failed = true
	push_error("GPU terrain deformation point-query pass failed; full-texture fallback remains available.")


func _try_initialize() -> void:
	if _init_requested or ready_state or failed:
		return
	if not TerrainDeformationGPU.ready_state or TerrainDeformationGPU.failed:
		return
	var texture_rids: Array[RID] = TerrainDeformationGPU.rd_texture_rids()
	if texture_rids.size() != 2:
		return
	var resource: Resource = load(SHADER_PATH)
	if resource == null or not (resource is RDShaderFile):
		return
	var spirv: RDShaderSPIRV = (resource as RDShaderFile).get_spirv()
	if spirv == null or not spirv.compile_error_compute.is_empty() or spirv.bytecode_compute.is_empty():
		ready_state = false
		failed = true
		push_error("GPU terrain deformation sample shader is invalid.")
		return
	_init_requested = true
	RenderingServer.call_on_render_thread(_render_initialize.bind(spirv, texture_rids))


func _render_initialize(spirv: RDShaderSPIRV, texture_rids: Array[RID]) -> void:
	var rd: RenderingDevice = RenderingServer.get_rendering_device()
	if rd == null:
		call_deferred("_on_initialized", false, RID(), RID(), RID(), RID(), RID(), [])
		return
	var shader: RID = rd.shader_create_from_spirv(spirv, "Asterra terrain deformation point queries")
	if not shader.is_valid():
		call_deferred("_on_initialized", false, RID(), RID(), RID(), RID(), RID(), [])
		return
	var pipeline: RID = rd.compute_pipeline_create(shader)
	if not pipeline.is_valid() or not rd.compute_pipeline_is_valid(pipeline):
		rd.free_rid(shader)
		call_deferred("_on_initialized", false, RID(), RID(), RID(), RID(), RID(), [])
		return
	var zero_queries := PackedByteArray()
	zero_queries.resize(QUERY_BUFFER_BYTES)
	var zero_results := PackedByteArray()
	zero_results.resize(RESULT_BUFFER_BYTES)
	var zero_params := PackedByteArray()
	zero_params.resize(PARAM_BYTES)
	var query_buffer: RID = rd.storage_buffer_create(QUERY_BUFFER_BYTES, zero_queries)
	var result_buffer: RID = rd.storage_buffer_create(RESULT_BUFFER_BYTES, zero_results)
	var param_buffer: RID = rd.uniform_buffer_create(PARAM_BYTES, zero_params)
	var ok: bool = query_buffer.is_valid() and result_buffer.is_valid() and param_buffer.is_valid()
	var sets: Array[RID] = []
	if ok:
		for texture_index in 2:
			var uniforms: Array[RDUniform] = []
			var texture_uniform := RDUniform.new()
			texture_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_IMAGE
			texture_uniform.binding = 0
			texture_uniform.add_id(texture_rids[texture_index])
			uniforms.append(texture_uniform)
			var query_uniform := RDUniform.new()
			query_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
			query_uniform.binding = 1
			query_uniform.add_id(query_buffer)
			uniforms.append(query_uniform)
			var result_uniform := RDUniform.new()
			result_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
			result_uniform.binding = 2
			result_uniform.add_id(result_buffer)
			uniforms.append(result_uniform)
			var param_uniform := RDUniform.new()
			param_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_UNIFORM_BUFFER
			param_uniform.binding = 3
			param_uniform.add_id(param_buffer)
			uniforms.append(param_uniform)
			var uniform_set: RID = rd.uniform_set_create(uniforms, shader, 0)
			if not uniform_set.is_valid():
				ok = false
				break
			sets.append(uniform_set)
	call_deferred("_on_initialized", ok, shader, pipeline, query_buffer,
		result_buffer, param_buffer, sets)


func _on_initialized(success: bool, shader: RID, pipeline: RID,
		query_buffer: RID, result_buffer: RID, param_buffer: RID, sets: Array) -> void:
	if not success:
		ready_state = false
		failed = true
		push_error("GPU terrain deformation point-query initialization failed.")
		return
	_rd_shader = shader
	_rd_pipeline = pipeline
	_rd_query_buffer = query_buffer
	_rd_result_buffer = result_buffer
	_rd_param_buffer = param_buffer
	_uniform_sets.assign(sets)
	if _uniform_sets.size() != 2:
		ready_state = false
		failed = true
		return
	_last_window_generation = TerrainDeformationGPU.window_generation()
	ready_state = true
