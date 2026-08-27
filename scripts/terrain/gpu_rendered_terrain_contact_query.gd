extends Node
## Batched asynchronous contact query against the exact production clipmap stack.
##
## Unlike TerrainHeightQuery, this service does not regenerate procedural terrain.
## It samples the active clipmap cache used by the vertex shader, validates the same
## toroidal cache keys, applies the same LOD morph, then samples the exact persistent
## and live deformation textures currently bound for rendering. It is intended for
## first-contact acquisition and other cases where visual/physical agreement is
## more important than accepting an approximate nearby height.

const SHADER_PATH := "res://shaders/terrain_rendered_contact_query.glsl"
const MAX_QUERIES := 128
const FLOATS_PER_QUERY := 4
const BYTES_PER_QUERY := FLOATS_PER_QUERY * 4
const QUERY_BUFFER_BYTES := MAX_QUERIES * BYTES_PER_QUERY
const RESULT_BUFFER_BYTES := MAX_QUERIES * BYTES_PER_QUERY
const PARAM_FLOATS := 48
const PARAM_BYTES := PARAM_FLOATS * 4
const SAMPLE_MATCH_DISTANCE_M := 0.20
const SAMPLE_MAX_AGE_S := 0.40
const DEDUPE_DISTANCE_M := 0.12
const MAX_CACHE_SAMPLES := 256

var supported := false
var ready_state := false
var failed := false

var _init_requested := false
var _bindings_building := false
var _bindings_ready := false
var _dispatch_in_flight := false
var _dispatch_token := 0
var _source_serial := 1

var _rd_shader := RID()
var _rd_pipeline := RID()
var _rd_sampler := RID()
var _rd_query_buffer := RID()
var _rd_result_buffer := RID()
var _rd_param_buffer := RID()
var _uniform_sets: Array[RID] = []

var _bound_cache_server_rid := RID()
var _bound_edit_server_rid := RID()
var _bound_active_rids: Array[RID] = []

var _last_cache_generation := -1
var _last_edit_generation := -1
var _last_active_generation := -1
var _last_active_window_generation := -1
var _last_active_index := -1
var _last_active_min := -1
var _last_active_max := -1
var _last_center_plane := Vector2(INF, INF)
var _last_cache_server_rid := RID()
var _last_edit_server_rid := RID()

var _pending: Array[Vector3] = []
var _samples: Array[Dictionary] = []
var _last_snapshot: Dictionary = {}
var _queries_dispatched := 0
var _readbacks := 0
var _invalid_cache_results := 0


func _ready() -> void:
	process_priority = 4
	var method: String = RenderingServer.get_current_rendering_method()
	supported = method == "forward_plus" or method == "mobile"
	if supported:
		call_deferred("_try_initialize")


func _process(_dt: float) -> void:
	if not supported or failed:
		return
	if not ready_state:
		_try_initialize()
		return
	var snapshot: Dictionary = _capture_snapshot()
	if snapshot.is_empty():
		return
	_last_snapshot = snapshot
	_update_source_serial(snapshot)
	if not _ensure_bindings(snapshot):
		return
	if not _pending.is_empty() and not _dispatch_in_flight:
		_dispatch_pending(snapshot)
	_prune_samples()


func request_height(direction: Vector3) -> void:
	if direction.length_squared() <= 1e-12:
		return
	var d: Vector3 = direction.normalized()
	if _find_sample(d).is_empty():
		_enqueue(d)


func request_batch(directions: Array[Vector3]) -> void:
	for direction: Vector3 in directions:
		request_height(direction)


func height_for_direction(direction: Vector3, fallback: float = NAN) -> float:
	if direction.length_squared() <= 1e-12:
		return fallback
	var d: Vector3 = direction.normalized()
	var sample: Dictionary = _find_sample(d)
	if sample.is_empty():
		_enqueue(d)
		return fallback
	return float(sample["height"])


func has_height(direction: Vector3) -> bool:
	if direction.length_squared() <= 1e-12:
		return false
	return not _find_sample(direction.normalized()).is_empty()


func sample_info(direction: Vector3) -> Dictionary:
	if direction.length_squared() <= 1e-12:
		return {}
	return _find_sample(direction.normalized()).duplicate(true)


func stats() -> Dictionary:
	return {
		"supported": supported,
		"ready": ready_state,
		"failed": failed,
		"bindings_ready": _bindings_ready,
		"in_flight": _dispatch_in_flight,
		"pending": _pending.size(),
		"cached_samples": _samples.size(),
		"source_serial": _source_serial,
		"queries_dispatched": _queries_dispatched,
		"readbacks": _readbacks,
		"invalid_cache_results": _invalid_cache_results,
		"exact_render_stack": true,
	}


func _capture_snapshot() -> Dictionary:
	if Planet.cfg == null or not Planet.ready_state:
		return {}
	var terrain: Node = get_node_or_null("/root/GroundGeometryClipmap")
	if terrain == null or not terrain.has_method("rendered_contact_sample_params"):
		return {}
	var render_value: Variant = terrain.call("rendered_contact_sample_params")
	if not (render_value is Dictionary):
		return {}
	var render_params: Dictionary = render_value
	if render_params.is_empty() or not bool(render_params.get("cache_ready", false)):
		return {}
	var cache_value: Variant = render_params.get("cache_texture", null)
	if not (cache_value is Texture2DArray):
		return {}

	var edits: Node = get_node_or_null("/root/TerrainEditDeltaGPU")
	var active: Node = get_node_or_null("/root/TerrainDeformationGPU")
	if edits == null or active == null or not edits.has_method("sample_params") \
			or not active.has_method("sample_params"):
		return {}
	var edit_value: Variant = edits.call("sample_params")
	var active_value: Variant = active.call("sample_params")
	if not (edit_value is Dictionary) or not (active_value is Dictionary):
		return {}
	var edit_params: Dictionary = edit_value
	var active_params: Dictionary = active_value
	var edit_texture_value: Variant = edit_params.get("texture", null)
	var active_texture_value: Variant = active_params.get("texture", null)
	if not (edit_texture_value is Texture2D) or not (active_texture_value is Texture2D):
		return {}
	var active_rids: Array[RID] = active.call("rd_texture_rids")
	if active_rids.size() != 2:
		return {}

	return {
		"render": render_params,
		"edit": edit_params,
		"active": active_params,
		"cache_texture": cache_value as Texture2DArray,
		"edit_texture": edit_texture_value as Texture2D,
		"active_texture": active_texture_value as Texture2D,
		"active_rids": active_rids,
	}


func _update_source_serial(snapshot: Dictionary) -> void:
	var render_params: Dictionary = snapshot["render"]
	var edit_params: Dictionary = snapshot["edit"]
	var active_params: Dictionary = snapshot["active"]
	var cache_texture: Texture2DArray = snapshot["cache_texture"]
	var edit_texture: Texture2D = snapshot["edit_texture"]
	var cache_rid: RID = cache_texture.get_rid()
	var edit_rid: RID = edit_texture.get_rid()
	var center_plane: Vector2 = render_params.get("lattice_center_plane", Vector2.ZERO)
	var cache_generation: int = int(render_params.get("cache_generation", -1))
	var edit_generation: int = int(edit_params.get("generation", -1))
	var active_generation: int = int(active_params.get("generation", -1))
	var active_window_generation: int = int(active_params.get("window_generation", -1))
	var active_index: int = int(active_params.get("active_index", -1))
	var active_min: int = int(render_params.get("active_min", -1))
	var active_max: int = int(render_params.get("active_max", -1))
	var changed: bool = cache_generation != _last_cache_generation \
		or edit_generation != _last_edit_generation \
		or active_generation != _last_active_generation \
		or active_window_generation != _last_active_window_generation \
		or active_index != _last_active_index \
		or active_min != _last_active_min or active_max != _last_active_max \
		or center_plane.distance_squared_to(_last_center_plane) > 1e-10 \
		or cache_rid != _last_cache_server_rid or edit_rid != _last_edit_server_rid
	if not changed:
		return
	_source_serial += 1
	_last_cache_generation = cache_generation
	_last_edit_generation = edit_generation
	_last_active_generation = active_generation
	_last_active_window_generation = active_window_generation
	_last_active_index = active_index
	_last_active_min = active_min
	_last_active_max = active_max
	_last_center_plane = center_plane
	_last_cache_server_rid = cache_rid
	_last_edit_server_rid = edit_rid


func _enqueue(direction: Vector3) -> void:
	if Planet.cfg == null:
		return
	for queued: Vector3 in _pending:
		var queued_distance_m: float = acos(clampf(queued.dot(direction), -1.0, 1.0)) \
			* Planet.cfg.planet_radius
		if queued_distance_m <= DEDUPE_DISTANCE_M:
			return
	if _pending.size() >= MAX_QUERIES:
		_pending.pop_front()
	_pending.append(direction)


func _find_sample(direction: Vector3) -> Dictionary:
	if Planet.cfg == null:
		return {}
	var now_s: float = Time.get_ticks_msec() * 0.001
	var best: Dictionary = {}
	var best_distance_m := INF
	for sample: Dictionary in _samples:
		if int(sample.get("serial", -1)) != _source_serial:
			continue
		if now_s - float(sample.get("time", 0.0)) > SAMPLE_MAX_AGE_S:
			continue
		var sample_dir_value: Variant = sample.get("dir", Vector3.ZERO)
		if not (sample_dir_value is Vector3):
			continue
		var sample_dir: Vector3 = sample_dir_value as Vector3
		var distance_m: float = acos(clampf(sample_dir.dot(direction), -1.0, 1.0)) \
			* Planet.cfg.planet_radius
		if distance_m <= SAMPLE_MATCH_DISTANCE_M and distance_m < best_distance_m:
			best_distance_m = distance_m
			best = sample
	return best


func _prune_samples() -> void:
	var now_s: float = Time.get_ticks_msec() * 0.001
	for index: int in range(_samples.size() - 1, -1, -1):
		var sample: Dictionary = _samples[index]
		if int(sample.get("serial", -1)) != _source_serial \
				or now_s - float(sample.get("time", 0.0)) > 1.0:
			_samples.remove_at(index)
	while _samples.size() > MAX_CACHE_SAMPLES:
		_samples.pop_front()


func _ensure_bindings(snapshot: Dictionary) -> bool:
	var cache_texture: Texture2DArray = snapshot["cache_texture"]
	var edit_texture: Texture2D = snapshot["edit_texture"]
	var cache_server_rid: RID = cache_texture.get_rid()
	var edit_server_rid: RID = edit_texture.get_rid()
	var active_rids: Array[RID] = snapshot["active_rids"]
	var same_active: bool = active_rids.size() == _bound_active_rids.size()
	if same_active:
		for index: int in active_rids.size():
			if active_rids[index] != _bound_active_rids[index]:
				same_active = false
				break
	if _bindings_ready and cache_server_rid == _bound_cache_server_rid \
			and edit_server_rid == _bound_edit_server_rid and same_active:
		return true
	if _bindings_building:
		return false
	_bindings_building = true
	RenderingServer.call_on_render_thread(_render_build_uniform_sets.bind(
		cache_server_rid, edit_server_rid, active_rids, _rd_shader, _rd_sampler,
		_rd_query_buffer, _rd_result_buffer, _rd_param_buffer))
	return false


func _render_build_uniform_sets(cache_server_rid: RID, edit_server_rid: RID,
		active_rids: Array[RID], shader: RID, sampler: RID, query_buffer: RID,
		result_buffer: RID, param_buffer: RID) -> void:
	var rd: RenderingDevice = RenderingServer.get_rendering_device()
	if rd == null or active_rids.size() != 2:
		call_deferred("_on_uniform_sets_built", false, cache_server_rid,
			edit_server_rid, active_rids, [])
		return
	var cache_rd: RID = RenderingServer.texture_get_rd_texture(cache_server_rid, false)
	var edit_rd: RID = RenderingServer.texture_get_rd_texture(edit_server_rid, false)
	if not cache_rd.is_valid() or not edit_rd.is_valid():
		call_deferred("_on_uniform_sets_built", false, cache_server_rid,
			edit_server_rid, active_rids, [])
		return
	var sets: Array[RID] = []
	for active_index: int in 2:
		if not active_rids[active_index].is_valid():
			call_deferred("_on_uniform_sets_built", false, cache_server_rid,
				edit_server_rid, active_rids, [])
			return
		var uniforms: Array[RDUniform] = []
		var cache_uniform := RDUniform.new()
		cache_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_SAMPLER_WITH_TEXTURE
		cache_uniform.binding = 0
		cache_uniform.add_id(sampler)
		cache_uniform.add_id(cache_rd)
		uniforms.append(cache_uniform)
		var edit_uniform := RDUniform.new()
		edit_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_SAMPLER_WITH_TEXTURE
		edit_uniform.binding = 1
		edit_uniform.add_id(sampler)
		edit_uniform.add_id(edit_rd)
		uniforms.append(edit_uniform)
		var active_uniform := RDUniform.new()
		active_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_SAMPLER_WITH_TEXTURE
		active_uniform.binding = 2
		active_uniform.add_id(sampler)
		active_uniform.add_id(active_rids[active_index])
		uniforms.append(active_uniform)
		var query_uniform := RDUniform.new()
		query_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
		query_uniform.binding = 3
		query_uniform.add_id(query_buffer)
		uniforms.append(query_uniform)
		var result_uniform := RDUniform.new()
		result_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
		result_uniform.binding = 4
		result_uniform.add_id(result_buffer)
		uniforms.append(result_uniform)
		var param_uniform := RDUniform.new()
		param_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_UNIFORM_BUFFER
		param_uniform.binding = 5
		param_uniform.add_id(param_buffer)
		uniforms.append(param_uniform)
		var uniform_set: RID = rd.uniform_set_create(uniforms, shader, 0)
		if not uniform_set.is_valid():
			call_deferred("_on_uniform_sets_built", false, cache_server_rid,
				edit_server_rid, active_rids, [])
			return
		sets.append(uniform_set)
	call_deferred("_on_uniform_sets_built", true, cache_server_rid,
		edit_server_rid, active_rids, sets)


func _on_uniform_sets_built(success: bool, cache_server_rid: RID,
		edit_server_rid: RID, active_rids: Array, sets: Array) -> void:
	_bindings_building = false
	if not success:
		_bindings_ready = false
		return
	_uniform_sets.assign(sets)
	_bound_cache_server_rid = cache_server_rid
	_bound_edit_server_rid = edit_server_rid
	_bound_active_rids.assign(active_rids)
	_bindings_ready = _uniform_sets.size() == 2


func _dispatch_pending(snapshot: Dictionary) -> void:
	if _pending.is_empty() or _uniform_sets.size() != 2:
		return
	var count: int = mini(_pending.size(), MAX_QUERIES)
	var directions: Array[Vector3] = []
	var packed := PackedFloat32Array()
	packed.resize(MAX_QUERIES * FLOATS_PER_QUERY)
	for query_index: int in count:
		var d: Vector3 = _pending[query_index]
		directions.append(d)
		var base: int = query_index * FLOATS_PER_QUERY
		packed[base] = d.x
		packed[base + 1] = d.y
		packed[base + 2] = d.z
		packed[base + 3] = 0.0
	_pending.clear()

	var render_params: Dictionary = snapshot["render"]
	var edit_params: Dictionary = snapshot["edit"]
	var active_params: Dictionary = snapshot["active"]
	var anchor_dir: Vector3 = render_params.get("anchor_dir", Vector3.RIGHT)
	var anchor_right: Vector3 = render_params.get("anchor_right", Vector3.BACK)
	var anchor_up: Vector3 = render_params.get("anchor_up", Vector3.UP)
	var center_plane: Vector2 = render_params.get("lattice_center_plane", Vector2.ZERO)
	var edit_center: Vector3 = edit_params.get("center_dir", Vector3.RIGHT)
	var edit_right: Vector3 = edit_params.get("center_right", Vector3.BACK)
	var edit_up: Vector3 = edit_params.get("center_up", Vector3.UP)
	var active_center: Vector3 = active_params.get("center_dir", Vector3.RIGHT)
	var active_right: Vector3 = active_params.get("center_right", Vector3.BACK)
	var active_up: Vector3 = active_params.get("center_up", Vector3.UP)

	var values := PackedFloat32Array([
		float(count), Planet.cfg.planet_radius,
		float(render_params.get("base_spacing", 0.75)),
		float(render_params.get("grid_cells", 400.0)),
		anchor_dir.x, anchor_dir.y, anchor_dir.z,
		float(render_params.get("cache_generation", 1)),
		anchor_right.x, anchor_right.y, anchor_right.z,
		1.0 if bool(render_params.get("cache_ready", false)) else 0.0,
		anchor_up.x, anchor_up.y, anchor_up.z,
		float(render_params.get("surface_bias", 0.035)),
		center_plane.x, center_plane.y,
		float(render_params.get("active_min", 0)),
		float(render_params.get("active_max", 14)),
		edit_center.x, edit_center.y, edit_center.z,
		1.0 if bool(edit_params.get("ready", false)) else 0.0,
		edit_right.x, edit_right.y, edit_right.z,
		float(edit_params.get("half_extent_m", 256.0)),
		edit_up.x, edit_up.y, edit_up.z, 0.0,
		active_center.x, active_center.y, active_center.z,
		1.0 if bool(active_params.get("ready", false)) else 0.0,
		active_right.x, active_right.y, active_right.z,
		float(active_params.get("half_extent_m", 32.0)),
		active_up.x, active_up.y, active_up.z, 0.0,
		float(render_params.get("cache_res", 512)), 0.0, 0.0, 0.0,
	])
	if values.size() != PARAM_FLOATS:
		return
	var active_index: int = clampi(int(active_params.get("active_index", 0)), 0, 1)
	_dispatch_token += 1
	var token: int = _dispatch_token
	var serial: int = _source_serial
	_dispatch_in_flight = true
	_queries_dispatched += count
	RenderingServer.call_on_render_thread(_render_dispatch.bind(
		token, serial, count, directions, packed.to_byte_array(), values.to_byte_array(),
		_rd_pipeline, _uniform_sets[active_index], _rd_query_buffer,
		_rd_result_buffer, _rd_param_buffer))


func _render_dispatch(token: int, serial: int, count: int,
		directions: Array[Vector3], query_bytes: PackedByteArray,
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
	var groups: int = maxi(int(ceil(float(count) / 64.0)), 1)
	rd.compute_list_dispatch(compute_list, groups, 1, 1)
	rd.compute_list_end()
	var result_bytes: int = count * BYTES_PER_QUERY
	var readback_error: Error = rd.buffer_get_data_async(result_buffer,
		_on_render_readback.bind(token, serial, count, directions), 0, result_bytes)
	if readback_error != OK:
		call_deferred("_on_dispatch_failed", token)


func _on_render_readback(data: PackedByteArray, token: int, serial: int,
		count: int, directions: Array[Vector3]) -> void:
	call_deferred("_accept_readback", data, token, serial, count, directions)


func _accept_readback(data: PackedByteArray, token: int, serial: int,
		count: int, directions: Array[Vector3]) -> void:
	if token != _dispatch_token:
		return
	_dispatch_in_flight = false
	if serial != _source_serial or data.size() < count * BYTES_PER_QUERY:
		return
	var now_s: float = Time.get_ticks_msec() * 0.001
	for query_index: int in count:
		var offset: int = query_index * BYTES_PER_QUERY
		var height: float = data.decode_float(offset)
		var valid: float = data.decode_float(offset + 4)
		var level: float = data.decode_float(offset + 8)
		var morph: float = data.decode_float(offset + 12)
		if valid < 0.5 or not is_finite(height):
			_invalid_cache_results += 1
			continue
		_samples.append({
			"dir": directions[query_index].normalized(),
			"height": height,
			"level": int(round(level)),
			"morph": morph,
			"serial": serial,
			"time": now_s,
		})
	_readbacks += 1


func _on_dispatch_failed(token: int) -> void:
	if token == _dispatch_token:
		_dispatch_in_flight = false
	failed = true
	push_error("Exact rendered terrain contact query dispatch failed.")


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
		push_error("Exact rendered terrain contact query shader is invalid.")
		return
	_init_requested = true
	RenderingServer.call_on_render_thread(_render_initialize.bind(spirv))


func _render_initialize(spirv: RDShaderSPIRV) -> void:
	var rd: RenderingDevice = RenderingServer.get_rendering_device()
	if rd == null:
		call_deferred("_on_initialized", false, RID(), RID(), RID(), RID(), RID(), RID())
		return
	var shader: RID = rd.shader_create_from_spirv(spirv,
		"Asterra exact rendered terrain contact query")
	if not shader.is_valid():
		call_deferred("_on_initialized", false, RID(), RID(), RID(), RID(), RID(), RID())
		return
	var pipeline: RID = rd.compute_pipeline_create(shader)
	if not pipeline.is_valid() or not rd.compute_pipeline_is_valid(pipeline):
		rd.free_rid(shader)
		call_deferred("_on_initialized", false, RID(), RID(), RID(), RID(), RID(), RID())
		return
	var sampler_state := RDSamplerState.new()
	sampler_state.min_filter = RenderingDevice.SAMPLER_FILTER_LINEAR
	sampler_state.mag_filter = RenderingDevice.SAMPLER_FILTER_LINEAR
	sampler_state.mip_filter = RenderingDevice.SAMPLER_FILTER_LINEAR
	sampler_state.repeat_u = RenderingDevice.SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE
	sampler_state.repeat_v = RenderingDevice.SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE
	sampler_state.repeat_w = RenderingDevice.SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE
	var sampler: RID = rd.sampler_create(sampler_state)
	var zero_queries := PackedByteArray()
	zero_queries.resize(QUERY_BUFFER_BYTES)
	var zero_results := PackedByteArray()
	zero_results.resize(RESULT_BUFFER_BYTES)
	var zero_params := PackedByteArray()
	zero_params.resize(PARAM_BYTES)
	var query_buffer: RID = rd.storage_buffer_create(QUERY_BUFFER_BYTES, zero_queries)
	var result_buffer: RID = rd.storage_buffer_create(RESULT_BUFFER_BYTES, zero_results)
	var param_buffer: RID = rd.uniform_buffer_create(PARAM_BYTES, zero_params)
	var ok: bool = sampler.is_valid() and query_buffer.is_valid() \
		and result_buffer.is_valid() and param_buffer.is_valid()
	call_deferred("_on_initialized", ok, shader, pipeline, sampler,
		query_buffer, result_buffer, param_buffer)


func _on_initialized(success: bool, shader: RID, pipeline: RID, sampler: RID,
		query_buffer: RID, result_buffer: RID, param_buffer: RID) -> void:
	if not success:
		failed = true
		ready_state = false
		return
	_rd_shader = shader
	_rd_pipeline = pipeline
	_rd_sampler = sampler
	_rd_query_buffer = query_buffer
	_rd_result_buffer = result_buffer
	_rd_param_buffer = param_buffer
	ready_state = true
	failed = false
	_bindings_ready = false
