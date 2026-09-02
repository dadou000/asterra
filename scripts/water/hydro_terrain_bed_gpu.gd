class_name HydroTerrainBedGPU
extends Node
## GPU initializer for reserved sparse-hydrology destination tiles.
##
## Pristine bed elevation is reconstructed directly on the main RenderingDevice
## from Planet.global_height_texture plus the same deterministic detail spectrum as
## the active terrain renderer. CPU only supplies a tiny tile-local array of sparse
## TerrainDeltas offsets; the full bed/state tile never travels CPU -> GPU.
##
## stage_reserved_tile() intentionally does not publish occupancy or metadata.
## Render-thread FIFO ordering lets the frontier activation pipeline queue:
##   terrain stage -> conservative handoff -> identity/occupancy publication.

signal initialized
signal initialization_failed(error: Error)
signal stage_recorded(request_id: int, tile_id: int, slot: int)
signal stage_failed(request_id: int, error: Error)
signal released

const LOCAL_X := 8
const LOCAL_Y := 8
const PARAM_BYTES := 64
const MAX_MACRO_MIP := 5

var _atlas: SparseHydroAtlasGPU
var _macro_texture: Texture2DArray
var _shader := RID()
var _pipeline := RID()
var _sampler := RID()
var _params := RID()
var _delta_offsets := RID()
var _uniform_set := RID()

var _planet_radius := 1.0
var _base_spacing := 1.0
var _terrain_level := 0
var _detail_seed := 1
var _detail_strength := 1.0
var _macro_face_res := 0
var _macro_max_mip := 0

var _initialized := false
var _init_pending := false
var _next_request_id := 1


func initialize(atlas: SparseHydroAtlasGPU, macro_texture: Texture2DArray = null,
		macro_face_res: int = -1, config_override: Dictionary = {}) -> Error:
	if _initialized or _init_pending:
		return ERR_BUSY
	if atlas == null or not atlas.initialized_ok():
		return ERR_INVALID_PARAMETER
	if RenderingServer.get_rendering_device() == null:
		return ERR_UNAVAILABLE

	_atlas = atlas
	_macro_texture = macro_texture
	if _macro_texture == null:
		if not Planet.ready_state or Planet.cfg == null:
			return ERR_UNCONFIGURED
		var texture_value: Variant = Planet.get("global_height_texture")
		if not (texture_value is Texture2DArray):
			return ERR_UNCONFIGURED
		_macro_texture = texture_value as Texture2DArray
		if macro_face_res < 0:
			macro_face_res = int(Planet.get("global_height_face_res"))

	if _macro_texture == null or macro_face_res <= 0:
		return ERR_INVALID_PARAMETER

	if config_override.has("planet_radius"):
		_planet_radius = maxf(float(config_override["planet_radius"]), 1.0)
	elif Planet.ready_state and Planet.cfg != null:
		_planet_radius = maxf(Planet.cfg.planet_radius, 1.0)
	else:
		return ERR_INVALID_PARAMETER

	if config_override.has("base_spacing"):
		_base_spacing = maxf(float(config_override["base_spacing"]), 1.0e-4)
	elif Planet.ready_state and Planet.cfg != null:
		_base_spacing = maxf(GroundHeightStore.spacing_for_level(0), 1.0e-4)
	else:
		_base_spacing = maxf(atlas.cell_size_m, 1.0e-4)

	if config_override.has("terrain_level"):
		_terrain_level = clampi(int(config_override["terrain_level"]), 0, 14)
	elif Planet.ready_state and Planet.cfg != null:
		_terrain_level = GroundHeightStore.level_for_spacing(atlas.cell_size_m)
	else:
		_terrain_level = 0

	if config_override.has("detail_seed"):
		_detail_seed = maxi(int(config_override["detail_seed"]), 1)
	elif Planet.ready_state and Planet.cfg != null:
		_detail_seed = maxi(Planet.cfg.stream_seed("gpu_visual_detail") & 0x00ffffff, 1)
	else:
		_detail_seed = 1

	if config_override.has("detail_strength"):
		_detail_strength = maxf(float(config_override["detail_strength"]), 0.0)
	elif Planet.ready_state and Planet.cfg != null:
		_detail_strength = maxf(0.05, Planet.cfg.detail_amplitude / 260.0)
	else:
		_detail_strength = 1.0

	_macro_face_res = macro_face_res
	var texture_width := maxi(_macro_texture.get_width(), 1)
	_macro_max_mip = mini(MAX_MACRO_MIP,
		maxi(int(floor(log(float(texture_width)) / log(2.0))), 0))

	var shader_file: RDShaderFile = load("res://shaders/water/hydro_terrain_bed_init.glsl")
	if shader_file == null:
		return ERR_CANT_OPEN
	var spirv := shader_file.get_spirv()
	if spirv == null:
		return ERR_CANT_CREATE

	_init_pending = true
	RenderingServer.call_on_render_thread(
		Callable(self, &"_init_render_thread").bind(spirv))
	return OK


func initialized_ok() -> bool:
	return _initialized


func configuration() -> Dictionary:
	return {
		"planet_radius": _planet_radius,
		"base_spacing": _base_spacing,
		"terrain_level": _terrain_level,
		"detail_seed": _detail_seed,
		"detail_strength": _detail_strength,
		"macro_face_res": _macro_face_res,
		"macro_max_mip": _macro_max_mip,
	}


## Queue GPU initialization for one ALLOCATING slot.
##
## delta_override is test/debug-only when supplied. In production the method
## samples only the sparse Deltas offsets at hydro cell centres; pristine bed is
## always reconstructed by the compute shader.
##
## Return contract is intentionally Dictionary so HydroFrontierActivationPipeline
## can distinguish queued GPU staging from its legacy PackedFloat32Array provider.
func stage_reserved_tile(key: HydroTileKey, slot: int,
		delta_override: PackedFloat32Array = PackedFloat32Array()) -> Dictionary:
	if not _initialized or _atlas == null or not _atlas.initialized_ok():
		return {"queued": false, "error": ERR_UNCONFIGURED}
	if key == null or slot < 0 or slot >= _atlas.capacity:
		return {"queued": false, "error": ERR_INVALID_PARAMETER}
	var deltas := delta_override
	if deltas.is_empty():
		deltas = _build_delta_patch(key)
	elif deltas.size() != _atlas.cells_per_tile():
		return {"queued": false, "error": ERR_INVALID_PARAMETER}

	var request_id := _next_request_id
	_next_request_id += 1
	var param_bytes := _make_params(key, slot)
	RenderingServer.call_on_render_thread(
		Callable(self, &"_stage_render_thread").bind(request_id, key.packed(), slot,
			param_bytes, deltas.to_byte_array()))
	return {"queued": true, "error": OK, "request_id": request_id}


func _build_delta_patch(key: HydroTileKey) -> PackedFloat32Array:
	var r := _atlas.tile_resolution
	var values := PackedFloat32Array()
	values.resize(r * r)
	if Deltas.is_empty():
		return values
	var side := float(1 << mini(key.level, 27))
	for y in r:
		for x in r:
			var fx := (float(key.x) + (float(x) + 0.5) / float(r)) / side
			var fy := (float(key.y) + (float(y) + 0.5) / float(r)) / side
			var u := fx * 2.0 - 1.0
			var v := fy * 2.0 - 1.0
			var d := CubeSphere.face_uv_to_dir(key.face, u, v)
			values[y * r + x] = Deltas.offset_at(d)
	return values


func _make_params(key: HydroTileKey, slot: int) -> PackedByteArray:
	var bytes := PackedByteArray()
	bytes.resize(PARAM_BYTES)
	bytes.encode_u32(0, key.face)
	bytes.encode_u32(4, key.level)
	bytes.encode_u32(8, key.x)
	bytes.encode_u32(12, key.y)
	bytes.encode_u32(16, slot)
	bytes.encode_u32(20, _atlas.tile_resolution)
	bytes.encode_u32(24, _terrain_level)
	bytes.encode_u32(28, _atlas.capacity)
	bytes.encode_float(32, _planet_radius)
	bytes.encode_float(36, _base_spacing)
	bytes.encode_float(40, _detail_strength)
	bytes.encode_float(44, float(_macro_face_res))
	bytes.encode_u32(48, _detail_seed)
	bytes.encode_u32(52, 1)
	bytes.encode_u32(56, _macro_max_mip)
	bytes.encode_u32(60, 0)
	return bytes


func _init_render_thread(spirv: RDShaderSPIRV) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null:
		call_deferred("_finish_init", ERR_UNAVAILABLE, {})
		return

	var macro_rd := RenderingServer.texture_get_rd_texture(_macro_texture.get_rid(), false)
	if not macro_rd.is_valid():
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
	sampler_state.mip_filter = RenderingDevice.SAMPLER_FILTER_LINEAR
	sampler_state.repeat_u = RenderingDevice.SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE
	sampler_state.repeat_v = RenderingDevice.SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE
	sampler_state.repeat_w = RenderingDevice.SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE
	var sampler := rd.sampler_create(sampler_state)
	var zero_params := PackedByteArray()
	zero_params.resize(PARAM_BYTES)
	var zero_deltas := PackedByteArray()
	zero_deltas.resize(_atlas.cells_per_tile() * 4)
	var params := rd.storage_buffer_create(PARAM_BYTES, zero_params)
	var delta_offsets := rd.storage_buffer_create(zero_deltas.size(), zero_deltas)
	if not sampler.is_valid() or not params.is_valid() or not delta_offsets.is_valid():
		_free_many(rd, [delta_offsets, params, sampler, pipeline, shader])
		call_deferred("_finish_init", ERR_CANT_CREATE, {})
		return

	var texture_uniform := RDUniform.new()
	texture_uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_SAMPLER_WITH_TEXTURE
	texture_uniform.binding = 5
	texture_uniform.add_id(sampler)
	texture_uniform.add_id(macro_rd)
	var set_rid := rd.uniform_set_create([
		_storage_uniform(0, _atlas.state_a_rid()),
		_storage_uniform(1, _atlas.state_b_rid()),
		_storage_uniform(2, _atlas.source_rid()),
		_storage_uniform(3, delta_offsets),
		_storage_uniform(4, params),
		texture_uniform,
	], shader, 0)
	if not set_rid.is_valid():
		_free_many(rd, [delta_offsets, params, sampler, pipeline, shader])
		call_deferred("_finish_init", ERR_CANT_CREATE, {})
		return

	call_deferred("_finish_init", OK, {
		"shader": shader, "pipeline": pipeline, "sampler": sampler,
		"params": params, "delta_offsets": delta_offsets, "set": set_rid,
	})


func _finish_init(error: Error, bundle: Dictionary) -> void:
	_init_pending = false
	if error != OK:
		_initialized = false
		initialization_failed.emit(error)
		return
	_shader = bundle["shader"]
	_pipeline = bundle["pipeline"]
	_sampler = bundle["sampler"]
	_params = bundle["params"]
	_delta_offsets = bundle["delta_offsets"]
	_uniform_set = bundle["set"]
	_initialized = true
	initialized.emit()


## Render-thread only. Every stage call updates the shared tiny parameter/delta
## buffers and records its dispatch before the next queued callback can overwrite
## them. This deliberately supports a batch of reserved destinations without a
## per-tile persistent GPU resource.
func _stage_render_thread(request_id: int, tile_id: int, slot: int,
		param_bytes: PackedByteArray, delta_bytes: PackedByteArray) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null or not _pipeline.is_valid() or not _uniform_set.is_valid():
		call_deferred("_finish_stage", request_id, tile_id, slot, ERR_UNAVAILABLE)
		return
	var err := rd.buffer_update(_params, 0, param_bytes.size(), param_bytes)
	if err == OK:
		err = rd.buffer_update(_delta_offsets, 0, delta_bytes.size(), delta_bytes)
	if err != OK:
		call_deferred("_finish_stage", request_id, tile_id, slot, err)
		return
	var groups := int(ceil(float(_atlas.tile_resolution) / float(LOCAL_X)))
	var compute := rd.compute_list_begin()
	rd.compute_list_bind_compute_pipeline(compute, _pipeline)
	rd.compute_list_bind_uniform_set(compute, _uniform_set, 0)
	rd.compute_list_dispatch(compute, groups, groups, 1)
	rd.compute_list_end()
	call_deferred("_finish_stage", request_id, tile_id, slot, OK)


func _finish_stage(request_id: int, tile_id: int, slot: int, error: Error) -> void:
	if error != OK:
		stage_failed.emit(request_id, error)
		return
	stage_recorded.emit(request_id, tile_id, slot)


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
	var rids := [_uniform_set, _delta_offsets, _params, _sampler, _pipeline, _shader]
	_initialized = false
	_init_pending = false
	_uniform_set = RID(); _delta_offsets = RID(); _params = RID()
	_sampler = RID(); _pipeline = RID(); _shader = RID()
	_atlas = null
	_macro_texture = null
	RenderingServer.call_on_render_thread(
		Callable(self, &"_release_render_thread").bind(rids))
	released.emit()


func _release_render_thread(rids: Array) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd != null:
		_free_many(rd, rids)


func _exit_tree() -> void:
	release()
