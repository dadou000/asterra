class_name SparseHydroStepGPULOD
extends SparseHydroStepGPU
## Phase-4 sparse SWE dispatcher.
##
## The base solver still performs the complete adaptive-CFL A->B step. Immediately
## afterward, every registered 2:1 interface replaces the reflective boundary term
## in B with the conservative coarse/fine flux computed from the same pre-step A.
## B is then canonicalized back to A exactly as before.

var lod_interface_flux: HydroLODInterfaceFluxGPU


func set_lod_interface_flux(provider: HydroLODInterfaceFluxGPU) -> Error:
	if step_pending() or diagnostics_pending():
		return ERR_BUSY
	if provider != null and not provider.initialized_ok():
		return ERR_UNCONFIGURED
	lod_interface_flux = provider
	return OK


func stats() -> Dictionary:
	var out := super.stats()
	out["lod_interface_flux"] = {} if lod_interface_flux == null \
		else lod_interface_flux.stats()
	out["same_step_cross_lod_reflux"] = lod_interface_flux != null \
		and lod_interface_flux.initialized_ok()
	return out


func _advance_render_thread(step_id: int, cap: int, request_diagnostics: bool,
		param_bytes: PackedByteArray) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null or not _all_runtime_rids_valid():
		call_deferred("_finish_advance", step_id, false, request_diagnostics)
		return
	if lod_interface_flux != null and not lod_interface_flux.initialized_ok():
		call_deferred("_finish_advance", step_id, false, request_diagnostics)
		return
	var err := rd.buffer_update(_params, 0, param_bytes.size(), param_bytes)
	var external_ledger_bytes := _atlas.total_cell_count() * EXTERNAL_LEDGER_FLOATS * 4
	if err != OK or rd.buffer_clear(_control, 0, CONTROL_BYTES) != OK \
			or rd.buffer_clear(_external_flux_ledger, 0, external_ledger_bytes) != OK:
		call_deferred("_finish_advance", step_id, false, request_diagnostics)
		return

	var groups_x := int(ceil(float(_atlas.tile_resolution) / float(LOCAL_X)))
	var groups_y := int(ceil(float(_atlas.tile_resolution) / float(LOCAL_Y)))
	var commit_groups := int(ceil(float(_atlas.total_cell_count()) / float(COMMIT_LOCAL_X)))
	var external_groups := int(ceil(float(_atlas.total_cell_count()) \
		/ float(EXTERNAL_REDUCE_LOCAL_X)))
	var compute := rd.compute_list_begin()

	for iteration in cap:
		rd.compute_list_bind_compute_pipeline(compute, _reset_pipeline)
		rd.compute_list_bind_uniform_set(compute, _reset_set, 0)
		rd.compute_list_dispatch(compute, 1, 1, 1)
		rd.compute_list_add_barrier(compute)

		rd.compute_list_bind_compute_pipeline(compute, _reduce_pipeline)
		rd.compute_list_bind_uniform_set(compute, _reduce_set, 0)
		_set_u32_push(rd, compute, 0)
		rd.compute_list_dispatch(compute, groups_x, groups_y, _atlas.capacity)
		rd.compute_list_add_barrier(compute)

		rd.compute_list_bind_compute_pipeline(compute, _prepare_pipeline)
		rd.compute_list_bind_uniform_set(compute, _prepare_set, 0)
		_set_u32_push(rd, compute, iteration)
		rd.compute_list_dispatch(compute, 1, 1, 1)
		rd.compute_list_add_barrier(compute)

		# Proven sparse step. Missing same-level neighbors are reflective here.
		rd.compute_list_bind_compute_pipeline(compute, _step_pipeline)
		rd.compute_list_bind_uniform_set(compute, _step_set, 0)
		_set_u32_push(rd, compute, iteration)
		rd.compute_list_dispatch(compute, groups_x, groups_y, _atlas.capacity)
		rd.compute_list_add_barrier(compute)

		# Replace only represented 2:1 reflective boundaries using the same A state.
		if lod_interface_flux != null:
			lod_interface_flux.record_corrections(rd, compute)

		rd.compute_list_bind_compute_pipeline(compute, _commit_pipeline)
		rd.compute_list_bind_uniform_set(compute, _commit_set, 0)
		rd.compute_list_dispatch(compute, commit_groups, 1, 1)
		rd.compute_list_add_barrier(compute)

	# Final physical-state health reduction sees the corrected canonical A state.
	rd.compute_list_bind_compute_pipeline(compute, _reduce_pipeline)
	rd.compute_list_bind_uniform_set(compute, _reduce_set, 0)
	_set_u32_push(rd, compute, 1)
	rd.compute_list_dispatch(compute, groups_x, groups_y, _atlas.capacity)
	rd.compute_list_add_barrier(compute)

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
		err = rd.buffer_get_data_async(_control, callback, 0, CONTROL_BYTES)
		if err != OK:
			call_deferred("_diagnostics_failed", step_id, err)
	call_deferred("_finish_advance", step_id, true, request_diagnostics)


func release() -> void:
	lod_interface_flux = null
	super.release()
