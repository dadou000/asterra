class_name HydroCoarseSeedGPU
extends Node
## One-shot exact-volume seed dispatcher for an unpublished sparse SWE slot.
##
## The caller first asks plan_volume() for the FP32-representable parcel. That
## represented volume is what the coarse ownership store must reserve/debit.
## seed_reserved() then adds the corresponding uniform depth and momentum to both
## atlas ping-pong states. No occupancy/identity is published here.

signal initialized
signal initialization_failed(error: Error)
signal seed_recorded(request_id: int, slot: int, represented_volume_m3: float)
signal seed_failed(request_id: int, error: Error)
signal released

const LOCAL_X := 8
const LOCAL_Y := 8
const PARAM_BYTES := 32

var atlas: SparseHydroAtlasGPU

var _shader := RID()
var _pipeline := RID()
var _params := RID()
var _uniform_set := RID()
var _initialized := false
var _init_pending := false
var _next_request_id := 1


func initialize(p_atlas: SparseHydroAtlasGPU) -> Error:
	if _initialized or _init_pending:
		return ERR_BUSY
	if p_atlas == null or not p_atlas.initialized_ok():
		return ERR_INVALID_PARAMETER
	if RenderingServer.get_rendering_device() == null:
		return ERR_UNAVAILABLE
	atlas = p_atlas
	var shader_file: RDShaderFile = load("res://shaders/water/hydro_coarse_seed.glsl")
	if shader_file == null:
		atlas = null
		return ERR_CANT_OPEN
	var spirv := shader_file.get_spirv()
	if spirv == null:
		atlas = null
		return ERR_CANT_CREATE
	_init_pending = true
	RenderingServer.call_on_render_thread(
		Callable(self, &"_init_render_thread").bind(spirv))
	return OK


func initialized_ok() -> bool:
	return _initialized


## Return the exact FP32 depth that the GPU will receive and the corresponding
## represented volume under the atlas metric. The ownership transaction must use
## represented_volume_m3, not the caller's unquantized request.
func plan_volume(requested_volume_m3: float) -> Dictionary:
	if not _initialized or atlas == null or not is_finite(requested_volume_m3) \
			or requested_volume_m3 <= 0.0:
		return {"error": ERR_INVALID_PARAMETER}
	var cell_area := maxf(atlas.cell_size_m * atlas.cell_size_m, 1.0e-12)
	var tile_area := cell_area * float(atlas.cells_per_tile())
	var raw_depth := requested_volume_m3 / tile_area
	var bytes := PackedByteArray()
	bytes.resize(4)
	bytes.encode_float(0, raw_depth)
	var depth_f32 := float(bytes.decode_float(0))
	if not is_finite(depth_f32) or depth_f32 <= 0.0:
		return {"error": ERR_INVALID_DATA}
	var represented := depth_f32 * tile_area
	return {
		"error": OK,
		"requested_volume_m3": requested_volume_m3,
		"depth_m": depth_f32,
		"represented_volume_m3": represented,
		"quantization_error_m3": represented - requested_volume_m3,
		"tile_area_m2": tile_area,
	}


## Seed an ALLOCATING/unpublished slot. `depth_m` should come from plan_volume().
## local_velocity is expressed in the tile's tangent/face-UV solver frame.
func seed_reserved(slot: int, depth_m: float,
		local_velocity: Vector2 = Vector2.ZERO) -> int:
	if not _initialized or atlas == null or slot < 0 or slot >= atlas.capacity:
		return -1
	if not is_finite(depth_m) or depth_m <= 0.0 \
			or not is_finite(local_velocity.x) or not is_finite(local_velocity.y):
		return -1
	var depth_bytes := PackedByteArray()
	depth_bytes.resize(4)
	depth_bytes.encode_float(0, depth_m)
	var depth_f32 := float(depth_bytes.decode_float(0))
	if depth_f32 <= 0.0:
		return -1
	var represented := depth_f32 * maxf(atlas.cell_size_m * atlas.cell_size_m, 1.0e-12) \
		* float(atlas.cells_per_tile())
	var params := _make_params(slot, depth_f32, local_velocity)
	var request_id := _next_request_id
	_next_request_id += 1
	RenderingServer.call_on_render_thread(
		Callable(self, &"_seed_render_thread").bind(request_id, slot,
			represented, params))
	return request_id


func _make_params(slot: int, depth_m: float, velocity: Vector2) -> PackedByteArray:
	var bytes := PackedByteArray()
	bytes.resize(PARAM_BYTES)
	bytes.encode_u32(0, slot)
	bytes.encode_u32(4, atlas.tile_resolution)
	bytes.encode_u32(8, atlas.capacity)
	bytes.encode_u32(12, 0)
	bytes.encode_float(16, depth_m)
	bytes.encode_float(20, velocity.x)
	bytes.encode_float(24, velocity.y)
	bytes.encode_float(28, 0.0)
	return bytes


func _init_render_thread(spirv: RDShaderSPIRV) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null:
		call_deferred("_finish_init", ERR_UNAVAILABLE, {})
		return
	var shader := rd.shader_create_from_spirv(spirv)
	if not shader.is_valid():
		call_deferred("_finish_init", ERR_CANT_CREATE, {})
		return
	var pipeline := rd.compute_pipeline_create(shader)
	var zero_params := PackedByteArray()
	zero_params.resize(PARAM_BYTES)
	var params := rd.storage_buffer_create(PARAM_BYTES, zero_params)
	if not pipeline.is_valid() or not params.is_valid():
		_free_many(rd, [params, pipeline, shader])
		call_deferred("_finish_init", ERR_CANT_CREATE, {})
		return
	var set_rid := rd.uniform_set_create([
		_storage_uniform(0, atlas.state_a_rid()),
		_storage_uniform(1, atlas.state_b_rid()),
		_storage_uniform(2, params),
	], shader, 0)
	if not set_rid.is_valid():
		_free_many(rd, [params, pipeline, shader])
		call_deferred("_finish_init", ERR_CANT_CREATE, {})
		return
	call_deferred("_finish_init", OK, {
		"shader": shader, "pipeline": pipeline,
		"params": params, "set": set_rid,
	})


func _finish_init(error: Error, bundle: Dictionary) -> void:
	_init_pending = false
	if error != OK:
		_initialized = false
		initialization_failed.emit(error)
		return
	_shader = bundle["shader"]
	_pipeline = bundle["pipeline"]
	_params = bundle["params"]
	_uniform_set = bundle["set"]
	_initialized = true
	initialized.emit()


func _seed_render_thread(request_id: int, slot: int,
		represented_volume_m3: float, param_bytes: PackedByteArray) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null or not _pipeline.is_valid() or not _uniform_set.is_valid():
		call_deferred("_finish_seed", request_id, slot, represented_volume_m3,
			ERR_UNAVAILABLE)
		return
	var err := rd.buffer_update(_params, 0, param_bytes.size(), param_bytes)
	if err != OK:
		call_deferred("_finish_seed", request_id, slot, represented_volume_m3, err)
		return
	var groups := int(ceil(float(atlas.tile_resolution) / float(LOCAL_X)))
	var compute := rd.compute_list_begin()
	rd.compute_list_bind_compute_pipeline(compute, _pipeline)
	rd.compute_list_bind_uniform_set(compute, _uniform_set, 0)
	rd.compute_list_dispatch(compute, groups, groups, 1)
	rd.compute_list_end()
	call_deferred("_finish_seed", request_id, slot, represented_volume_m3, OK)


func _finish_seed(request_id: int, slot: int,
		represented_volume_m3: float, error: Error) -> void:
	if error != OK:
		seed_failed.emit(request_id, error)
		return
	seed_recorded.emit(request_id, slot, represented_volume_m3)


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
	var rids := [_uniform_set, _params, _pipeline, _shader]
	_initialized = false
	_init_pending = false
	_uniform_set = RID(); _params = RID(); _pipeline = RID(); _shader = RID()
	atlas = null
	RenderingServer.call_on_render_thread(
		Callable(self, &"_release_render_thread").bind(rids))
	released.emit()


func _release_render_thread(rids: Array) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd != null:
		_free_many(rd, rids)


func _exit_tree() -> void:
	release()
