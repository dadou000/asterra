extends Node
class_name GPUTerrainClipmapCache
## Incremental GPU cache for the analytic terrain field.
##
## Each LOD owns one 512x512 layer in a single RGBA32F texture array. World
## lattice cells map directly to toroidal texels with a power-of-two modulo, so
## moving the clipmap never copies texture data. Newly exposed strips are the only
## cells queued after movement. Full fills are split into small center-first jobs.
##
## R = final procedural height, G = macro height, B = exact 24-bit validity key.
## The validity key includes logical cell, LOD and anchor generation. The render
## shader therefore falls back to the analytic function only for cells which have
## not been synthesized yet; stale cache data can never create terrain holes.

const SHADER_PATH := "res://shaders/terrain_clipmap_cache.glsl"
const CACHE_RES := 512
const CACHE_MASK := CACHE_RES - 1
const LEVEL_COUNT := 15
const LOCAL_SIZE := 8

# A compute job is intentionally small. The frame scheduler rotates through LODs
# and stops at a fixed sample budget so initial warm-up cannot become one giant GPU
# spike. Exposed movement strips use the same budget and have priority over warm-up.
const MAX_JOB_SAMPLES := 4096
const FRAME_SAMPLE_BUDGET := 12288
const FRAME_JOB_BUDGET := 12

var supported := false
var ready_state := false
var failed := false

var _init_requested := false
var _bindings_ready := false
var _bindings_building := false
var _binding_generation := -1
var _binding_macro_rid := RID()
var _render_batch_in_flight := false

var _rd_shader := RID()
var _rd_pipeline := RID()
var _rd_sampler := RID()
var _rd_cache := RID()
var _rd_uniform_set := RID()
var _cache_texture: Texture2DArrayRD

var _anchor_generation := 1
var _anchor_initialized := false
var _anchor_dir := Vector3(1.0, 0.0, 0.0)
var _anchor_right := Vector3(0.0, 0.0, -1.0)
var _anchor_up := Vector3(0.0, 1.0, 0.0)
var _center_plane := Vector2.ZERO
var _base_spacing := 0.75
var _needed_min := 0
var _needed_max := 0
var _frame_cursor := 0

var _window_min: Array[Vector2i] = []
var _window_known: Array[bool] = []
var _urgent_jobs: Array = []
var _warm_jobs: Array = []

var _jobs_dispatched := 0
var _samples_dispatched := 0
var _last_frame_samples := 0
var _last_frame_jobs := 0
var _anchor_resets := 0
var _strip_updates := 0
var _full_fills := 0


func _ready() -> void:
	process_mode = Node.PROCESS_MODE_DISABLED
	_window_min.resize(LEVEL_COUNT)
	_window_known.resize(LEVEL_COUNT)
	_urgent_jobs.resize(LEVEL_COUNT)
	_warm_jobs.resize(LEVEL_COUNT)
	for level: int in LEVEL_COUNT:
		_window_min[level] = Vector2i.ZERO
		_window_known[level] = false
		_urgent_jobs[level] = []
		_warm_jobs[level] = []

	var method := RenderingServer.get_current_rendering_method()
	supported = method == "forward_plus" or method == "mobile"
	if Planet.has_signal("world_ready"):
		Planet.world_ready.connect(_on_world_ready)
	if Planet.has_signal("coast_profile_changed"):
		Planet.coast_profile_changed.connect(_on_coast_profile_changed)
	if supported:
		call_deferred("_try_initialize")


func _exit_tree() -> void:
	if _cache_texture != null:
		_cache_texture.texture_rd_rid = RID()
	var rids: Array[RID] = [_rd_uniform_set, _rd_sampler, _rd_cache, _rd_pipeline, _rd_shader]
	RenderingServer.call_on_render_thread(_render_free.bind(rids))


func texture() -> Texture2DArrayRD:
	return _cache_texture


func cache_ready() -> bool:
	return ready_state and _cache_texture != null


func anchor_generation() -> int:
	return _anchor_generation


func cache_resolution() -> int:
	return CACHE_RES


func update_cache(anchor_dir: Vector3, anchor_right: Vector3, anchor_up: Vector3,
		center_plane: Vector2, base_spacing: float, active_min: int,
		active_max: int) -> void:
	if not supported or failed or Planet.cfg == null or not Planet.ready_state:
		return
	if not ready_state:
		_try_initialize()
		return
	if not _ensure_bindings():
		return

	var anchor_changed := not _anchor_initialized \
		or anchor_dir.distance_squared_to(_anchor_dir) > 1e-12 \
		or anchor_right.distance_squared_to(_anchor_right) > 1e-12 \
		or anchor_up.distance_squared_to(_anchor_up) > 1e-12
	if anchor_changed:
		_anchor_dir = anchor_dir.normalized()
		_anchor_right = anchor_right.normalized()
		_anchor_up = anchor_up.normalized()
		_anchor_initialized = true
		_invalidate_anchor()

	_center_plane = center_plane
	_base_spacing = maxf(base_spacing, 1e-6)
	_needed_min = clampi(active_min, 0, LEVEL_COUNT - 1)
	# Morphing the outermost active level can sample one parent level.
	_needed_max = clampi(active_max + 1, _needed_min, LEVEL_COUNT - 1)

	for level: int in range(_needed_min, _needed_max + 1):
		_update_level_window(level)

	_dispatch_staggered_batch()


func _on_world_ready(_fields: PlanetFields) -> void:
	_bindings_ready = false
	_binding_generation = -1
	_binding_macro_rid = RID()
	_anchor_initialized = false
	_invalidate_anchor()


func _on_coast_profile_changed() -> void:
	# Coast edits alter the macro/detail handoff even if the underlying texture RID
	# remains stable. Advance the generation so every old cache key becomes invalid.
	_invalidate_anchor()


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
		push_error("GPU terrain clipmap cache shader is invalid.")
		return
	_init_requested = true
	RenderingServer.call_on_render_thread(_render_initialize.bind(spirv))


func _render_initialize(spirv: RDShaderSPIRV) -> void:
	var rd: RenderingDevice = RenderingServer.get_rendering_device()
	if rd == null:
		call_deferred("_on_initialized", false, RID(), RID(), RID(), RID())
		return

	var shader := rd.shader_create_from_spirv(spirv, "Asterra terrain clipmap cache")
	if not shader.is_valid():
		call_deferred("_on_initialized", false, RID(), RID(), RID(), RID())
		return
	var pipeline := rd.compute_pipeline_create(shader)
	if not pipeline.is_valid() or not rd.compute_pipeline_is_valid(pipeline):
		rd.free_rid(shader)
		call_deferred("_on_initialized", false, RID(), RID(), RID(), RID())
		return

	var sampler_state := RDSamplerState.new()
	sampler_state.min_filter = RenderingDevice.SAMPLER_FILTER_LINEAR
	sampler_state.mag_filter = RenderingDevice.SAMPLER_FILTER_LINEAR
	sampler_state.mip_filter = RenderingDevice.SAMPLER_FILTER_LINEAR
	sampler_state.repeat_u = RenderingDevice.SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE
	sampler_state.repeat_v = RenderingDevice.SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE
	sampler_state.repeat_w = RenderingDevice.SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE
	var sampler := rd.sampler_create(sampler_state)

	var format := RDTextureFormat.new()
	format.texture_type = RenderingDevice.TEXTURE_TYPE_2D_ARRAY
	format.format = RenderingDevice.DATA_FORMAT_R32G32B32A32_SFLOAT
	format.width = CACHE_RES
	format.height = CACHE_RES
	format.depth = 1
	format.array_layers = LEVEL_COUNT
	format.mipmaps = 1
	format.usage_bits = RenderingDevice.TEXTURE_USAGE_SAMPLING_BIT \
		| RenderingDevice.TEXTURE_USAGE_STORAGE_BIT \
		| RenderingDevice.TEXTURE_USAGE_CAN_COPY_TO_BIT
	var cache := rd.texture_create(format, RDTextureView.new(), [])
	var ok := sampler.is_valid() and cache.is_valid()
	if ok:
		# Key zero is reserved for invalid/uninitialized texels.
		rd.texture_clear(cache, Color(0.0, 0.0, 0.0, 0.0), 0, 1, 0, LEVEL_COUNT)
	call_deferred("_on_initialized", ok, shader, pipeline, sampler, cache)


func _on_initialized(success: bool, shader: RID, pipeline: RID,
		sampler: RID, cache: RID) -> void:
	if not success:
		failed = true
		ready_state = false
		return
	_rd_shader = shader
	_rd_pipeline = pipeline
	_rd_sampler = sampler
	_rd_cache = cache
	_cache_texture = Texture2DArrayRD.new()
	_cache_texture.texture_rd_rid = cache
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
	if _bindings_ready and generation == _binding_generation \
			and macro_rid == _binding_macro_rid:
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
		generation, macro_rid, server_rids, _rd_shader, _rd_sampler, _rd_cache))
	return false


func _render_build_uniform_set(generation: int, macro_rid: RID,
		server_rids: Array, shader: RID, sampler: RID, cache: RID) -> void:
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

	var output := RDUniform.new()
	output.uniform_type = RenderingDevice.UNIFORM_TYPE_IMAGE
	output.binding = 7
	output.add_id(cache)
	uniforms.append(output)
	var set := rd.uniform_set_create(uniforms, shader, 0)
	call_deferred("_on_uniform_set_built", set.is_valid(), generation, macro_rid, set)


func _on_uniform_set_built(success: bool, generation: int,
		macro_rid: RID, uniform_set: RID) -> void:
	_bindings_building = false
	if not success:
		_bindings_ready = false
		return
	_rd_uniform_set = uniform_set
	_binding_generation = generation
	_binding_macro_rid = macro_rid
	_bindings_ready = true
	# Input data changed; old synthesized keys must not survive a context rebuild.
	_invalidate_anchor()


func _invalidate_anchor() -> void:
	_anchor_generation += 1
	if _anchor_generation > 1000000:
		_anchor_generation = 1
	_anchor_resets += 1
	for level: int in LEVEL_COUNT:
		_window_known[level] = false
		(_urgent_jobs[level] as Array).clear()
		(_warm_jobs[level] as Array).clear()


func _update_level_window(level: int) -> void:
	var spacing := _base_spacing * pow(2.0, float(level))
	var center := Vector2i(
		int(round(_center_plane.x / spacing)),
		int(round(_center_plane.y / spacing)))
	var desired_min := center - Vector2i(CACHE_RES >> 1, CACHE_RES >> 1)

	if not _window_known[level]:
		_window_min[level] = desired_min
		_window_known[level] = true
		_queue_full_window(level, desired_min)
		return

	var old_min: Vector2i = _window_min[level]
	if old_min == desired_min:
		return
	var delta := desired_min - old_min
	_window_min[level] = desired_min

	if absi(delta.x) >= CACHE_RES or absi(delta.y) >= CACHE_RES:
		(_urgent_jobs[level] as Array).clear()
		(_warm_jobs[level] as Array).clear()
		_queue_full_window(level, desired_min)
		return

	# Exact newly exposed strips. Jobs already pending from an older nearby window
	# remain useful; dispatch-time clipping drops anything that has since moved out.
	if delta.x > 0:
		_queue_rect(level, Rect2i(
			Vector2i(old_min.x + CACHE_RES, desired_min.y),
			Vector2i(delta.x, CACHE_RES)), true)
	elif delta.x < 0:
		_queue_rect(level, Rect2i(
			Vector2i(desired_min.x, desired_min.y),
			Vector2i(-delta.x, CACHE_RES)), true)

	if delta.y > 0:
		_queue_rect(level, Rect2i(
			Vector2i(desired_min.x, old_min.y + CACHE_RES),
			Vector2i(CACHE_RES, delta.y)), true)
	elif delta.y < 0:
		_queue_rect(level, Rect2i(
			Vector2i(desired_min.x, desired_min.y),
			Vector2i(CACHE_RES, -delta.y)), true)
	_strip_updates += 1


func _queue_full_window(level: int, window_min: Vector2i) -> void:
	# 64x64 jobs are one MAX_JOB_SAMPLES dispatch. Queue centre tiles first so the
	# actual visible 401-cell disc becomes cached before the unused outer margin.
	var local_jobs: Array = []
	var tile := 64
	for y: int in range(0, CACHE_RES, tile):
		for x: int in range(0, CACHE_RES, tile):
			var size := Vector2i(mini(tile, CACHE_RES - x), mini(tile, CACHE_RES - y))
			var center := Vector2(float(x) + float(size.x) * 0.5,
				float(y) + float(size.y) * 0.5)
			local_jobs.append({
				"level": level,
				"origin": window_min + Vector2i(x, y),
				"size": size,
				"generation": _anchor_generation,
				"priority": center.distance_squared_to(Vector2(CACHE_RES * 0.5, CACHE_RES * 0.5)),
			})
	local_jobs.sort_custom(_job_priority_less)
	for job: Dictionary in local_jobs:
		(_warm_jobs[level] as Array).append(job)
	_full_fills += 1


func _job_priority_less(a: Dictionary, b: Dictionary) -> bool:
	return float(a.get("priority", 0.0)) < float(b.get("priority", 0.0))


func _queue_rect(level: int, rect: Rect2i, urgent: bool) -> void:
	if rect.size.x <= 0 or rect.size.y <= 0:
		return
	var x := 0
	while x < rect.size.x:
		var chunk_w := mini(rect.size.x - x, MAX_JOB_SAMPLES)
		var max_h := maxi(1, MAX_JOB_SAMPLES / chunk_w)
		var y := 0
		while y < rect.size.y:
			var chunk_h := mini(rect.size.y - y, max_h)
			var job := {
				"level": level,
				"origin": rect.position + Vector2i(x, y),
				"size": Vector2i(chunk_w, chunk_h),
				"generation": _anchor_generation,
				"priority": 0.0,
			}
			if urgent:
				(_urgent_jobs[level] as Array).append(job)
			else:
				(_warm_jobs[level] as Array).append(job)
			y += chunk_h
		x += chunk_w


func _dispatch_staggered_batch() -> void:
	if _render_batch_in_flight or not _bindings_ready:
		return
	var payloads: Array[PackedByteArray] = []
	var sizes: Array[Vector2i] = []
	var remaining := FRAME_SAMPLE_BUDGET
	var jobs_taken := 0
	_last_frame_samples = 0
	_last_frame_jobs = 0

	while remaining > 0 and jobs_taken < FRAME_JOB_BUDGET:
		var job := _take_next_job(true)
		if job.is_empty():
			job = _take_next_job(false)
		if job.is_empty():
			break
		if int(job.get("generation", -1)) != _anchor_generation:
			continue
		var level := int(job.get("level", -1))
		if level < _needed_min or level > _needed_max or not _window_known[level]:
			continue

		var clipped := _clip_job_to_window(job, level)
		if clipped.is_empty():
			continue
		var size: Vector2i = clipped["size"]
		var samples := size.x * size.y
		if samples > remaining and not payloads.is_empty():
			# Put the job back at the front for the next balanced frame.
			(_urgent_jobs[level] as Array).push_front(clipped)
			break

		payloads.append(_make_push_constants(clipped))
		sizes.append(size)
		remaining -= samples
		_last_frame_samples += samples
		jobs_taken += 1

	if payloads.is_empty():
		return
	_frame_cursor += 1
	_last_frame_jobs = payloads.size()
	_render_batch_in_flight = true
	RenderingServer.call_on_render_thread(_render_dispatch_batch.bind(
		payloads, sizes, _rd_pipeline, _rd_uniform_set))


func _take_next_job(urgent: bool) -> Dictionary:
	var count := _needed_max - _needed_min + 1
	if count <= 0:
		return {}
	for offset: int in count:
		var level := _needed_min + ((_frame_cursor + offset) % count)
		var queue: Array = _urgent_jobs[level] if urgent else _warm_jobs[level]
		if not queue.is_empty():
			_frame_cursor = (level - _needed_min + 1) % count
			return queue.pop_front()
	return {}


func _clip_job_to_window(job: Dictionary, level: int) -> Dictionary:
	var origin: Vector2i = job["origin"]
	var size: Vector2i = job["size"]
	var job_end := origin + size
	var win_min: Vector2i = _window_min[level]
	var win_end := win_min + Vector2i(CACHE_RES, CACHE_RES)
	var clipped_min := Vector2i(maxi(origin.x, win_min.x), maxi(origin.y, win_min.y))
	var clipped_end := Vector2i(mini(job_end.x, win_end.x), mini(job_end.y, win_end.y))
	var clipped_size := clipped_end - clipped_min
	if clipped_size.x <= 0 or clipped_size.y <= 0:
		return {}
	return {
		"level": level,
		"origin": clipped_min,
		"size": clipped_size,
		"generation": _anchor_generation,
		"priority": job.get("priority", 0.0),
	}


func _make_push_constants(job: Dictionary) -> PackedByteArray:
	var bytes := PackedByteArray()
	bytes.resize(96)
	_encode_vec4(bytes, 0, _anchor_dir.x, _anchor_dir.y, _anchor_dir.z,
		Planet.cfg.planet_radius)
	_encode_vec4(bytes, 16, _anchor_right.x, _anchor_right.y, _anchor_right.z,
		_base_spacing)
	_encode_vec4(bytes, 32, _anchor_up.x, _anchor_up.y, _anchor_up.z,
		float(Planet.global_height_face_res))
	var context: Node = get_node_or_null("/root/PlanetContext")
	var context_res := float(context.get("face_res")) if context != null else 0.0
	var detail_seed := Planet.cfg.stream_seed("gpu_visual_detail") & 0x00ffffff
	var detail_strength := maxf(0.05, Planet.cfg.detail_amplitude / 260.0)
	_encode_vec4(bytes, 48, context_res, float(maxi(detail_seed, 1)),
		detail_strength, 0.0)
	var origin: Vector2i = job["origin"]
	var size: Vector2i = job["size"]
	bytes.encode_s32(64, origin.x)
	bytes.encode_s32(68, origin.y)
	bytes.encode_s32(72, size.x)
	bytes.encode_s32(76, size.y)
	bytes.encode_s32(80, int(job["level"]))
	bytes.encode_s32(84, CACHE_RES)
	bytes.encode_s32(88, _anchor_generation)
	bytes.encode_s32(92, 0)
	return bytes


func _encode_vec4(bytes: PackedByteArray, offset: int,
		x: float, y: float, z: float, w: float) -> void:
	bytes.encode_float(offset, x)
	bytes.encode_float(offset + 4, y)
	bytes.encode_float(offset + 8, z)
	bytes.encode_float(offset + 12, w)


func _render_dispatch_batch(payloads: Array, sizes: Array,
		pipeline: RID, uniform_set: RID) -> void:
	var rd: RenderingDevice = RenderingServer.get_rendering_device()
	if rd == null or not pipeline.is_valid() or not uniform_set.is_valid():
		call_deferred("_on_batch_complete", false, 0, 0)
		return
	var list := rd.compute_list_begin()
	rd.compute_list_bind_compute_pipeline(list, pipeline)
	rd.compute_list_bind_uniform_set(list, uniform_set, 0)
	var samples := 0
	for i: int in payloads.size():
		var push: PackedByteArray = payloads[i]
		var size: Vector2i = sizes[i]
		rd.compute_list_set_push_constant(list, push, push.size())
		rd.compute_list_dispatch(list,
			ceili(float(size.x) / float(LOCAL_SIZE)),
			ceili(float(size.y) / float(LOCAL_SIZE)), 1)
		samples += size.x * size.y
	rd.compute_list_end()
	call_deferred("_on_batch_complete", true, payloads.size(), samples)


func _on_batch_complete(success: bool, jobs: int, samples: int) -> void:
	_render_batch_in_flight = false
	if not success:
		return
	_jobs_dispatched += jobs
	_samples_dispatched += samples


func _render_free(rids: Array) -> void:
	var rd: RenderingDevice = RenderingServer.get_rendering_device()
	if rd == null:
		return
	for rid: RID in rids:
		if rid.is_valid():
			rd.free_rid(rid)


func _queued_job_count() -> int:
	var total := 0
	for level: int in LEVEL_COUNT:
		total += (_urgent_jobs[level] as Array).size()
		total += (_warm_jobs[level] as Array).size()
	return total


func stats() -> Dictionary:
	return {
		"supported": supported,
		"ready": ready_state,
		"failed": failed,
		"bindings_ready": _bindings_ready,
		"resolution": CACHE_RES,
		"levels": LEVEL_COUNT,
		"generation": _anchor_generation,
		"queued_jobs": _queued_job_count(),
		"last_frame_jobs": _last_frame_jobs,
		"last_frame_samples": _last_frame_samples,
		"jobs_dispatched": _jobs_dispatched,
		"samples_dispatched": _samples_dispatched,
		"anchor_resets": _anchor_resets,
		"strip_updates": _strip_updates,
		"full_fills": _full_fills,
		"sample_budget": FRAME_SAMPLE_BUDGET,
		"toroidal": true,
		"staggered": true,
	}
