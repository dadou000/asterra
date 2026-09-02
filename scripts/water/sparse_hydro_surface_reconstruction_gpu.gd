class_name SparseHydroSurfaceReconstructionGPU
extends Node
## View/cache reconstruction from SparseHydroAtlasGPU into WaterSurfaceResources.
##
## The physical atlas remains authoritative and planet-persistent. This object owns
## only an ephemeral metadata->slot hash, compute pipelines and params. Every cache
## texel maps planet direction -> HydroTileKey -> resident slot entirely on GPU.

signal initialized
signal initialization_failed(error: Error)
signal reconstruction_recorded(request_id: int)
signal reconstruction_failed(request_id: int, error: Error)
signal released

const HASH_LOCAL_X := 64
const RECON_LOCAL_X := 8
const RECON_LOCAL_Y := 8
const PARAM_FLOATS := 24
const HASH_LOAD_FACTOR_DENOM := 8 # table >= capacity*8; low collision rate

var _atlas: SparseHydroAtlasGPU
var _target := RID()
var _hash_shader := RID()
var _hash_pipeline := RID()
var _recon_shader := RID()
var _recon_pipeline := RID()
var _hash_table := RID()
var _params := RID()
var _hash_set := RID()
var _recon_set := RID()

var _hydro_level := 0
var _planet_radius_m := 1.0
var _target_resolution := 0
var _target_half_extent_m := 1.0
var _dry_eps := 1.0e-5
var _hash_size := 0
var _initialized := false
var _init_pending := false
var _dispatch_pending := false
var _next_request_id := 1


func initialize(atlas: SparseHydroAtlasGPU, target_texture_rid: RID,
		target_resolution: int, target_half_extent_m: float,
		hydro_level: int, planet_radius_m: float, dry_eps: float = 1.0e-5) -> Error:
	if _initialized or _init_pending or _dispatch_pending:
		return ERR_BUSY
	if atlas == null or not atlas.initialized_ok() or not target_texture_rid.is_valid() \
			or target_resolution <= 0 or target_half_extent_m <= 0.0 \
			or planet_radius_m <= 0.0:
		return ERR_INVALID_PARAMETER
	if RenderingServer.get_rendering_device() == null:
		return ERR_UNAVAILABLE

	var hash_file: RDShaderFile = load("res://shaders/water/sparse_hydro_surface_hash.glsl")
	var recon_file: RDShaderFile = load(
		"res://shaders/water/sparse_hydro_surface_reconstruct.glsl")
	if hash_file == null or recon_file == null:
		return ERR_CANT_OPEN
	var hash_spirv := hash_file.get_spirv()
	var recon_spirv := recon_file.get_spirv()
	if hash_spirv == null or recon_spirv == null:
		return ERR_CANT_CREATE

	_atlas = atlas
	_target = target_texture_rid
	_target_resolution = target_resolution
	_target_half_extent_m = target_half_extent_m
	_hydro_level = clampi(hydro_level, 0, HydroTileKey.MAX_LEVEL)
	_planet_radius_m = maxf(planet_radius_m, 1.0)
	_dry_eps = maxf(dry_eps, 1.0e-8)
	_hash_size = _next_power_of_two(maxi(atlas.capacity * HASH_LOAD_FACTOR_DENOM, 64))
	_init_pending = true
	RenderingServer.call_on_render_thread(Callable(self, &"_init_render_thread").bind(
		hash_spirv, recon_spirv))
	return OK


func initialized_ok() -> bool:
	return _initialized


func pending() -> bool:
	return _dispatch_pending


func hash_size() -> int:
	return _hash_size


func gpu_bytes_estimate() -> int:
	return _hash_size * 4 + PARAM_FLOATS * 4


func stats() -> Dictionary:
	return {
		"initialized": _initialized,
		"pending": _dispatch_pending,
		"hydro_level": _hydro_level,
		"planet_radius_m": _planet_radius_m,
		"target_resolution": _target_resolution,
		"target_half_extent_m": _target_half_extent_m,
		"hash_size": _hash_size,
		"gpu_bytes": gpu_bytes_estimate(),
	}


func reconstruct(anchor_dir: Vector3, anchor_right: Vector3, anchor_up: Vector3,
		target_center_plane: Vector2 = Vector2.ZERO,
		activity_gain: float = 0.30) -> int:
	if not _initialized or _dispatch_pending or anchor_dir.length_squared() < 0.5 \
			or anchor_right.length_squared() < 0.5 or anchor_up.length_squared() < 0.5:
		return -1
	var request_id := _next_request_id
	_next_request_id += 1
	_dispatch_pending = true
	var normalized_dir := anchor_dir.normalized()
	var normalized_right := anchor_right.normalized()
	var normalized_up := anchor_up.normalized()
	var values := PackedFloat32Array([
		float(_atlas.capacity), float(_atlas.tile_resolution), float(_hydro_level),
		float(_hash_size - 1),
		_planet_radius_m, _dry_eps, float(_target_resolution), _target_half_extent_m,
		normalized_dir.x, normalized_dir.y, normalized_dir.z, 0.0,
		normalized_right.x, normalized_right.y, normalized_right.z, 0.0,
		normalized_up.x, normalized_up.y, normalized_up.z, 0.0,
		target_center_plane.x, target_center_plane.y, maxf(activity_gain, 0.0), 0.0,
	])
	RenderingServer.call_on_render_thread(Callable(self, &"_reconstruct_render_thread").bind(
		request_id, values.to_byte_array()))
	return request_id


func _init_render_thread(hash_spirv: RDShaderSPIRV, recon_spirv: RDShaderSPIRV) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null or not rd.texture_is_valid(_target):
		call_deferred("_finish_init", ERR_UNAVAILABLE, {})
		return
	var hash_shader := rd.shader_create_from_spirv(hash_spirv)
	var recon_shader := rd.shader_create_from_spirv(recon_spirv)
	if not hash_shader.is_valid() or not recon_shader.is_valid():
		_free_many(rd, [hash_shader, recon_shader])
		call_deferred("_finish_init", ERR_CANT_CREATE, {})
		return
	var hash_pipeline := rd.compute_pipeline_create(hash_shader)
	var recon_pipeline := rd.compute_pipeline_create(recon_shader)
	if not hash_pipeline.is_valid() or not recon_pipeline.is_valid():
		_free_many(rd, [hash_pipeline, recon_pipeline, hash_shader, recon_shader])
		call_deferred("_finish_init", ERR_CANT_CREATE, {})
		return

	var hash_bytes := PackedByteArray()
	hash_bytes.resize(_hash_size * 4)
	var param_bytes := PackedByteArray()
	param_bytes.resize(PARAM_FLOATS * 4)
	var hash_table := rd.storage_buffer_create(hash_bytes.size(), hash_bytes)
	var params := rd.storage_buffer_create(param_bytes.size(), param_bytes)
	if not hash_table.is_valid() or not params.is_valid():
		_free_many(rd, [hash_table, params, hash_pipeline, recon_pipeline,
			hash_shader, recon_shader])
		call_deferred("_finish_init", ERR_CANT_CREATE, {})
		return

	var hash_set := rd.uniform_set_create([
		_storage_uniform(0, _atlas.occupancy_rid()),
		_storage_uniform(1, _atlas.tile_metadata_rid()),
		_storage_uniform(2, hash_table),
		_storage_uniform(3, params),
	], hash_shader, 0)
	var recon_set := rd.uniform_set_create([
		_storage_uniform(0, _atlas.state_a_rid()),
		_storage_uniform(1, _atlas.occupancy_rid()),
		_storage_uniform(2, _atlas.tile_metadata_rid()),
		_storage_uniform(3, hash_table),
		_image_uniform(4, _target),
		_storage_uniform(5, params),
	], recon_shader, 0)
	if not hash_set.is_valid() or not recon_set.is_valid():
		_free_many(rd, [hash_set, recon_set, hash_table, params,
			hash_pipeline, recon_pipeline, hash_shader, recon_shader])
		call_deferred("_finish_init", ERR_CANT_CREATE, {})
		return

	call_deferred("_finish_init", OK, {
		"hash_shader": hash_shader,
		"hash_pipeline": hash_pipeline,
		"recon_shader": recon_shader,
		"recon_pipeline": recon_pipeline,
		"hash_table": hash_table,
		"params": params,
		"hash_set": hash_set,
		"recon_set": recon_set,
	})


func _finish_init(error: Error, bundle: Dictionary) -> void:
	_init_pending = false
	if error != OK:
		initialization_failed.emit(error)
		return
	_hash_shader = bundle["hash_shader"]
	_hash_pipeline = bundle["hash_pipeline"]
	_recon_shader = bundle["recon_shader"]
	_recon_pipeline = bundle["recon_pipeline"]
	_hash_table = bundle["hash_table"]
	_params = bundle["params"]
	_hash_set = bundle["hash_set"]
	_recon_set = bundle["recon_set"]
	_initialized = true
	initialized.emit()


func _reconstruct_render_thread(request_id: int, params_bytes: PackedByteArray) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null or not _all_rids_valid() or not rd.texture_is_valid(_target):
		call_deferred("_finish_reconstruct", request_id, ERR_UNAVAILABLE)
		return
	var err := rd.buffer_update(_params, 0, params_bytes.size(), params_bytes)
	if err != OK:
		call_deferred("_finish_reconstruct", request_id, err)
		return
	err = rd.buffer_clear(_hash_table, 0, _hash_size * 4)
	if err != OK:
		call_deferred("_finish_reconstruct", request_id, err)
		return

	var compute := rd.compute_list_begin()
	rd.compute_list_bind_compute_pipeline(compute, _hash_pipeline)
	rd.compute_list_bind_uniform_set(compute, _hash_set, 0)
	rd.compute_list_dispatch(compute,
		int(ceil(float(_atlas.capacity) / float(HASH_LOCAL_X))), 1, 1)
	rd.compute_list_add_barrier(compute)

	rd.compute_list_bind_compute_pipeline(compute, _recon_pipeline)
	rd.compute_list_bind_uniform_set(compute, _recon_set, 0)
	rd.compute_list_dispatch(compute,
		int(ceil(float(_target_resolution) / float(RECON_LOCAL_X))),
		int(ceil(float(_target_resolution) / float(RECON_LOCAL_Y))), 1)
	rd.compute_list_end()
	call_deferred("_finish_reconstruct", request_id, OK)


func _finish_reconstruct(request_id: int, error: Error) -> void:
	_dispatch_pending = false
	if error != OK:
		reconstruction_failed.emit(request_id, error)
		return
	reconstruction_recorded.emit(request_id)


func _all_rids_valid() -> bool:
	return _hash_shader.is_valid() and _hash_pipeline.is_valid() \
		and _recon_shader.is_valid() and _recon_pipeline.is_valid() \
		and _hash_table.is_valid() and _params.is_valid() \
		and _hash_set.is_valid() and _recon_set.is_valid()


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


func _next_power_of_two(value: int) -> int:
	var out := 1
	var target := maxi(value, 1)
	while out < target:
		out = out << 1
	return out


func _free_many(rd: RenderingDevice, values: Array) -> void:
	for value in values:
		if value is RID:
			var rid: RID = value
			if rid.is_valid():
				rd.free_rid(rid)


func release() -> void:
	if not _initialized and not _hash_shader.is_valid():
		return
	var rids := [_hash_set, _recon_set, _hash_table, _params,
		_hash_pipeline, _recon_pipeline, _hash_shader, _recon_shader]
	_initialized = false
	_init_pending = false
	_dispatch_pending = false
	_hash_set = RID(); _recon_set = RID(); _hash_table = RID(); _params = RID()
	_hash_pipeline = RID(); _recon_pipeline = RID()
	_hash_shader = RID(); _recon_shader = RID()
	_atlas = null
	RenderingServer.call_on_render_thread(
		Callable(self, &"_release_render_thread").bind(rids))
	released.emit()


func _release_render_thread(rids: Array) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd != null:
		_free_many(rd, rids)


func _exit_tree() -> void:
	release()
