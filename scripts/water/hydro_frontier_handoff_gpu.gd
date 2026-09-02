class_name HydroFrontierHandoffGPU
extends Node
## GPU dispatcher for conservative same-level frontier pre-wetting.
##
## The destination slot must be reserved/ALLOCATING and staged with its own dry
## terrain state, but still unpublished (occupancy=0). seed() records a pairwise
## source-edge -> destination-edge transfer into canonical atlas A. Callers should
## activate the reserved tile only after handoff_recorded, then rebuild connectivity.

signal initialized
signal initialization_failed(error: Error)
signal handoff_recorded(request_id: int, source_slot: int, destination_slot: int)
signal handoff_failed(request_id: int, error: Error)
signal released

const LOCAL_X := 64
const PARAM_BYTES := 48

var _atlas: SparseHydroAtlasGPU
var _shader := RID()
var _pipeline := RID()
var _params := RID()
var _uniform_set := RID()
var _initialized := false
var _init_pending := false
var _handoff_pending := false
var _next_request_id := 1


func initialize(atlas: SparseHydroAtlasGPU) -> Error:
	if _initialized or _init_pending or _handoff_pending:
		return ERR_BUSY
	if atlas == null or not atlas.initialized_ok():
		return ERR_INVALID_PARAMETER
	if RenderingServer.get_rendering_device() == null:
		return ERR_UNAVAILABLE
	var shader_file: RDShaderFile = load(
		"res://shaders/water/sparse_hydro_frontier_handoff.glsl")
	if shader_file == null:
		return ERR_CANT_OPEN
	var spirv := shader_file.get_spirv()
	if spirv == null:
		return ERR_CANT_CREATE
	_atlas = atlas
	_init_pending = true
	RenderingServer.call_on_render_thread(
		Callable(self, &"_init_render_thread").bind(spirv))
	return OK


func initialized_ok() -> bool:
	return _initialized


func pending() -> bool:
	return _handoff_pending


func seed(source_slot: int, destination_slot: int, source_direction: int,
		destination_direction: int, reversed: bool, seed_dt_s: float,
		max_source_fraction: float = 0.12, gravity: float = 9.81) -> int:
	if not _initialized or _handoff_pending or _atlas == null \
			or not _atlas.initialized_ok():
		return -1
	if source_slot < 0 or source_slot >= _atlas.capacity \
			or destination_slot < 0 or destination_slot >= _atlas.capacity \
			or source_slot == destination_slot:
		return -1
	if source_direction < 0 or source_direction > 3 \
			or destination_direction < 0 or destination_direction > 3 \
			or not is_finite(seed_dt_s) or seed_dt_s <= 0.0:
		return -1

	var request_id := _next_request_id
	_next_request_id += 1
	_handoff_pending = true
	var bytes := _make_params(source_slot, destination_slot, source_direction,
		destination_direction, reversed, seed_dt_s, max_source_fraction, gravity)
	RenderingServer.call_on_render_thread(
		Callable(self, &"_seed_render_thread").bind(
			request_id, source_slot, destination_slot, bytes))
	return request_id


func _make_params(source_slot: int, destination_slot: int, source_direction: int,
		destination_direction: int, reversed: bool, seed_dt_s: float,
		max_source_fraction: float, gravity: float) -> PackedByteArray:
	# std430 layout: 2*uvec4 + vec4 = 48 bytes. Build the integer and float words
	# directly into one byte array so no Variant packing ambiguity reaches RD.
	var bytes := PackedByteArray()
	bytes.resize(PARAM_BYTES)
	bytes.encode_u32(0, source_slot)
	bytes.encode_u32(4, destination_slot)
	bytes.encode_u32(8, source_direction)
	bytes.encode_u32(12, destination_direction)
	bytes.encode_u32(16, 1 if reversed else 0)
	bytes.encode_u32(20, _atlas.tile_resolution)
	bytes.encode_u32(24, _atlas.capacity)
	bytes.encode_u32(28, 0)
	bytes.encode_float(32, _atlas.cell_size_m)
	bytes.encode_float(36, seed_dt_s)
	bytes.encode_float(40, clampf(max_source_fraction, 0.0, 0.5))
	bytes.encode_float(44, maxf(gravity, 1.0e-4))
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
	if not pipeline.is_valid():
		rd.free_rid(shader)
		call_deferred("_finish_init", ERR_CANT_CREATE, {})
		return
	var zero := PackedByteArray()
	zero.resize(PARAM_BYTES)
	var params := rd.storage_buffer_create(PARAM_BYTES, zero)
	if not params.is_valid():
		_free_many(rd, [pipeline, shader])
		call_deferred("_finish_init", ERR_CANT_CREATE, {})
		return
	var set_rid := rd.uniform_set_create([
		_storage_uniform(0, _atlas.state_a_rid()),
		_storage_uniform(1, _atlas.occupancy_rid()),
		_storage_uniform(2, params),
	], shader, 0)
	if not set_rid.is_valid():
		_free_many(rd, [params, pipeline, shader])
		call_deferred("_finish_init", ERR_CANT_CREATE, {})
		return
	call_deferred("_finish_init", OK, {
		"shader": shader, "pipeline": pipeline, "params": params, "set": set_rid,
	})


func _finish_init(error: Error, bundle: Dictionary) -> void:
	_init_pending = false
	if error != OK:
		initialization_failed.emit(error)
		return
	_shader = bundle["shader"]
	_pipeline = bundle["pipeline"]
	_params = bundle["params"]
	_uniform_set = bundle["set"]
	_initialized = true
	initialized.emit()


func _seed_render_thread(request_id: int, source_slot: int,
		destination_slot: int, param_bytes: PackedByteArray) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null or not _pipeline.is_valid() or not _uniform_set.is_valid():
		call_deferred("_finish_handoff", request_id, source_slot,
			destination_slot, ERR_UNAVAILABLE)
		return
	var err := rd.buffer_update(_params, 0, param_bytes.size(), param_bytes)
	if err != OK:
		call_deferred("_finish_handoff", request_id, source_slot,
			destination_slot, err)
		return
	var compute := rd.compute_list_begin()
	rd.compute_list_bind_compute_pipeline(compute, _pipeline)
	rd.compute_list_bind_uniform_set(compute, _uniform_set, 0)
	rd.compute_list_dispatch(compute,
		int(ceil(float(_atlas.tile_resolution) / float(LOCAL_X))), 1, 1)
	rd.compute_list_end()
	call_deferred("_finish_handoff", request_id, source_slot, destination_slot, OK)


func _finish_handoff(request_id: int, source_slot: int,
		destination_slot: int, error: Error) -> void:
	_handoff_pending = false
	if error != OK:
		handoff_failed.emit(request_id, error)
		return
	handoff_recorded.emit(request_id, source_slot, destination_slot)


func _storage_uniform(binding: int, rid: RID) -> RDUniform:
	var u := RDUniform.new()
	u.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
	u.binding = binding
	u.add_id(rid)
	return u


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
	_handoff_pending = false
	_uniform_set = RID(); _params = RID(); _pipeline = RID(); _shader = RID()
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
