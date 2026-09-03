extends "res://scripts/terrain/gpu_terrain_clipmap_cache_budgeted.gd"
## Phase 42A cache: consume the same normalized production geomorph snapshot as the
## visible analytic terrain shader.
##
## The historical cache compute shader carried a frozen copy of the production
## geomorph constants. Planet Studio could therefore change live render uniforms
## while warm cache texels retained old terrain. This cache owns a small std430
## control buffer, invalidates every cache key when the active snapshot changes, and
## updates the buffer before new synthesis dispatches are queued on the render thread.

const GEOMORPH_CONTRACT := preload(
	"res://scripts/world_authoring/model/terrain_geomorph_gpu_contract.gd")

var _rd_geomorph_controls := RID()
var _geomorph_controls: Dictionary = GEOMORPH_CONTRACT.normalized_controls({})
var _geomorph_control_bytes: PackedByteArray = GEOMORPH_CONTRACT.pack_controls({})
var _geomorph_fingerprint: String = ""
var _effective_detail_seed: int = 1337
var _geomorph_revision: int = 0
var _geomorph_buffer_updates: int = 0


func _exit_tree() -> void:
	var control_buffer: RID = _rd_geomorph_controls
	_rd_geomorph_controls = RID()
	if control_buffer.is_valid():
		RenderingServer.call_on_render_thread(_render_free.bind([control_buffer]))
	super._exit_tree()


func set_production_controls(source: Dictionary) -> bool:
	var fallback_seed: int = 1337
	if Planet.cfg != null:
		fallback_seed = int(Planet.cfg.stream_seed("gpu_visual_detail")) & 0x00ffffff
	var normalized: Dictionary = GEOMORPH_CONTRACT.normalized_controls(source)
	var next_seed: int = GEOMORPH_CONTRACT.effective_seed(source, fallback_seed)
	var next_fingerprint: String = GEOMORPH_CONTRACT.fingerprint(source, fallback_seed)
	if next_fingerprint == _geomorph_fingerprint:
		return false

	_geomorph_controls = normalized
	_geomorph_control_bytes = GEOMORPH_CONTRACT.pack_controls(normalized)
	_effective_detail_seed = next_seed
	_geomorph_fingerprint = next_fingerprint
	_geomorph_revision += 1

	# Cache keys carry the anchor generation. Advancing it before any dispatch makes
	# every texel synthesized from the previous control snapshot unreadable at once;
	# the renderer uses its exact analytic fallback until replacement texels arrive.
	_invalidate_anchor("terrain_edit")
	if _rd_geomorph_controls.is_valid():
		RenderingServer.call_on_render_thread(_render_update_geomorph_controls.bind(
			_rd_geomorph_controls, _geomorph_control_bytes.duplicate()))
	return true


func production_controls_fingerprint() -> String:
	return _geomorph_fingerprint


func production_controls_snapshot() -> Dictionary:
	return _geomorph_controls.duplicate(true)


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
	RenderingServer.call_on_render_thread(_render_initialize_phase42.bind(
		spirv, _geomorph_control_bytes.duplicate()))


func _render_initialize_phase42(spirv: RDShaderSPIRV,
		control_bytes: PackedByteArray) -> void:
	var rd: RenderingDevice = RenderingServer.get_rendering_device()
	if rd == null:
		call_deferred("_on_initialized_phase42", false,
			RID(), RID(), RID(), RID(), RID())
		return

	var shader := rd.shader_create_from_spirv(spirv, "Asterra terrain clipmap cache")
	if not shader.is_valid():
		call_deferred("_on_initialized_phase42", false,
			RID(), RID(), RID(), RID(), RID())
		return
	var pipeline := rd.compute_pipeline_create(shader)
	if not pipeline.is_valid() or not rd.compute_pipeline_is_valid(pipeline):
		rd.free_rid(shader)
		call_deferred("_on_initialized_phase42", false,
			RID(), RID(), RID(), RID(), RID())
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
	var controls := rd.storage_buffer_create(control_bytes.size(), control_bytes)
	var ok: bool = sampler.is_valid() and cache.is_valid() and controls.is_valid()
	if ok:
		rd.texture_clear(cache, Color(0.0, 0.0, 0.0, 0.0),
			0, 1, 0, LEVEL_COUNT)
	else:
		for rid: RID in [controls, cache, sampler, pipeline, shader]:
			if rid.is_valid():
				rd.free_rid(rid)
		shader = RID()
		pipeline = RID()
		sampler = RID()
		cache = RID()
		controls = RID()
	call_deferred("_on_initialized_phase42", ok,
		shader, pipeline, sampler, cache, controls)


func _on_initialized_phase42(success: bool, shader: RID, pipeline: RID,
		sampler: RID, cache: RID, controls: RID) -> void:
	_rd_geomorph_controls = controls
	super._on_initialized(success, shader, pipeline, sampler, cache)


func _render_build_uniform_set(generation: int, macro_rid: RID,
		server_rids: Array, shader: RID, sampler: RID, cache: RID) -> void:
	var rd: RenderingDevice = RenderingServer.get_rendering_device()
	if rd == null or not _rd_geomorph_controls.is_valid():
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
		uniforms.append(u)

	var output := RDUniform.new()
	output.uniform_type = RenderingDevice.UNIFORM_TYPE_IMAGE
	output.binding = 7
	output.add_id(cache)
	uniforms.append(output)

	var controls_uniform := RDUniform.new()
	controls_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
	controls_uniform.binding = 8
	controls_uniform.add_id(_rd_geomorph_controls)
	uniforms.append(controls_uniform)

	var set := rd.uniform_set_create(uniforms, shader, 0)
	call_deferred("_on_uniform_set_built", set.is_valid(), generation, macro_rid, set)


func _render_update_geomorph_controls(buffer: RID,
		bytes: PackedByteArray) -> void:
	var rd: RenderingDevice = RenderingServer.get_rendering_device()
	if rd == null or not buffer.is_valid() or bytes.size() != GEOMORPH_CONTRACT.BYTE_SIZE:
		return
	var error: Error = rd.buffer_update(buffer, 0, bytes.size(), bytes)
	if error == OK:
		call_deferred("_on_geomorph_buffer_updated")


func _on_geomorph_buffer_updated() -> void:
	_geomorph_buffer_updates += 1


func _make_push_constants(job: Dictionary) -> PackedByteArray:
	var bytes: PackedByteArray = super._make_push_constants(job)
	# Keep the historical push fields coherent for diagnostics/backward shader
	# compatibility. Phase 42 compute reads detail strength from the shared buffer.
	bytes.encode_float(52, float(_effective_detail_seed))
	bytes.encode_float(56, float(_geomorph_controls.get("detail_strength", 1.0)))
	return bytes


func stats() -> Dictionary:
	var out: Dictionary = super.stats()
	out["geomorph_contract_version"] = GEOMORPH_CONTRACT.CONTRACT_VERSION
	out["geomorph_controls_fingerprint"] = _geomorph_fingerprint
	out["geomorph_revision"] = _geomorph_revision
	out["geomorph_effective_seed"] = _effective_detail_seed
	out["geomorph_buffer_ready"] = _rd_geomorph_controls.is_valid()
	out["geomorph_buffer_updates"] = _geomorph_buffer_updates
	out["geomorph_control_bytes"] = GEOMORPH_CONTRACT.BYTE_SIZE
	return out
