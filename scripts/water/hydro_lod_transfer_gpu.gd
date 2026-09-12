class_name HydroLODTransferGPU
extends Node
## Conservative 2:1 physical-HydroLOD state transfer on the shared sparse atlas.
##
## Restriction: four published children -> one hidden parent.
## Prolongation: one published parent -> four hidden, terrain-staged children.
##
## Both operations write canonical A+B and clear destination source layers. They do
## not publish/unpublish ownership; HydroPhysicalLODManager owns that transaction.

signal initialized
signal initialization_failed(error: Error)
signal transfer_recorded(request_id: int, mode: String, parent_slot: int,
	child_slots: PackedInt32Array)
signal transfer_failed(request_id: int, error: Error)
signal released

const LOCAL_X := 8
const LOCAL_Y := 8
const PARAM_BYTES := 32

var atlas: SparseHydroAtlasGPU
var atmospheric_source_rid := RID()

var _restrict_shader := RID()
var _restrict_pipeline := RID()
var _prolong_shader := RID()
var _prolong_pipeline := RID()
var _params := RID()
var _restrict_set := RID()
var _prolong_set := RID()
var _initialized := false
var _init_pending := false
var _pending := false
var _next_request_id := 1


func initialize(p_atlas: SparseHydroAtlasGPU,
		p_atmospheric_source_rid: RID) -> Error:
	if _initialized or _init_pending or _pending:
		return ERR_BUSY
	if p_atlas == null or not p_atlas.initialized_ok() \
			or not p_atmospheric_source_rid.is_valid():
		return ERR_INVALID_PARAMETER
	if RenderingServer.get_rendering_device() == null:
		return ERR_UNAVAILABLE
	var restrict_file: RDShaderFile = load("res://shaders/water/hydro_lod_restrict.glsl")
	var prolong_file: RDShaderFile = load("res://shaders/water/hydro_lod_prolong.glsl")
	if restrict_file == null or prolong_file == null \
			or restrict_file.get_spirv() == null or prolong_file.get_spirv() == null:
		return ERR_CANT_OPEN
	atlas = p_atlas
	atmospheric_source_rid = p_atmospheric_source_rid
	_init_pending = true
	RenderingServer.call_on_render_thread(Callable(self, &"_init_render_thread").bind(
		restrict_file.get_spirv(), prolong_file.get_spirv()))
	return OK


func initialized_ok() -> bool:
	return _initialized


func pending() -> bool:
	return _pending


func restrict(parent_slot: int, child_slots: PackedInt32Array) -> int:
	return _submit("restrict", parent_slot, child_slots)


func prolong(parent_slot: int, child_slots: PackedInt32Array) -> int:
	return _submit("prolong", parent_slot, child_slots)


func _submit(mode: String, parent_slot: int,
		child_slots: PackedInt32Array) -> int:
	if not _initialized or _pending or atlas == null or not atlas.initialized_ok() \
			or mode not in ["restrict", "prolong"]:
		return -1
	if parent_slot < 0 or parent_slot >= atlas.capacity or child_slots.size() != 4:
		return -1
	var seen: Dictionary = {parent_slot: true}
	for slot in child_slots:
		if slot < 0 or slot >= atlas.capacity or seen.has(int(slot)):
			return -1
		seen[int(slot)] = true
	var bytes := PackedByteArray()
	bytes.resize(PARAM_BYTES)
	bytes.encode_u32(0, parent_slot)
	bytes.encode_u32(4, child_slots[0])
	bytes.encode_u32(8, child_slots[1])
	bytes.encode_u32(12, child_slots[2])
	bytes.encode_u32(16, child_slots[3])
	bytes.encode_u32(20, atlas.tile_resolution)
	bytes.encode_u32(24, atlas.capacity)
	bytes.encode_u32(28, 0)
	var request_id := _next_request_id
	_next_request_id += 1
	_pending = true
	RenderingServer.call_on_render_thread(Callable(self, &"_transfer_render_thread").bind(
		request_id, mode, parent_slot, child_slots.duplicate(), bytes))
	return request_id


func _init_render_thread(restrict_spirv: RDShaderSPIRV,
		prolong_spirv: RDShaderSPIRV) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null:
		call_deferred(&"_finish_init", ERR_UNAVAILABLE, {})
		return
	var restrict_shader := rd.shader_create_from_spirv(restrict_spirv)
	var prolong_shader := rd.shader_create_from_spirv(prolong_spirv)
	var restrict_pipeline := rd.compute_pipeline_create(restrict_shader) \
		if restrict_shader.is_valid() else RID()
	var prolong_pipeline := rd.compute_pipeline_create(prolong_shader) \
		if prolong_shader.is_valid() else RID()
	var zero := PackedByteArray(); zero.resize(PARAM_BYTES)
	var params := rd.storage_buffer_create(PARAM_BYTES, zero)
	if not restrict_shader.is_valid() or not prolong_shader.is_valid() \
			or not restrict_pipeline.is_valid() or not prolong_pipeline.is_valid() \
			or not params.is_valid():
		_free_many(rd, [params, prolong_pipeline, restrict_pipeline,
			prolong_shader, restrict_shader])
		call_deferred(&"_finish_init", ERR_CANT_CREATE, {})
		return
	var uniforms := [
		_storage_uniform(0, atlas.state_a_rid()),
		_storage_uniform(1, atlas.state_a_rid()),
		_storage_uniform(2, atlas.state_b_rid()),
		_storage_uniform(3, atlas.source_rid()),
		_storage_uniform(4, atmospheric_source_rid),
		_storage_uniform(5, atlas.occupancy_rid()),
		_storage_uniform(6, params),
	]
	var restrict_set := rd.uniform_set_create(uniforms, restrict_shader, 0)
	var prolong_set := rd.uniform_set_create(uniforms, prolong_shader, 0)
	if not restrict_set.is_valid() or not prolong_set.is_valid():
		_free_many(rd, [prolong_set, restrict_set, params,
			prolong_pipeline, restrict_pipeline, prolong_shader, restrict_shader])
		call_deferred(&"_finish_init", ERR_CANT_CREATE, {})
		return
	call_deferred(&"_finish_init", OK, {
		"restrict_shader": restrict_shader,
		"restrict_pipeline": restrict_pipeline,
		"prolong_shader": prolong_shader,
		"prolong_pipeline": prolong_pipeline,
		"params": params,
		"restrict_set": restrict_set,
		"prolong_set": prolong_set,
	})


func _finish_init(error: Error, bundle: Dictionary) -> void:
	_init_pending = false
	if error != OK:
		initialization_failed.emit(error)
		return
	_restrict_shader = bundle["restrict_shader"]
	_restrict_pipeline = bundle["restrict_pipeline"]
	_prolong_shader = bundle["prolong_shader"]
	_prolong_pipeline = bundle["prolong_pipeline"]
	_params = bundle["params"]
	_restrict_set = bundle["restrict_set"]
	_prolong_set = bundle["prolong_set"]
	_initialized = true
	initialized.emit()


func _transfer_render_thread(request_id: int, mode: String, parent_slot: int,
		child_slots: PackedInt32Array, param_bytes: PackedByteArray) -> void:
	var rd := RenderingServer.get_rendering_device()
	var pipeline := _restrict_pipeline if mode == "restrict" else _prolong_pipeline
	var set_rid := _restrict_set if mode == "restrict" else _prolong_set
	if rd == null or not pipeline.is_valid() or not set_rid.is_valid():
		call_deferred(&"_finish_transfer", request_id, mode, parent_slot,
			child_slots, ERR_UNAVAILABLE)
		return
	var err := rd.buffer_update(_params, 0, PARAM_BYTES, param_bytes)
	if err != OK:
		call_deferred(&"_finish_transfer", request_id, mode, parent_slot,
			child_slots, err)
		return
	var groups := int(ceil(float(atlas.tile_resolution) / float(LOCAL_X)))
	var compute := rd.compute_list_begin()
	rd.compute_list_bind_compute_pipeline(compute, pipeline)
	rd.compute_list_bind_uniform_set(compute, set_rid, 0)
	rd.compute_list_dispatch(compute, groups, groups, 1)
	rd.compute_list_end()
	call_deferred(&"_finish_transfer", request_id, mode, parent_slot,
		child_slots, OK)


func _finish_transfer(request_id: int, mode: String, parent_slot: int,
		child_slots: PackedInt32Array, error: Error) -> void:
	_pending = false
	if error != OK:
		transfer_failed.emit(request_id, error)
	else:
		transfer_recorded.emit(request_id, mode, parent_slot, child_slots)


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
	if not _initialized and not _restrict_shader.is_valid():
		return
	var rids := [_restrict_set, _prolong_set, _params,
		_restrict_pipeline, _prolong_pipeline, _restrict_shader, _prolong_shader]
	_initialized = false
	_init_pending = false
	_pending = false
	_restrict_set = RID(); _prolong_set = RID(); _params = RID()
	_restrict_pipeline = RID(); _prolong_pipeline = RID()
	_restrict_shader = RID(); _prolong_shader = RID()
	atmospheric_source_rid = RID(); atlas = null
	RenderingServer.call_on_render_thread(
		Callable(self, &"_release_render_thread").bind(rids))
	released.emit()


func _release_render_thread(rids: Array) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd != null:
		_free_many(rd, rids)


func _exit_tree() -> void:
	release()
