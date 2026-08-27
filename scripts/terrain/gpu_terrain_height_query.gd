extends Node
## Batched asynchronous GPU terrain query service.
##
## The compute kernel remains the exact one-point pristine terrain evaluator, but
## a pool of independent parameter/result buffers lets gameplay submit many points
## in one frame (wheels, landing gear, foundations, ray probes, etc.). Mutable
## terrain deltas are added from Deltas on read, so gameplay and visible edits use
## the same persistent edit source without duplicating procedural synthesis on CPU.

const SHADER_PATH := "res://shaders/terrain_height_query.glsl"
const PARAM_BYTES := 32
const RESULT_BYTES := 16
const SLOT_COUNT := 24
const MAX_PENDING := 512
const MAX_CACHE_SAMPLES := 384
const MAX_CACHE_DISTANCE_M := 64.0
const MAX_CACHE_AGE_S := 0.55
const NORMAL_SAMPLE_M := 1.5
const DEDUPE_DISTANCE_M := 0.35

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
var _slot_param_buffers: Array[RID] = []
var _slot_result_buffers: Array[RID] = []
var _slot_uniform_sets: Array[RID] = []
var _slot_busy := PackedByteArray()
var _slot_tokens := PackedInt64Array()

var _pending: Array[Vector3] = []
var _next_token := 1
var _samples: Array[Dictionary] = []


func _ready() -> void:
	process_priority = -5
	_slot_busy.resize(SLOT_COUNT)
	_slot_tokens.resize(SLOT_COUNT)
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
	_dispatch_pending()
	_prune_samples()


func _on_world_ready(_fields: PlanetFields) -> void:
	_bindings_ready = false
	_binding_generation = -1
	_binding_macro_rid = RID()
	_pending.clear()
	_samples.clear()
	for i in SLOT_COUNT:
		_slot_busy[i] = 0
		_slot_tokens[i] = 0


func request_height(direction: Vector3) -> void:
	_enqueue(direction)


func request_batch(directions: Array[Vector3]) -> void:
	for d: Vector3 in directions:
		_enqueue(d)


func request_surface(direction: Vector3) -> void:
	if Planet.cfg == null or direction.length_squared() <= 1e-12:
		return
	var d := direction.normalized()
	var basis := _tangent_basis(d)
	var theta := NORMAL_SAMPLE_M / Planet.cfg.planet_radius
	_enqueue(d)
	_enqueue((d + basis[0] * theta).normalized())
	_enqueue((d + basis[1] * theta).normalized())


func request_surfaces(directions: Array[Vector3]) -> void:
	for d: Vector3 in directions:
		request_surface(d)


func height_for_direction(direction: Vector3, fallback: float) -> float:
	var pristine := pristine_height_for_direction(direction, NAN)
	if is_nan(pristine):
		return fallback
	return pristine + Deltas.offset_at(direction.normalized())


func pristine_height_for_direction(direction: Vector3, fallback: float = NAN) -> float:
	if Planet.cfg == null or direction.length_squared() <= 1e-12:
		return fallback
	var found := _find_sample(direction.normalized(), MAX_CACHE_DISTANCE_M, MAX_CACHE_AGE_S)
	if found.is_empty():
		return fallback
	return float(found["height"])


func surface_for_direction(direction: Vector3, fallback_height: float) -> Dictionary:
	if Planet.cfg == null or direction.length_squared() <= 1e-12:
		return {"height": fallback_height, "normal": direction.normalized(), "precise": false}
	var d := direction.normalized()
	request_surface(d)
	var basis := _tangent_basis(d)
	var theta := NORMAL_SAMPLE_M / Planet.cfg.planet_radius
	var dx := (d + basis[0] * theta).normalized()
	var dy := (d + basis[1] * theta).normalized()
	var s0 := _find_sample(d, MAX_CACHE_DISTANCE_M, MAX_CACHE_AGE_S)
	var sx := _find_sample(dx, MAX_CACHE_DISTANCE_M, MAX_CACHE_AGE_S)
	var sy := _find_sample(dy, MAX_CACHE_DISTANCE_M, MAX_CACHE_AGE_S)
	if s0.is_empty():
		return {"height": fallback_height, "normal": d, "precise": false}
	var h0 := float(s0["height"]) + Deltas.offset_at(d)
	if sx.is_empty() or sy.is_empty():
		return {"height": h0, "normal": d, "precise": true}
	var hx := float(sx["height"]) + Deltas.offset_at(dx)
	var hy := float(sy["height"]) + Deltas.offset_at(dy)
	var p0 := d * (Planet.cfg.planet_radius + h0)
	var px := dx * (Planet.cfg.planet_radius + hx)
	var py := dy * (Planet.cfg.planet_radius + hy)
	var n := (px - p0).cross(py - p0)
	if n.length_squared() <= 1e-10:
		n = d
	else:
		n = n.normalized()
		if n.dot(d) < 0.0:
			n = -n
	return {"height": h0, "normal": n, "precise": true}


func has_fresh_height(direction: Vector3) -> bool:
	if direction.length_squared() <= 1e-12:
		return false
	return not _find_sample(direction.normalized(), MAX_CACHE_DISTANCE_M, MAX_CACHE_AGE_S).is_empty()


func _enqueue(direction: Vector3) -> void:
	if direction.length_squared() <= 1e-12 or Planet.cfg == null:
		return
	var d := direction.normalized()
	if not _find_sample(d, DEDUPE_DISTANCE_M, 0.08).is_empty():
		return
	for queued: Vector3 in _pending:
		var dist := acos(clampf(queued.dot(d), -1.0, 1.0)) * Planet.cfg.planet_radius
		if dist <= DEDUPE_DISTANCE_M:
			return
	if _pending.size() >= MAX_PENDING:
		_pending.pop_front()
	_pending.append(d)


func _find_sample(direction: Vector3, max_distance_m: float, max_age_s: float) -> Dictionary:
	if Planet.cfg == null:
		return {}
	var now := Time.get_ticks_msec() * 0.001
	var best := {}
	var best_distance := INF
	for sample: Dictionary in _samples:
		var age := now - float(sample["time"])
		if age > max_age_s:
			continue
		var sd: Vector3 = sample["dir"]
		var distance := acos(clampf(sd.dot(direction), -1.0, 1.0)) * Planet.cfg.planet_radius
		if distance <= max_distance_m and distance < best_distance:
			best_distance = distance
			best = sample
	return best


func _prune_samples() -> void:
	var now := Time.get_ticks_msec() * 0.001
	for i in range(_samples.size() - 1, -1, -1):
		if now - float(_samples[i]["time"]) > 2.0:
			_samples.remove_at(i)
	while _samples.size() > MAX_CACHE_SAMPLES:
		_samples.pop_front()


func _tangent_basis(d: Vector3) -> Array[Vector3]:
	var ref := Vector3.UP
	if absf(d.dot(ref)) > 0.995:
		ref = Vector3.RIGHT
	var right := ref.cross(d).normalized()
	var up := d.cross(right).normalized()
	return [right, up]


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
	var rd := RenderingServer.get_rendering_device()
	if rd == null:
		call_deferred("_on_initialized", false, RID(), RID(), RID(), [], [])
		return
	var shader := rd.shader_create_from_spirv(spirv, "Asterra batched terrain queries")
	if not shader.is_valid():
		call_deferred("_on_initialized", false, RID(), RID(), RID(), [], [])
		return
	var pipeline := rd.compute_pipeline_create(shader)
	if not pipeline.is_valid() or not rd.compute_pipeline_is_valid(pipeline):
		rd.free_rid(shader)
		call_deferred("_on_initialized", false, RID(), RID(), RID(), [], [])
		return
	var sampler := rd.sampler_create(RDSamplerState.new())
	var params: Array[RID] = []
	var results: Array[RID] = []
	for _i in SLOT_COUNT:
		var pzero := PackedByteArray()
		pzero.resize(PARAM_BYTES)
		var rzero := PackedByteArray()
		rzero.resize(RESULT_BYTES)
		params.append(rd.uniform_buffer_create(PARAM_BYTES, pzero))
		results.append(rd.storage_buffer_create(RESULT_BYTES, rzero))
	var ok := sampler.is_valid()
	for rid: RID in params:
		ok = ok and rid.is_valid()
	for rid: RID in results:
		ok = ok and rid.is_valid()
	call_deferred("_on_initialized", ok, shader, pipeline, sampler, params, results)


func _on_initialized(success: bool, shader: RID, pipeline: RID, sampler: RID,
		params: Array, results: Array) -> void:
	if not success:
		failed = true
		return
	_rd_shader = shader
	_rd_pipeline = pipeline
	_rd_sampler = sampler
	_slot_param_buffers.assign(params)
	_slot_result_buffers.assign(results)
	ready_state = true
	failed = false
	_bindings_ready = false


func _ensure_bindings() -> bool:
	if not ready_state or Planet.cfg == null or not Planet.ready_state:
		return false
	var context := get_node_or_null("/root/PlanetContext")
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
	var textures: Array = [macro, context.get("soil_texture"), context.get("surface_texture"),
		context.get("geology_texture"), context.get("structure_texture"),
		context.get("climate_texture"), context.get("hydrology_texture")]
	var server_rids: Array[RID] = []
	for value: Variant in textures:
		if not (value is Texture2DArray):
			return false
		server_rids.append((value as Texture2DArray).get_rid())
	_bindings_building = true
	RenderingServer.call_on_render_thread(_render_build_uniform_sets.bind(
		generation, macro_rid, server_rids, _rd_shader, _rd_sampler,
		_slot_result_buffers, _slot_param_buffers))
	return false


func _render_build_uniform_sets(generation: int, macro_rid: RID, server_rids: Array,
		shader: RID, sampler: RID, result_buffers: Array, param_buffers: Array) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null:
		call_deferred("_on_uniform_sets_built", false, generation, macro_rid, [])
		return
	var texture_rids: Array[RID] = []
	for server_rid: RID in server_rids:
		var rd_tex := RenderingServer.texture_get_rd_texture(server_rid, false)
		if not rd_tex.is_valid():
			call_deferred("_on_uniform_sets_built", false, generation, macro_rid, [])
			return
		texture_rids.append(rd_tex)
	var sets: Array[RID] = []
	for slot in SLOT_COUNT:
		var uniforms: Array[RDUniform] = []
		for binding in 7:
			var u := RDUniform.new()
			u.uniform_type = RenderingDevice.UNIFORM_TYPE_SAMPLER_WITH_TEXTURE
			u.binding = binding
			u.add_id(sampler)
			u.add_id(texture_rids[binding])
			uniforms.append(u)
		var result_u := RDUniform.new()
		result_u.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
		result_u.binding = 7
		result_u.add_id(result_buffers[slot])
		uniforms.append(result_u)
		var params_u := RDUniform.new()
		params_u.uniform_type = RenderingDevice.UNIFORM_TYPE_UNIFORM_BUFFER
		params_u.binding = 8
		params_u.add_id(param_buffers[slot])
		uniforms.append(params_u)
		var set := rd.uniform_set_create(uniforms, shader, 0)
		if not set.is_valid():
			call_deferred("_on_uniform_sets_built", false, generation, macro_rid, [])
			return
		sets.append(set)
	call_deferred("_on_uniform_sets_built", true, generation, macro_rid, sets)


func _on_uniform_sets_built(success: bool, generation: int, macro_rid: RID, sets: Array) -> void:
	_bindings_building = false
	if not success:
		_bindings_ready = false
		return
	_slot_uniform_sets.assign(sets)
	_binding_generation = generation
	_binding_macro_rid = macro_rid
	_bindings_ready = true


func _dispatch_pending() -> void:
	if _pending.is_empty() or _slot_uniform_sets.size() != SLOT_COUNT:
		return
	var context := get_node_or_null("/root/PlanetContext")
	if context == null:
		return
	var seed := Planet.cfg.stream_seed("gpu_visual_detail") & 0x00ffffff
	for slot in SLOT_COUNT:
		if _pending.is_empty():
			break
		if _slot_busy[slot] != 0:
			continue
		var d := _pending.pop_front()
		var token := _next_token
		_next_token += 1
		_slot_busy[slot] = 1
		_slot_tokens[slot] = token
		var values := PackedFloat32Array([d.x, d.y, d.z, Planet.cfg.planet_radius,
			float(Planet.global_height_face_res), float(context.get("face_res")),
			float(maxi(seed, 1)), 0.75])
		RenderingServer.call_on_render_thread(_render_dispatch_slot.bind(slot, token, d,
			values.to_byte_array(), _rd_pipeline, _slot_uniform_sets[slot],
			_slot_param_buffers[slot], _slot_result_buffers[slot]))


func _render_dispatch_slot(slot: int, token: int, direction: Vector3, params: PackedByteArray,
		pipeline: RID, uniform_set: RID, param_buffer: RID, result_buffer: RID) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null or not pipeline.is_valid() or not uniform_set.is_valid():
		call_deferred("_on_dispatch_failed", slot, token)
		return
	if rd.buffer_update(param_buffer, 0, PARAM_BYTES, params) != OK:
		call_deferred("_on_dispatch_failed", slot, token)
		return
	var list := rd.compute_list_begin()
	rd.compute_list_bind_compute_pipeline(list, pipeline)
	rd.compute_list_bind_uniform_set(list, uniform_set, 0)
	rd.compute_list_dispatch(list, 1, 1, 1)
	rd.compute_list_end()
	var err := rd.buffer_get_data_async(result_buffer,
		_on_render_readback.bind(slot, token, direction), 0, RESULT_BYTES)
	if err != OK:
		call_deferred("_on_dispatch_failed", slot, token)


func _on_render_readback(data: PackedByteArray, slot: int, token: int, direction: Vector3) -> void:
	var h := data.decode_float(0) if data.size() >= 4 else NAN
	call_deferred("_accept_height", slot, token, direction, h)


func _accept_height(slot: int, token: int, direction: Vector3, height: float) -> void:
	if slot < 0 or slot >= SLOT_COUNT or _slot_tokens[slot] != token:
		return
	_slot_busy[slot] = 0
	_slot_tokens[slot] = 0
	if not is_finite(height):
		return
	_samples.append({"dir": direction.normalized(), "height": height,
		"time": Time.get_ticks_msec() * 0.001})


func _on_dispatch_failed(slot: int, token: int) -> void:
	if slot >= 0 and slot < SLOT_COUNT and _slot_tokens[slot] == token:
		_slot_busy[slot] = 0
		_slot_tokens[slot] = 0


func stats() -> Dictionary:
	var busy := 0
	for value in _slot_busy:
		busy += 1 if value != 0 else 0
	return {"supported": supported, "ready": ready_state, "failed": failed,
		"bindings_ready": _bindings_ready, "in_flight": busy,
		"pending": _pending.size(), "cached_samples": _samples.size(),
		"query_slots": SLOT_COUNT, "cpu_procedural_detail": false,
		"async_readback": true, "batched": true}
