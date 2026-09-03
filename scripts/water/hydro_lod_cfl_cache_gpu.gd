class_name HydroLODCFLCacheGPU
extends Node
## GPU-resident per-tile CFL/health + activity summary cache for temporal HydroLOD.
##
## A slot summary is refreshed only when that slot's canonical state changes. The
## entire cache is rebuilt when sparse topology_revision changes because activation
## and conservative LOD transfers may rewrite atlas slots outside the timestep loop.
## The same changed-tile state scan also emits the established 80-byte activity ABI,
## eliminating the separate post-solve HydroTileActivityGPU compute pass.
##
## A persistent GPU live-slot queue is rebuilt with the same topology revision and
## drives CFL/health reduction through indirect dispatch. Fine-clock reduction cost
## therefore scales with resident hydrology slots rather than atlas capacity.

signal initialized
signal initialization_failed(error: Error)
signal released

const SUMMARY_UINTS := 8
const SUMMARY_BYTES_PER_SLOT := SUMMARY_UINTS * 4
const ACTIVITY_SUMMARY_VEC4S := 5
const ACTIVITY_SUMMARY_BYTES_PER_SLOT := ACTIVITY_SUMMARY_VEC4S * 16
const SUMMARY_INDIRECT_BYTES := 16
const LIVE_QUEUE_HEADER_BYTES := 16
const LIVE_SLOT_BYTES := 4
const LIVE_INDIRECT_BYTES := 16
const LIVE_BUILD_LOCAL_X := 64
const REDUCE_LOCAL_X := 256

var atlas: SparseHydroAtlasGPU

var _control := RID()
var _params := RID()
var _due_slots := RID()
var _due_indirect := RID()
var _summaries := RID()
var _activity_summaries := RID()
var _summary_indirect := RID()
var _live_queue := RID()
var _live_indirect := RID()

var _refresh_shader := RID()
var _refresh_pipeline := RID()
var _zero_due_shader := RID()
var _zero_due_pipeline := RID()
var _reduce_shader := RID()
var _reduce_pipeline := RID()
var _summary_indirect_shader := RID()
var _summary_indirect_pipeline := RID()
var _live_build_shader := RID()
var _live_build_pipeline := RID()
var _live_finalize_shader := RID()
var _live_finalize_pipeline := RID()
var _refresh_set := RID()
var _zero_due_set := RID()
var _reduce_set := RID()
var _summary_indirect_set := RID()
var _live_build_set := RID()
var _live_finalize_set := RID()

var _initialized := false
var _init_pending := false
var _synced_topology_revision := -1


func initialize(p_atlas: SparseHydroAtlasGPU, control_rid: RID, params_rid: RID,
		due_slots_rid: RID, due_indirect_rid: RID) -> Error:
	if _initialized or _init_pending:
		return ERR_BUSY
	if p_atlas == null or not p_atlas.initialized_ok() \
			or not control_rid.is_valid() or not params_rid.is_valid() \
			or not due_slots_rid.is_valid() or not due_indirect_rid.is_valid():
		return ERR_INVALID_PARAMETER
	if RenderingServer.get_rendering_device() == null:
		return ERR_UNAVAILABLE

	var refresh_file: RDShaderFile = load(
		"res://shaders/water/hydro_lod_cfl_cache_refresh.glsl")
	var zero_file: RDShaderFile = load(
		"res://shaders/water/hydro_lod_cfl_cache_zero_due.glsl")
	var reduce_file: RDShaderFile = load(
		"res://shaders/water/hydro_lod_cfl_cache_reduce.glsl")
	var indirect_file: RDShaderFile = load(
		"res://shaders/water/hydro_lod_summary_indirect.glsl")
	var live_build_file: RDShaderFile = load(
		"res://shaders/water/hydro_lod_live_slots_build.glsl")
	var live_finalize_file: RDShaderFile = load(
		"res://shaders/water/hydro_lod_live_slots_finalize.glsl")
	if refresh_file == null or zero_file == null or reduce_file == null \
			or indirect_file == null or live_build_file == null \
			or live_finalize_file == null:
		return ERR_CANT_OPEN
	var refresh_spirv := refresh_file.get_spirv()
	var zero_spirv := zero_file.get_spirv()
	var reduce_spirv := reduce_file.get_spirv()
	var indirect_spirv := indirect_file.get_spirv()
	var live_build_spirv := live_build_file.get_spirv()
	var live_finalize_spirv := live_finalize_file.get_spirv()
	if refresh_spirv == null or zero_spirv == null or reduce_spirv == null \
			or indirect_spirv == null or live_build_spirv == null \
			or live_finalize_spirv == null:
		return ERR_CANT_CREATE

	atlas = p_atlas
	_control = control_rid
	_params = params_rid
	_due_slots = due_slots_rid
	_due_indirect = due_indirect_rid
	_init_pending = true
	RenderingServer.call_on_render_thread(Callable(self, &"_init_render_thread").bind(
		refresh_spirv, zero_spirv, reduce_spirv, indirect_spirv,
		live_build_spirv, live_finalize_spirv))
	return OK


func initialized_ok() -> bool:
	return _initialized


func activity_summary_rid() -> RID:
	return _activity_summaries if _initialized else RID()


func needs_rebuild(topology_revision: int) -> bool:
	return not _initialized or topology_revision < 0 \
		or topology_revision != _synced_topology_revision


## Render-thread only. Must be called before compute_list_begin(). The live queue
## header is reset here; stale payload slots are harmless because count owns validity.
func clear_for_full_rebuild(rd: RenderingDevice) -> Error:
	if not _initialized or not _summaries.is_valid() or not _activity_summaries.is_valid() \
			or not _live_queue.is_valid() or not _live_indirect.is_valid():
		return ERR_UNAVAILABLE
	var err := rd.buffer_clear(_summaries, 0, atlas.capacity * SUMMARY_BYTES_PER_SLOT)
	if err != OK:
		return err
	err = rd.buffer_clear(_activity_summaries, 0,
		atlas.capacity * ACTIVITY_SUMMARY_BYTES_PER_SLOT)
	if err != OK:
		return err
	err = rd.buffer_clear(_live_queue, 0, LIVE_QUEUE_HEADER_BYTES)
	if err != OK:
		return err
	return rd.buffer_clear(_live_indirect, 0, LIVE_INDIRECT_BYTES)


## Render-thread only. State A -> all occupied CFL + activity tile summaries, then
## rebuild the persistent occupied-slot queue for this topology revision.
func record_full_refresh(rd: RenderingDevice, compute: int) -> void:
	if not _initialized:
		return
	rd.compute_list_bind_compute_pipeline(compute, _refresh_pipeline)
	rd.compute_list_bind_uniform_set(compute, _refresh_set, 0)
	_set_push(rd, compute, 0)
	rd.compute_list_dispatch(compute, 1, 1, atlas.capacity)
	rd.compute_list_add_barrier(compute)

	rd.compute_list_bind_compute_pipeline(compute, _live_build_pipeline)
	rd.compute_list_bind_uniform_set(compute, _live_build_set, 0)
	var live_groups := maxi(int(ceil(float(atlas.capacity) / float(LIVE_BUILD_LOCAL_X))), 1)
	rd.compute_list_dispatch(compute, live_groups, 1, 1)
	rd.compute_list_add_barrier(compute)

	rd.compute_list_bind_compute_pipeline(compute, _live_finalize_pipeline)
	rd.compute_list_bind_uniform_set(compute, _live_finalize_set, 0)
	rd.compute_list_dispatch(compute, 1, 1, 1)
	rd.compute_list_add_barrier(compute)


func mark_topology_synced(topology_revision: int) -> void:
	_synced_topology_revision = topology_revision


## Render-thread only. Derive the one-group-per-tile summary command from the SWE
## due count and clear the old CFL records before those exact states are advanced.
## Activity records may remain visible during the step; no consumer observes them
## until the solver command list has completed and the due refresh has overwritten them.
func record_zero_due(rd: RenderingDevice, compute: int) -> void:
	if not _initialized:
		return
	rd.compute_list_bind_compute_pipeline(compute, _summary_indirect_pipeline)
	rd.compute_list_bind_uniform_set(compute, _summary_indirect_set, 0)
	rd.compute_list_dispatch(compute, 1, 1, 1)
	rd.compute_list_add_barrier(compute)

	rd.compute_list_bind_compute_pipeline(compute, _zero_due_pipeline)
	rd.compute_list_bind_uniform_set(compute, _zero_due_set, 0)
	rd.compute_list_dispatch_indirect(compute, _summary_indirect, 0)
	rd.compute_list_add_barrier(compute)


## Render-thread only. Recompute both products only for tiles whose canonical state
## was just committed. One 64-thread workgroup scans one queued tile.
func record_refresh_due(rd: RenderingDevice, compute: int) -> void:
	if not _initialized:
		return
	rd.compute_list_bind_compute_pipeline(compute, _refresh_pipeline)
	rd.compute_list_bind_uniform_set(compute, _refresh_set, 0)
	_set_push(rd, compute, 1)
	rd.compute_list_dispatch_indirect(compute, _summary_indirect, 0)
	rd.compute_list_add_barrier(compute)


## Render-thread only. mode 0 -> CFL iteration scratch, mode 1 -> post diagnostics.
## Dispatch size comes from the topology-persistent live queue, not atlas capacity.
func record_reduce(rd: RenderingDevice, compute: int, mode: int) -> void:
	if not _initialized:
		return
	rd.compute_list_bind_compute_pipeline(compute, _reduce_pipeline)
	rd.compute_list_bind_uniform_set(compute, _reduce_set, 0)
	_set_push(rd, compute, mode)
	rd.compute_list_dispatch_indirect(compute, _live_indirect, 0)
	rd.compute_list_add_barrier(compute)


func invalidate() -> void:
	_synced_topology_revision = -1


func gpu_bytes_estimate() -> int:
	if atlas == null:
		return 0
	return atlas.capacity * (SUMMARY_BYTES_PER_SLOT + ACTIVITY_SUMMARY_BYTES_PER_SLOT \
		+ LIVE_SLOT_BYTES) + SUMMARY_INDIRECT_BYTES + LIVE_QUEUE_HEADER_BYTES \
		+ LIVE_INDIRECT_BYTES


func stats() -> Dictionary:
	return {
		"initialized": _initialized,
		"synced_topology_revision": _synced_topology_revision,
		"summary_bytes_per_slot": SUMMARY_BYTES_PER_SLOT,
		"activity_summary_bytes_per_slot": ACTIVITY_SUMMARY_BYTES_PER_SLOT,
		"gpu_bytes": gpu_bytes_estimate(),
		"per_tile_characteristic_rate_cache": true,
		"fused_activity_summary_cache": true,
		"activity_abi_compatible": true,
		"due_only_refresh": true,
		"one_cell_scan_for_cfl_and_activity": true,
		"full_rebuild_on_topology_revision": true,
		"persistent_live_slot_queue": true,
		"live_slot_queue_rebuild_on_topology_revision": true,
		"cfl_reduction_scope": "live_slots",
		"cfl_reduction_indirect_dispatch": true,
		"fine_tick_capacity_scan": false,
		"cpu_cfl_summary_readback": false,
	}


func _init_render_thread(refresh_spirv: RDShaderSPIRV, zero_spirv: RDShaderSPIRV,
		reduce_spirv: RDShaderSPIRV, indirect_spirv: RDShaderSPIRV,
		live_build_spirv: RDShaderSPIRV, live_finalize_spirv: RDShaderSPIRV) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null:
		call_deferred(&"_finish_init", ERR_UNAVAILABLE, {})
		return

	var refresh_shader := rd.shader_create_from_spirv(refresh_spirv)
	var zero_shader := rd.shader_create_from_spirv(zero_spirv)
	var reduce_shader := rd.shader_create_from_spirv(reduce_spirv)
	var indirect_shader := rd.shader_create_from_spirv(indirect_spirv)
	var live_build_shader := rd.shader_create_from_spirv(live_build_spirv)
	var live_finalize_shader := rd.shader_create_from_spirv(live_finalize_spirv)
	if not refresh_shader.is_valid() or not zero_shader.is_valid() \
			or not reduce_shader.is_valid() or not indirect_shader.is_valid() \
			or not live_build_shader.is_valid() or not live_finalize_shader.is_valid():
		_free_many(rd, [live_finalize_shader, live_build_shader, indirect_shader,
			reduce_shader, zero_shader, refresh_shader])
		call_deferred(&"_finish_init", ERR_CANT_CREATE, {})
		return
	var refresh_pipeline := rd.compute_pipeline_create(refresh_shader)
	var zero_pipeline := rd.compute_pipeline_create(zero_shader)
	var reduce_pipeline := rd.compute_pipeline_create(reduce_shader)
	var indirect_pipeline := rd.compute_pipeline_create(indirect_shader)
	var live_build_pipeline := rd.compute_pipeline_create(live_build_shader)
	var live_finalize_pipeline := rd.compute_pipeline_create(live_finalize_shader)
	if not refresh_pipeline.is_valid() or not zero_pipeline.is_valid() \
			or not reduce_pipeline.is_valid() or not indirect_pipeline.is_valid() \
			or not live_build_pipeline.is_valid() or not live_finalize_pipeline.is_valid():
		_free_many(rd, [live_finalize_pipeline, live_build_pipeline, indirect_pipeline,
			reduce_pipeline, zero_pipeline, refresh_pipeline, live_finalize_shader,
			live_build_shader, indirect_shader, reduce_shader, zero_shader, refresh_shader])
		call_deferred(&"_finish_init", ERR_CANT_CREATE, {})
		return

	var cfl_zero_bytes := PackedByteArray()
	cfl_zero_bytes.resize(atlas.capacity * SUMMARY_BYTES_PER_SLOT)
	var activity_zero_bytes := PackedByteArray()
	activity_zero_bytes.resize(atlas.capacity * ACTIVITY_SUMMARY_BYTES_PER_SLOT)
	var indirect_zero_bytes := PackedByteArray()
	indirect_zero_bytes.resize(SUMMARY_INDIRECT_BYTES)
	var live_queue_zero := PackedByteArray()
	live_queue_zero.resize(LIVE_QUEUE_HEADER_BYTES + atlas.capacity * LIVE_SLOT_BYTES)
	var live_indirect_zero := PackedByteArray()
	live_indirect_zero.resize(LIVE_INDIRECT_BYTES)
	var summaries := rd.storage_buffer_create(cfl_zero_bytes.size(), cfl_zero_bytes)
	var activity_summaries := rd.storage_buffer_create(
		activity_zero_bytes.size(), activity_zero_bytes)
	var summary_indirect := rd.storage_buffer_create(SUMMARY_INDIRECT_BYTES,
		indirect_zero_bytes, RenderingDevice.STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT)
	var live_queue := rd.storage_buffer_create(live_queue_zero.size(), live_queue_zero)
	var live_indirect := rd.storage_buffer_create(LIVE_INDIRECT_BYTES,
		live_indirect_zero, RenderingDevice.STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT)
	if not summaries.is_valid() or not activity_summaries.is_valid() \
			or not summary_indirect.is_valid() or not live_queue.is_valid() \
			or not live_indirect.is_valid():
		_free_many(rd, [live_indirect, live_queue, summary_indirect, activity_summaries,
			summaries, live_finalize_pipeline, live_build_pipeline, indirect_pipeline,
			reduce_pipeline, zero_pipeline, refresh_pipeline, live_finalize_shader,
			live_build_shader, indirect_shader, reduce_shader, zero_shader, refresh_shader])
		call_deferred(&"_finish_init", ERR_CANT_CREATE, {})
		return

	var refresh_set := rd.uniform_set_create([
		_storage_uniform(0, atlas.state_a_rid()),
		_storage_uniform(1, atlas.occupancy_rid()),
		_storage_uniform(2, atlas.tile_metadata_rid()),
		_storage_uniform(3, _params),
		_storage_uniform(4, _due_slots),
		_storage_uniform(5, summaries),
		_storage_uniform(6, activity_summaries),
	], refresh_shader, 0)
	var zero_set := rd.uniform_set_create([
		_storage_uniform(0, _due_slots),
		_storage_uniform(1, summaries),
		_storage_uniform(2, _params),
	], zero_shader, 0)
	var reduce_set := rd.uniform_set_create([
		_storage_uniform(0, summaries),
		_storage_uniform(1, live_queue),
		_storage_uniform(2, _control),
		_storage_uniform(3, _params),
	], reduce_shader, 0)
	var summary_indirect_set := rd.uniform_set_create([
		_storage_uniform(0, _due_indirect),
		_storage_uniform(1, summary_indirect),
	], indirect_shader, 0)
	var live_build_set := rd.uniform_set_create([
		_storage_uniform(0, atlas.occupancy_rid()),
		_storage_uniform(1, live_queue),
		_storage_uniform(2, _params),
	], live_build_shader, 0)
	var live_finalize_set := rd.uniform_set_create([
		_storage_uniform(0, live_queue),
		_storage_uniform(1, live_indirect),
		_storage_uniform(2, _params),
	], live_finalize_shader, 0)
	if not refresh_set.is_valid() or not zero_set.is_valid() \
			or not reduce_set.is_valid() or not summary_indirect_set.is_valid() \
			or not live_build_set.is_valid() or not live_finalize_set.is_valid():
		_free_many(rd, [live_finalize_set, live_build_set, summary_indirect_set,
			reduce_set, zero_set, refresh_set, live_indirect, live_queue,
			summary_indirect, activity_summaries, summaries, live_finalize_pipeline,
			live_build_pipeline, indirect_pipeline, reduce_pipeline, zero_pipeline,
			refresh_pipeline, live_finalize_shader, live_build_shader, indirect_shader,
			reduce_shader, zero_shader, refresh_shader])
		call_deferred(&"_finish_init", ERR_CANT_CREATE, {})
		return

	call_deferred(&"_finish_init", OK, {
		"refresh_shader": refresh_shader,
		"refresh_pipeline": refresh_pipeline,
		"zero_shader": zero_shader,
		"zero_pipeline": zero_pipeline,
		"reduce_shader": reduce_shader,
		"reduce_pipeline": reduce_pipeline,
		"indirect_shader": indirect_shader,
		"indirect_pipeline": indirect_pipeline,
		"live_build_shader": live_build_shader,
		"live_build_pipeline": live_build_pipeline,
		"live_finalize_shader": live_finalize_shader,
		"live_finalize_pipeline": live_finalize_pipeline,
		"summaries": summaries,
		"activity_summaries": activity_summaries,
		"summary_indirect": summary_indirect,
		"live_queue": live_queue,
		"live_indirect": live_indirect,
		"refresh_set": refresh_set,
		"zero_set": zero_set,
		"reduce_set": reduce_set,
		"summary_indirect_set": summary_indirect_set,
		"live_build_set": live_build_set,
		"live_finalize_set": live_finalize_set,
	})


func _finish_init(error: Error, bundle: Dictionary) -> void:
	_init_pending = false
	if error != OK:
		initialization_failed.emit(error)
		return
	_refresh_shader = bundle["refresh_shader"]
	_refresh_pipeline = bundle["refresh_pipeline"]
	_zero_due_shader = bundle["zero_shader"]
	_zero_due_pipeline = bundle["zero_pipeline"]
	_reduce_shader = bundle["reduce_shader"]
	_reduce_pipeline = bundle["reduce_pipeline"]
	_summary_indirect_shader = bundle["indirect_shader"]
	_summary_indirect_pipeline = bundle["indirect_pipeline"]
	_live_build_shader = bundle["live_build_shader"]
	_live_build_pipeline = bundle["live_build_pipeline"]
	_live_finalize_shader = bundle["live_finalize_shader"]
	_live_finalize_pipeline = bundle["live_finalize_pipeline"]
	_summaries = bundle["summaries"]
	_activity_summaries = bundle["activity_summaries"]
	_summary_indirect = bundle["summary_indirect"]
	_live_queue = bundle["live_queue"]
	_live_indirect = bundle["live_indirect"]
	_refresh_set = bundle["refresh_set"]
	_zero_due_set = bundle["zero_set"]
	_reduce_set = bundle["reduce_set"]
	_summary_indirect_set = bundle["summary_indirect_set"]
	_live_build_set = bundle["live_build_set"]
	_live_finalize_set = bundle["live_finalize_set"]
	_initialized = true
	initialized.emit()


func _set_push(rd: RenderingDevice, compute: int, value: int) -> void:
	var push := PackedInt32Array([value, 0, 0, 0]).to_byte_array()
	rd.compute_list_set_push_constant(compute, push, push.size())


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
	if not _initialized and not _refresh_shader.is_valid():
		return
	var rids := [
		_live_finalize_set, _live_build_set, _summary_indirect_set, _reduce_set,
		_zero_due_set, _refresh_set, _live_indirect, _live_queue, _summary_indirect,
		_activity_summaries, _summaries, _live_finalize_pipeline, _live_build_pipeline,
		_summary_indirect_pipeline, _reduce_pipeline, _zero_due_pipeline, _refresh_pipeline,
		_live_finalize_shader, _live_build_shader, _summary_indirect_shader,
		_reduce_shader, _zero_due_shader, _refresh_shader,
	]
	_initialized = false
	_init_pending = false
	_synced_topology_revision = -1
	_live_finalize_set = RID(); _live_build_set = RID()
	_summary_indirect_set = RID(); _reduce_set = RID()
	_zero_due_set = RID(); _refresh_set = RID()
	_live_indirect = RID(); _live_queue = RID()
	_summary_indirect = RID(); _activity_summaries = RID(); _summaries = RID()
	_live_finalize_pipeline = RID(); _live_build_pipeline = RID()
	_summary_indirect_pipeline = RID(); _reduce_pipeline = RID()
	_zero_due_pipeline = RID(); _refresh_pipeline = RID()
	_live_finalize_shader = RID(); _live_build_shader = RID()
	_summary_indirect_shader = RID(); _reduce_shader = RID()
	_zero_due_shader = RID(); _refresh_shader = RID()
	_control = RID(); _params = RID(); _due_slots = RID(); _due_indirect = RID()
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
