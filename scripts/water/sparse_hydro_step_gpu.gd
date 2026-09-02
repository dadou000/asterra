class_name SparseHydroStepGPU
extends Node
## Connected same-level sparse SWE dispatcher.
##
## The atlas owns state/source/occupancy storage and SparseHydroConnectivityGPU
## owns exact W/E/S/N resident links. This class only records one explicit,
## caller-CFL-safe substep. State A is canonical: the solver writes A -> B, then
## commits B -> A on-GPU so existing activity/frontier/reconstruction passes never
## need to track ping-pong parity.
##
## IMPORTANT: substep() is deliberately low-level. The caller must supply a dt
## satisfying the current sparse-domain CFL limit. A later Phase 3 scheduler will
## move the adaptive reduction/prepare loop entirely onto the GPU, as FixedHydroGPU
## already does for the single fixed domain.

signal initialized
signal initialization_failed(error: Error)
signal substep_recorded(step_id: int)
signal released

const LOCAL_X := 8
const LOCAL_Y := 8
const COMMIT_LOCAL_X := 256
const STEP_PARAM_FLOATS := 8
const COMMIT_PARAM_INTS := 4

var gravity := 9.81
var dry_eps := 1.0e-5
var manning_n := 0.025

var _atlas: SparseHydroAtlasGPU
var _connectivity: SparseHydroConnectivityGPU

var _step_shader := RID()
var _step_pipeline := RID()
var _commit_shader := RID()
var _commit_pipeline := RID()
var _step_params := RID()
var _commit_params := RID()
var _step_set := RID()
var _commit_set := RID()

var _initialized := false
var _init_pending := false
var _step_pending := false
var _next_step_id := 1


func initialize(atlas: SparseHydroAtlasGPU,
		connectivity: SparseHydroConnectivityGPU) -> Error:
	if _initialized or _init_pending or _step_pending:
		return ERR_BUSY
	if atlas == null or connectivity == null \
			or not atlas.initialized_ok() or not connectivity.initialized_ok():
		return ERR_INVALID_PARAMETER
	if atlas.capacity != connectivity.capacity:
		return ERR_INVALID_PARAMETER
	if RenderingServer.get_rendering_device() == null:
		return ERR_UNAVAILABLE

	var step_file: RDShaderFile = load("res://shaders/water/sparse_hydro_step.glsl")
	var commit_file: RDShaderFile = load("res://shaders/water/sparse_hydro_commit.glsl")
	if step_file == null or commit_file == null:
		return ERR_CANT_OPEN
	var step_spirv := step_file.get_spirv()
	var commit_spirv := commit_file.get_spirv()
	if step_spirv == null or commit_spirv == null:
		return ERR_CANT_CREATE

	_atlas = atlas
	_connectivity = connectivity
	_init_pending = true
	RenderingServer.call_on_render_thread(Callable(self, &"_init_render_thread").bind(
		step_spirv, commit_spirv))
	return OK


func initialized_ok() -> bool:
	return _initialized


func step_pending() -> bool:
	return _step_pending


func canonical_state_rid() -> RID:
	return _atlas.state_a_rid() if _initialized and _atlas != null else RID()


func substep(dt_s: float) -> int:
	if not _initialized or _step_pending or not is_finite(dt_s) or dt_s <= 0.0:
		return -1
	if _atlas == null or _connectivity == null \
			or not _atlas.initialized_ok() or not _connectivity.initialized_ok():
		return -1

	var step_id := _next_step_id
	_next_step_id += 1
	_step_pending = true
	var params := PackedFloat32Array([
		float(_atlas.tile_resolution), float(_atlas.capacity),
		_atlas.cell_size_m, dt_s,
		gravity, dry_eps, manning_n, 0.0,
	])
	RenderingServer.call_on_render_thread(Callable(self, &"_substep_render_thread").bind(
		step_id, params.to_byte_array()))
	return step_id


func gpu_bytes_estimate() -> int:
	return STEP_PARAM_FLOATS * 4 + COMMIT_PARAM_INTS * 4


func stats() -> Dictionary:
	return {
		"initialized": _initialized,
		"initialization_pending": _init_pending,
		"step_pending": _step_pending,
		"gravity": gravity,
		"dry_eps": dry_eps,
		"manning_n": manning_n,
		"canonical_state": "atlas_A",
		"gpu_bytes_owned": gpu_bytes_estimate(),
	}


func _init_render_thread(step_spirv: RDShaderSPIRV,
		commit_spirv: RDShaderSPIRV) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null:
		call_deferred("_finish_init", ERR_UNAVAILABLE, {})
		return

	var step_shader := rd.shader_create_from_spirv(step_spirv)
	var commit_shader := rd.shader_create_from_spirv(commit_spirv)
	if not step_shader.is_valid() or not commit_shader.is_valid():
		_free_many(rd, [step_shader, commit_shader])
		call_deferred("_finish_init", ERR_CANT_CREATE, {})
		return
	var step_pipeline := rd.compute_pipeline_create(step_shader)
	var commit_pipeline := rd.compute_pipeline_create(commit_shader)
	if not step_pipeline.is_valid() or not commit_pipeline.is_valid():
		_free_many(rd, [step_pipeline, commit_pipeline, step_shader, commit_shader])
		call_deferred("_finish_init", ERR_CANT_CREATE, {})
		return

	var zero_step := PackedByteArray()
	zero_step.resize(STEP_PARAM_FLOATS * 4)
	var commit_values := PackedInt32Array([
		_atlas.total_cell_count(), _atlas.cells_per_tile(), _atlas.capacity, 0,
	])
	var step_params := rd.storage_buffer_create(zero_step.size(), zero_step)
	var commit_params := rd.storage_buffer_create(
		commit_values.to_byte_array().size(), commit_values.to_byte_array())
	if not step_params.is_valid() or not commit_params.is_valid():
		_free_many(rd, [step_params, commit_params, step_pipeline, commit_pipeline,
			step_shader, commit_shader])
		call_deferred("_finish_init", ERR_CANT_CREATE, {})
		return

	var step_set := rd.uniform_set_create([
		_storage_uniform(0, _atlas.state_a_rid()),
		_storage_uniform(1, _atlas.state_b_rid()),
		_storage_uniform(2, _atlas.source_rid()),
		_storage_uniform(3, _atlas.occupancy_rid()),
		_storage_uniform(4, _connectivity.neighbor_slots_rid()),
		_storage_uniform(5, _connectivity.neighbor_links_rid()),
		_storage_uniform(6, step_params),
	], step_shader, 0)
	var commit_set := rd.uniform_set_create([
		_storage_uniform(0, _atlas.state_b_rid()),
		_storage_uniform(1, _atlas.state_a_rid()),
		_storage_uniform(2, _atlas.occupancy_rid()),
		_storage_uniform(3, commit_params),
	], commit_shader, 0)
	if not step_set.is_valid() or not commit_set.is_valid():
		_free_many(rd, [step_set, commit_set, step_params, commit_params,
			step_pipeline, commit_pipeline, step_shader, commit_shader])
		call_deferred("_finish_init", ERR_CANT_CREATE, {})
		return

	call_deferred("_finish_init", OK, {
		"step_shader": step_shader,
		"step_pipeline": step_pipeline,
		"commit_shader": commit_shader,
		"commit_pipeline": commit_pipeline,
		"step_params": step_params,
		"commit_params": commit_params,
		"step_set": step_set,
		"commit_set": commit_set,
	})


func _finish_init(error: Error, bundle: Dictionary) -> void:
	_init_pending = false
	if error != OK:
		_initialized = false
		initialization_failed.emit(error)
		return
	_step_shader = bundle["step_shader"]
	_step_pipeline = bundle["step_pipeline"]
	_commit_shader = bundle["commit_shader"]
	_commit_pipeline = bundle["commit_pipeline"]
	_step_params = bundle["step_params"]
	_commit_params = bundle["commit_params"]
	_step_set = bundle["step_set"]
	_commit_set = bundle["commit_set"]
	_initialized = true
	initialized.emit()


func _substep_render_thread(step_id: int, param_bytes: PackedByteArray) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null or not _all_runtime_rids_valid():
		call_deferred("_finish_substep", step_id, false)
		return
	var err := rd.buffer_update(_step_params, 0, param_bytes.size(), param_bytes)
	if err != OK:
		call_deferred("_finish_substep", step_id, false)
		return

	var groups_x := int(ceil(float(_atlas.tile_resolution) / float(LOCAL_X)))
	var groups_y := int(ceil(float(_atlas.tile_resolution) / float(LOCAL_Y)))
	var commit_groups := int(ceil(float(_atlas.total_cell_count()) / float(COMMIT_LOCAL_X)))
	var compute := rd.compute_list_begin()
	rd.compute_list_bind_compute_pipeline(compute, _step_pipeline)
	rd.compute_list_bind_uniform_set(compute, _step_set, 0)
	rd.compute_list_dispatch(compute, groups_x, groups_y, _atlas.capacity)
	rd.compute_list_add_barrier(compute)
	rd.compute_list_bind_compute_pipeline(compute, _commit_pipeline)
	rd.compute_list_bind_uniform_set(compute, _commit_set, 0)
	rd.compute_list_dispatch(compute, commit_groups, 1, 1)
	rd.compute_list_end()
	call_deferred("_finish_substep", step_id, true)


func _finish_substep(step_id: int, success: bool) -> void:
	_step_pending = false
	if not success:
		push_error("SparseHydroStepGPU: failed to record sparse substep %d" % step_id)
		return
	substep_recorded.emit(step_id)


func _all_runtime_rids_valid() -> bool:
	return _step_pipeline.is_valid() and _commit_pipeline.is_valid() \
		and _step_set.is_valid() and _commit_set.is_valid() \
		and _step_params.is_valid() and _commit_params.is_valid()


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
	if not _initialized and not _step_shader.is_valid():
		return
	var rids := [_step_set, _commit_set, _step_params, _commit_params,
		_step_pipeline, _commit_pipeline, _step_shader, _commit_shader]
	_initialized = false
	_init_pending = false
	_step_pending = false
	_step_set = RID(); _commit_set = RID()
	_step_params = RID(); _commit_params = RID()
	_step_pipeline = RID(); _commit_pipeline = RID()
	_step_shader = RID(); _commit_shader = RID()
	_atlas = null
	_connectivity = null
	RenderingServer.call_on_render_thread(Callable(self, &"_release_render_thread").bind(rids))
	released.emit()


func _release_render_thread(rids: Array) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd != null:
		_free_many(rd, rids)


func _exit_tree() -> void:
	release()
