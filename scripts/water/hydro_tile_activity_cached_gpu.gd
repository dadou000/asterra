class_name HydroTileActivityCachedGPU
extends HydroTileActivityGPU
## Readback-only HydroTileActivityGPU facade for temporal HydroLOD.
##
## The 80-byte-per-slot summary buffer is owned and refreshed by HydroLODCFLCacheGPU
## in the same state scan that produces CFL/health summaries. This class preserves
## the established activity signals, dictionary decoder and summary_rid() contract
## without recording a second cell-classification compute dispatch.

var _external_summary := false


func initialize_cached(summary_rid: RID, capacity: int) -> Error:
	if _initialized or _init_pending or _dispatch_pending or _readback_pending:
		return ERR_BUSY
	if not summary_rid.is_valid() or capacity <= 0:
		return ERR_INVALID_PARAMETER
	if RenderingServer.get_rendering_device() == null:
		return ERR_UNAVAILABLE
	_summary = summary_rid
	_capacity = capacity
	_external_summary = true
	_initialized = true
	initialized.emit()
	return OK


func classify(request_readback: bool = false) -> int:
	if not _initialized or not _external_summary \
			or _dispatch_pending or _readback_pending or not _summary.is_valid():
		return -1
	var request_id := _next_request_id
	_next_request_id += 1
	_dispatch_pending = true
	_readback_pending = request_readback
	if request_readback:
		RenderingServer.call_on_render_thread(
			Callable(self, &"_cached_readback_render_thread").bind(request_id))
	else:
		call_deferred(&"_finish_classification", request_id, OK)
	return request_id


func cached_stats() -> Dictionary:
	return {
		"initialized": _initialized,
		"external_summary_buffer": _external_summary,
		"compute_dispatch": false,
		"readback_only": true,
		"summary_bytes_per_slot": SUMMARY_BYTES_PER_TILE,
		"capacity": _capacity,
	}


func _cached_readback_render_thread(request_id: int) -> void:
	var rd := RenderingServer.get_rendering_device()
	if rd == null or not _summary.is_valid():
		call_deferred(&"_finish_classification", request_id, ERR_UNAVAILABLE)
		return
	var callback := Callable(self, &"_on_summary_bytes").bind(request_id)
	var err := rd.buffer_get_data_async(_summary, callback,
		0, _capacity * SUMMARY_BYTES_PER_TILE)
	if err != OK:
		call_deferred(&"_finish_classification", request_id, err)
		return
	call_deferred(&"_finish_classification", request_id, OK)


## The summary RID is external and must never be freed here. HydroLODCFLCacheGPU is
## released after frontier/activity consumers have detached from it.
func release() -> void:
	if not _initialized and not _external_summary:
		return
	_initialized = false
	_init_pending = false
	_dispatch_pending = false
	_readback_pending = false
	_external_summary = false
	_summary = RID()
	_capacity = 0
	_state = RID(); _occupancy = RID(); _metadata = RID(); _fallback_metadata = RID()
	_shader = RID(); _pipeline = RID(); _params = RID(); _uniform_set = RID()
	released.emit()


func _exit_tree() -> void:
	release()
