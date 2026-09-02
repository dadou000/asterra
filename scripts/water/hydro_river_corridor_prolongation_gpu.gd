class_name HydroRiverCorridorProlongationGPU
extends Node
## Terrain-aware exact-volume seeding for one unpublished sparse river tile.
##
## After terrain staging, the shader restricts water to a line corridor through the
## tile, solves one level free surface over that corridor, and writes identical A+B
## state with a prescribed local downstream velocity. The target physical parcel is
## quantized downward to FP32 before the coarse ownership reservation, matching the
## surface prolongation contract.

signal initialized
signal initialization_failed(error: Error)
signal seed_recorded(request_id: int, slot: int, represented_volume_m3: float)
signal seed_failed(request_id: int, error: Error)
signal released

const PARAM_BYTES := 64
const BISECTION_ITERATIONS := 32
const MAX_SUPPORTED_CELLS := 4096

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
	if p_atlas == null or not p_atlas.initialized_ok() \
			or p_atlas.cells_per_tile() <= 0 \
			or p_atlas.cells_per_tile() > MAX_SUPPORTED_CELLS:
		return ERR_INVALID_PARAMETER
	if RenderingServer.get_rendering_device() == null:
		return ERR_UNAVAILABLE
	var shader_file: RDShaderFile = load(
		"res://shaders/water/hydro_river_corridor_prolongation.glsl")
	if shader_file == null:
		return ERR_CANT_OPEN
	var spirv := shader_file.get_spirv()
	if spirv == null:
		return ERR_CANT_CREATE
	atlas = p_atlas
	_init_pending = true
	RenderingServer.call_on_render_thread(
		Callable(self, &"_init_render_thread").bind(spirv))
	return OK


func initialized_ok() -> bool:
	return _initialized


func plan_volume(requested_volume_m3: float) -> Dictionary:
	if not _initialized or atlas == null or not is_finite(requested_volume_m3) \
			or requested_volume_m3 <= 0.0:
		return {"error": ERR_INVALID_PARAMETER}
	var bytes := PackedByteArray()
	bytes.resize(4)
	bytes.encode_float(0, requested_volume_m3)
	var target := float(bytes.decode_float(0))
	if not is_finite(target) or target <= 0.0:
		return {"error": ERR_INVALID_DATA}
	if target > requested_volume_m3:
		var bits := bytes.decode_u32(0)
		if bits == 0:
			return {"error": ERR_INVALID_DATA}
		bytes.encode_u32(0, bits - 1)
		target = float(bytes.decode_float(0))
	if not is_finite(target) or target <= 0.0 or target > requested_volume_m3:
		return {"error": ERR_INVALID_DATA}
	return {
		"error": OK,
		"requested_volume_m3": requested_volume_m3,
		"represented_volume_m3": target,
		"quantization_error_m3": target - requested_volume_m3,
		"strategy": "terrain_aligned_river_corridor",
	}


## center_cell is in tile-cell coordinates (0..tile_resolution), using the same
## coordinate system as pixel centres x+0.5/y+0.5. direction_cell is the local
## downstream direction in that grid. half_width_m is the physical river half-width.
func seed_reserved(slot: int, represented_volume_m3: float,
		center_cell: Vector2, direction_cell: Vector2, half_width_m: float,
		local_velocity: Vector2) -> int:
	if not _initialized or atlas == null or slot < 0 or slot >= atlas.capacity \
			or not is_finite(represented_volume_m3) or represented_volume_m3 <= 0.0 \
			or not _finite_vec2(center_cell) or not _finite_vec2(direction_cell) \
			or direction_cell.length_squared() <= 1.0e-12 \
			or not is_finite(half_width_m) or half_width_m <= 0.0 \
			or not _finite_vec2(local_velocity):
		return -1
	var plan := plan_volume(represented_volume_m3)
	if int(plan.get("error", FAILED)) != OK:
		return -1
	var target := float(plan.get("represented_volume_m3", 0.0))
	var tolerance := maxf(1.0e-9, represented_volume_m3 * 1.0e-7)
	if absf(target - represented_volume_m3) > tolerance:
		return -1
	var params := _make_params(slot, target, center_cell,
		direction_cell.normalized(), half_width_m, local_velocity)
	var request_id := _next_request_id
	_next_request_id += 1
	RenderingServer.call_on_render_thread(
		Callable(self, &"_seed_render_thread").bind(request_id, slot, target, params))
	return request_id


func _make_params(slot: int, target_volume_m3: float,
		center_cell: Vector2, direction_cell: Vector2, half_width_m: float,
		velocity: Vector2) -> PackedByteArray:
	var bytes := PackedByteArray()
	bytes.resize(PARAM_BYTES)
	bytes.encode_u32(0, slot)
	bytes.encode_u32(4, atlas.cells_per_tile())
	bytes.encode_u32(8, atlas.tile_resolution)
	bytes.encode_u32(12, atlas.capacity)
	bytes.encode_float(16, target_volume_m3)
	bytes.encode_float(20, atlas.cell_size_m * atlas.cell_size_m)
	bytes.encode_float(24, velocity.x)
	bytes.encode_float(28, velocity.y)
	bytes.encode_float(32, center_cell.x)
	bytes.encode_float(36, center_cell.y)
	bytes.encode_float(40, direction_cell.x)
	bytes.encode_float(44, direction_cell.y)
	bytes.encode_float(48, maxf(half_width_m / atlas.cell_size_m, 0.51))
	bytes.encode_float(52, float(BISECTION_ITERATIONS))
	bytes.encode_float(56, float(MAX_SUPPORTED_CELLS))
	bytes.encode_float(60, 0.0)
	return bytes


func _init_render_thread(spirv: RDShaderSPIRV) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null:
		call_deferred(&"_finish_init", ERR_UNAVAILABLE, {})
		return
	var shader := rd.shader_create_from_spirv(spirv)
	if not shader.is_valid():
		call_deferred(&"_finish_init", ERR_CANT_CREATE, {})
		return
	var pipeline := rd.compute_pipeline_create(shader)
	var zero := PackedByteArray()
	zero.resize(PARAM_BYTES)
	var params := rd.storage_buffer_create(PARAM_BYTES, zero)
	if not pipeline.is_valid() or not params.is_valid():
		_free_many(rd, [params, pipeline, shader])
		call_deferred(&"_finish_init", ERR_CANT_CREATE, {})
		return
	var set_rid := rd.uniform_set_create([
		_storage_uniform(0, atlas.state_a_rid()),
		_storage_uniform(1, atlas.state_b_rid()),
		_storage_uniform(2, params),
	], shader, 0)
	if not set_rid.is_valid():
		_free_many(rd, [params, pipeline, shader])
		call_deferred(&"_finish_init", ERR_CANT_CREATE, {})
		return
	call_deferred(&"_finish_init", OK, {
		"shader": shader,
		"pipeline": pipeline,
		"params": params,
		"set": set_rid,
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
		call_deferred(&"_finish_seed", request_id, slot,
			represented_volume_m3, ERR_UNAVAILABLE)
		return
	var err := rd.buffer_update(_params, 0, PARAM_BYTES, param_bytes)
	if err != OK:
		call_deferred(&"_finish_seed", request_id, slot, represented_volume_m3, err)
		return
	var compute := rd.compute_list_begin()
	rd.compute_list_bind_compute_pipeline(compute, _pipeline)
	rd.compute_list_bind_uniform_set(compute, _uniform_set, 0)
	rd.compute_list_dispatch(compute, 1, 1, 1)
	rd.compute_list_end()
	call_deferred(&"_finish_seed", request_id, slot, represented_volume_m3, OK)


func _finish_seed(request_id: int, slot: int,
		represented_volume_m3: float, error: Error) -> void:
	if error != OK:
		seed_failed.emit(request_id, error)
	else:
		seed_recorded.emit(request_id, slot, represented_volume_m3)


func _storage_uniform(binding: int, rid: RID) -> RDUniform:
	var uniform := RDUniform.new()
	uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
	uniform.binding = binding
	uniform.add_id(rid)
	return uniform


func _finite_vec2(v: Vector2) -> bool:
	return is_finite(v.x) and is_finite(v.y)


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
	_uniform_set = RID()
	_params = RID()
	_pipeline = RID()
	_shader = RID()
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
