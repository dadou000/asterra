class_name SparseHydroStepGPUSubcycledCached
extends SparseHydroStepGPUSubcycled
## Temporal HydroLOD solver with persistent per-tile CFL/health summaries.
##
## The underlying subcycled solver still owns all SWE, due-queue and interface
## resources. This layer changes only reduction scheduling:
##   topology change -> one full tile-summary rebuild;
##   every fine tick -> reduce cached tile summaries;
##   after commit -> refresh only the due slots.

var cfl_cache: HydroLODCFLCacheGPU
var _cfl_topology_revision := -1


func set_cfl_cache(provider: HydroLODCFLCacheGPU) -> Error:
	if step_pending() or diagnostics_pending():
		return ERR_BUSY
	if provider != null and not provider.initialized_ok():
		return ERR_UNCONFIGURED
	cfl_cache = provider
	return OK


func set_cfl_cache_topology_revision(revision: int) -> void:
	_cfl_topology_revision = revision


func invalidate_cfl_cache() -> void:
	if cfl_cache != null:
		cfl_cache.invalidate()


func params_buffer_rid() -> RID:
	return _params if initialized_ok() else RID()


func due_slots_rid() -> RID:
	return _due_queue_slots if initialized_ok() else RID()


func due_indirect_rid() -> RID:
	return _due_queue_indirect if initialized_ok() else RID()


func gpu_bytes_estimate() -> int:
	return super.gpu_bytes_estimate() + (0 if cfl_cache == null \
		else cfl_cache.gpu_bytes_estimate())


func stats() -> Dictionary:
	var out := super.stats()
	out["cached_cfl_reduction"] = cfl_cache != null and cfl_cache.initialized_ok()
	out["cfl_cache_topology_revision"] = _cfl_topology_revision
	out["full_cell_cfl_scan_each_fine_tick"] = false
	out["full_cell_cache_rebuild_on_topology_change"] = true
	out["fine_tick_cfl_reduction_scope"] = "tile_summaries"
	out["post_commit_cache_refresh_scope"] = "due_slots"
	out["cfl_cache"] = {} if cfl_cache == null else cfl_cache.stats()
	return out


func _advance_render_thread(step_id: int, cap: int, request_diagnostics: bool,
		param_bytes: PackedByteArray) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null or not _all_runtime_rids_valid() or not _due_queue_rids_valid() \
			or temporal_schedule == null or not temporal_schedule.initialized_ok() \
			or cfl_cache == null or not cfl_cache.initialized_ok():
		call_deferred("_finish_advance", step_id, false, request_diagnostics)
		return
	if lod_interface_flux != null and not lod_interface_flux.initialized_ok():
		call_deferred("_finish_advance", step_id, false, request_diagnostics)
		return

	var err := rd.buffer_update(_params, 0, param_bytes.size(), param_bytes)
	var external_ledger_bytes := _atlas.total_cell_count() * EXTERNAL_LEDGER_FLOATS * 4
	var rebuild_cache := cfl_cache.needs_rebuild(_cfl_topology_revision)
	if err == OK and rebuild_cache:
		err = cfl_cache.clear_for_full_rebuild(rd)
	if err != OK or rd.buffer_clear(_control, 0, SUBCYCLED_CONTROL_BYTES) != OK \
			or rd.buffer_clear(_external_flux_ledger, 0, external_ledger_bytes) != OK:
		call_deferred("_finish_advance", step_id, false, request_diagnostics)
		return
	if lod_interface_flux is HydroLODInterfaceFluxSubcycledGPU:
		err = (lod_interface_flux as HydroLODInterfaceFluxSubcycledGPU) \
			.clear_flux_register(rd)
		if err != OK:
			call_deferred("_finish_advance", step_id, false, request_diagnostics)
			return

	var groups_x := int(ceil(float(_atlas.tile_resolution) / float(LOCAL_X)))
	var groups_y := int(ceil(float(_atlas.tile_resolution) / float(LOCAL_Y)))
	var due_queue_groups := maxi(int(ceil(float(_atlas.capacity) \
		/ float(DUE_QUEUE_LOCAL_X))), 1)
	var external_groups := int(ceil(float(_atlas.total_cell_count()) \
		/ float(EXTERNAL_REDUCE_LOCAL_X)))
	var compute := rd.compute_list_begin()

	if rebuild_cache:
		cfl_cache.record_full_refresh(rd, compute)
		cfl_cache.mark_topology_synced(_cfl_topology_revision)

	for iteration in cap:
		rd.compute_list_bind_compute_pipeline(compute, _reset_pipeline)
		rd.compute_list_bind_uniform_set(compute, _reset_set, 0)
		rd.compute_list_dispatch(compute, 1, 1, 1)
		rd.compute_list_add_barrier(compute)

		# CFL preparation consumes cached summaries. This is O(atlas capacity) small
		# records instead of O(capacity * tile_resolution^2) state-cell reads.
		cfl_cache.record_reduce(rd, compute, 0)

		rd.compute_list_bind_compute_pipeline(compute, _prepare_pipeline)
		rd.compute_list_bind_uniform_set(compute, _prepare_set, 0)
		_set_u32_push(rd, compute, iteration)
		rd.compute_list_dispatch(compute, 1, 1, 1)
		rd.compute_list_add_barrier(compute)

		temporal_schedule.record_prepare(rd, compute, iteration)

		rd.compute_list_bind_compute_pipeline(compute, _due_queue_reset_pipeline)
		rd.compute_list_bind_uniform_set(compute, _due_queue_reset_set, 0)
		rd.compute_list_dispatch(compute, 1, 1, 1)
		rd.compute_list_add_barrier(compute)
		rd.compute_list_bind_compute_pipeline(compute, _due_queue_build_pipeline)
		rd.compute_list_bind_uniform_set(compute, _due_queue_build_set, 0)
		rd.compute_list_dispatch(compute, due_queue_groups, 1, 1)
		rd.compute_list_add_barrier(compute)

		# Summaries are cleared before the corresponding state changes. They are not
		# consumed again until after canonical A has been committed and refreshed.
		cfl_cache.record_zero_due(rd, compute)

		rd.compute_list_bind_compute_pipeline(compute, _step_pipeline)
		rd.compute_list_bind_uniform_set(compute, _step_set, 0)
		_set_u32_push(rd, compute, iteration)
		rd.compute_list_dispatch_indirect(compute, _due_queue_indirect, 0)
		rd.compute_list_add_barrier(compute)

		if lod_interface_flux != null:
			lod_interface_flux.record_corrections(rd, compute)

		rd.compute_list_bind_compute_pipeline(compute, _commit_pipeline)
		rd.compute_list_bind_uniform_set(compute, _commit_set, 0)
		rd.compute_list_dispatch_indirect(compute, _due_queue_indirect, 0)
		rd.compute_list_add_barrier(compute)

		# Only the tiles whose canonical state changed are rescanned. Non-due coarse
		# summaries remain bit-for-bit valid for the next fine-clock CFL decision.
		cfl_cache.record_refresh_due(rd, compute)
		temporal_schedule.record_commit(rd, compute)

	# Final health diagnostics are also a tile-summary reduction; the forced final
	# temporal synchronization has refreshed every level that advanced at the edge.
	cfl_cache.record_reduce(rd, compute, 1)

	rd.compute_list_bind_compute_pipeline(compute, _external_reduce_pipeline)
	rd.compute_list_bind_uniform_set(compute, _external_reduce_set, 0)
	rd.compute_list_dispatch(compute, external_groups, 1, 1)
	rd.compute_list_add_barrier(compute)
	rd.compute_list_bind_compute_pipeline(compute, _external_finalize_pipeline)
	rd.compute_list_bind_uniform_set(compute, _external_finalize_set, 0)
	rd.compute_list_dispatch(compute, 1, 1, 1)
	rd.compute_list_end()

	if request_diagnostics:
		var callback := Callable(self, &"_on_diagnostics_bytes").bind(step_id)
		err = rd.buffer_get_data_async(_control, callback, 0, SUBCYCLED_CONTROL_BYTES)
		if err != OK:
			call_deferred("_diagnostics_failed", step_id, err)
	call_deferred("_finish_advance", step_id, true, request_diagnostics)


func release() -> void:
	cfl_cache = null
	_cfl_topology_revision = -1
	super.release()
