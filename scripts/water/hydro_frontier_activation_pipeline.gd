class_name HydroFrontierActivationPipeline
extends Node
## End-to-end CPU policy coordinator for conservative sparse frontier expansion.
##
## Pipeline for one GPU frontier batch:
##   resolve candidates with defer_activation=true
##   -> reserve destination slots (ALLOCATING, GPU occupancy still 0)
##   -> stage each destination's own dry terrain state into A+B exactly once
##   -> optional transactional coarse-surface preseed into hidden A+B
##   -> wait for every preseed acknowledgement/coarse debit
##   -> perform one or more conservative source-edge handoffs into that destination
##   -> activate_reserved() only after the final seed
##   -> identity bridge publishes metadata/occupancy
##   -> rebuild resident connectivity
##
## Destination initializer contract supports two modes:
##
## Legacy/test CPU state:
##   PackedFloat32Array provider(destination_key, destination_slot)
##   containing tile_res*tile_res vec4(h,hu,hv,bed).
##
## Production GPU stage:
##   Dictionary provider(destination_key, destination_slot)
##   { "queued": true, "error": OK, "request_id": ... }
##
## A queued GPU initializer must record its RenderingDevice work through
## RenderingServer.call_on_render_thread(). Coarse preseed and edge handoff are
## queued afterwards, so render-thread FIFO ordering preserves stage -> seed ->
## handoff. Missing/invalid initialization FAILS CLOSED and releases reservation.
##
## Failure ownership is based on reversibility. A coarse-only preseed can be
## restored exactly if the first edge handoff fails, so that hidden destination is
## cancelled. Once any edge handoff succeeds, however, the source tile has already
## lost a parcel and there is no reverse GPU handoff; the destination must therefore
## be published/preserved. Other unfinished destinations in the same batch are
## cleaned deterministically by the same rule.

signal initialized
signal initialization_failed(error: Error)
signal batch_started(batch_id: int, reserved_destinations: int, handoffs: int)
signal destination_coarse_seeded(batch_id: int, tile_id: int, slot: int, volume_m3: float)
signal destination_activated(batch_id: int, tile_id: int, slot: int)
signal destination_cancelled(batch_id: int, tile_id: int, slot: int, reason: String)
signal batch_completed(batch_id: int, results: Array[Dictionary])
signal batch_failed(batch_id: int, error: Error)
signal released

var scheduler: SparseHydroScheduler
var atlas: SparseHydroAtlasGPU
var connectivity: SparseHydroConnectivityGPU
var identity_bridge: SparseHydroIdentityBridge
var resolver: HydroFrontierResolver
var handoff: HydroFrontierHandoffGPU
## Externally owned. WaterSystem binds this only when the persistent coarse store
## and sparse runtime refer to the same generated world.
var coarse_preseed: HydroFrontierCoarsePreseed

var _initialized := false
var _busy := false
var _next_batch_id := 1
var _batch_id := -1
var _results: Array[Dictionary] = []
var _jobs: Array[Dictionary] = []
var _job_index := 0
var _pending_preseeds: Dictionary = {} # request id -> {destination_id, slot}
var _handoff_success_by_destination: Dictionary = {} # tile id -> successful handoff count
var _seed_dt_s := 0.02
var _max_seed_fraction := 0.12
var _gravity := 9.81


func initialize(p_scheduler: SparseHydroScheduler, p_atlas: SparseHydroAtlasGPU,
		p_connectivity: SparseHydroConnectivityGPU,
		p_identity_bridge: SparseHydroIdentityBridge) -> Error:
	if _initialized or _busy:
		return ERR_BUSY
	if p_scheduler == null or p_atlas == null or p_connectivity == null \
			or p_identity_bridge == null:
		return ERR_INVALID_PARAMETER
	if not p_atlas.initialized_ok() or not p_connectivity.initialized_ok() \
			or not p_identity_bridge.is_bound():
		return ERR_UNCONFIGURED
	if p_scheduler.pool.capacity != p_atlas.capacity \
			or p_connectivity.capacity != p_atlas.capacity:
		return ERR_INVALID_PARAMETER

	scheduler = p_scheduler
	atlas = p_atlas
	connectivity = p_connectivity
	identity_bridge = p_identity_bridge
	resolver = HydroFrontierResolver.new(scheduler)
	handoff = HydroFrontierHandoffGPU.new()
	add_child(handoff)
	handoff.initialized.connect(_on_handoff_initialized)
	handoff.initialization_failed.connect(_on_handoff_initialization_failed)
	handoff.handoff_recorded.connect(_on_handoff_recorded)
	handoff.handoff_failed.connect(_on_handoff_failed)
	var err := handoff.initialize(atlas)
	if err != OK:
		handoff.queue_free()
		handoff = null
		return err
	return OK


func initialized_ok() -> bool:
	return _initialized


func busy() -> bool:
	return _busy


func set_coarse_preseed(provider: HydroFrontierCoarsePreseed) -> Error:
	if _busy:
		return ERR_BUSY
	_disconnect_coarse_preseed()
	if provider == null:
		return OK
	if not provider.initialized_ok():
		return ERR_UNCONFIGURED
	coarse_preseed = provider
	coarse_preseed.preseed_completed.connect(_on_coarse_preseed_completed)
	coarse_preseed.preseed_failed.connect(_on_coarse_preseed_failed)
	return OK


## Returns batch ID, or -1 when rejected. Multiple candidates targeting the same
## new tile are grouped: destination state is staged once, optional coarse state is
## transactionally seeded once, every incoming edge can then contribute a
## conservative parcel, and activation occurs after the final edge seed.
func process_candidates(candidates: Array[Dictionary], reachability: Callable,
		destination_state_provider: Callable, seed_dt_s: float = 0.02,
		max_seed_fraction: float = 0.12, gravity: float = 9.81) -> int:
	if not _initialized or _busy or not reachability.is_valid() \
			or not destination_state_provider.is_valid():
		return -1
	if not is_finite(seed_dt_s) or seed_dt_s <= 0.0:
		return -1

	var batch_id := _next_batch_id
	_next_batch_id += 1
	var resolved := resolver.resolve_candidates(candidates, reachability, true)
	var groups: Dictionary = {}
	for result in resolved:
		if not bool(result.get("accepted", false)) \
				or not bool(result.get("needs_handoff", false)):
			continue
		var destination_id := int(result.get("destination_tile_id", -1))
		if destination_id < 0:
			continue
		if not groups.has(destination_id):
			groups[destination_id] = []
		(groups[destination_id] as Array).append(result)

	var jobs: Array[Dictionary] = []
	var pending_preseeds: Dictionary = {}
	var reserved_destinations := 0
	for destination_variant in groups.keys():
		var destination_id := int(destination_variant)
		var incoming: Array = groups[destination_id]
		if incoming.is_empty():
			continue
		var first := incoming[0] as Dictionary
		var destination_slot := int(first.get("destination_slot", -1))
		var destination := HydroTileKey.unpack(destination_id)
		if destination == null:
			continue

		var stage_variant: Variant = destination_state_provider.call(
			destination, destination_slot)
		var stage_error := _accept_destination_stage(stage_variant, destination_slot)
		if stage_error != OK:
			_cancel_group(batch_id, destination, destination_slot,
				"destination_stage_failed", resolved)
			continue

		# The terrain stage has now been queued. Any coarse preseed records after it
		# and before edge handoff. request 0 is a valid no-op (no coarse surface water).
		if coarse_preseed != null:
			var preseed_request := coarse_preseed.request_preseed(
				destination, destination_slot)
			if preseed_request < 0:
				_cancel_group(batch_id, destination, destination_slot,
					"coarse_preseed_rejected", resolved)
				continue
			if preseed_request > 0:
				pending_preseeds[preseed_request] = {
					"destination_id": destination_id,
					"slot": destination_slot,
				}

		reserved_destinations += 1
		for i in incoming.size():
			var r := (incoming[i] as Dictionary).duplicate(true)
			r["activate_after"] = i == incoming.size() - 1
			jobs.append(r)

	_results = resolved
	_jobs = jobs
	_job_index = 0
	_pending_preseeds = pending_preseeds
	_handoff_success_by_destination.clear()
	_batch_id = batch_id
	_seed_dt_s = seed_dt_s
	_max_seed_fraction = clampf(max_seed_fraction, 0.0, 0.5)
	_gravity = maxf(gravity, 1.0e-4)
	_busy = not jobs.is_empty() or not _pending_preseeds.is_empty()
	batch_started.emit(batch_id, reserved_destinations, jobs.size())

	if not _pending_preseeds.is_empty():
		return batch_id
	if jobs.is_empty():
		call_deferred("_finish_batch")
	else:
		_start_current_job()
	return batch_id


func _accept_destination_stage(stage_variant: Variant, destination_slot: int) -> Error:
	if typeof(stage_variant) == TYPE_PACKED_FLOAT32_ARRAY:
		var state: PackedFloat32Array = stage_variant
		if state.size() != atlas.cells_per_tile() * SparseHydroAtlasGPU.STATE_FLOATS:
			return ERR_INVALID_DATA
		return atlas.stage_slot_state(destination_slot, state)

	if typeof(stage_variant) == TYPE_DICTIONARY:
		var result: Dictionary = stage_variant
		var error := int(result.get("error", ERR_INVALID_DATA))
		if error != OK or not bool(result.get("queued", false)):
			return error if error != OK else ERR_INVALID_DATA
		return OK

	return ERR_INVALID_DATA


func _cancel_group(batch_id: int, destination: HydroTileKey, slot: int,
		reason: String, resolved: Array[Dictionary]) -> void:
	if destination == null:
		return
	var destination_id := destination.packed()
	scheduler.cancel_reserved(destination, reason)
	for result in resolved:
		if int(result.get("destination_tile_id", -1)) == destination_id:
			result["accepted"] = false
			result["reserved"] = false
			result["needs_handoff"] = false
			result["reason"] = reason
	destination_cancelled.emit(batch_id, destination_id, slot, reason)


func _on_coarse_preseed_completed(request_id: int, tile_id: int, slot: int,
		report: Dictionary) -> void:
	if not _busy or not _pending_preseeds.has(request_id):
		return
	var expected: Dictionary = _pending_preseeds[request_id]
	if int(expected.get("destination_id", -1)) != tile_id \
			or int(expected.get("slot", -1)) != slot:
		if coarse_preseed != null and coarse_preseed.has_provisional_tile(tile_id):
			coarse_preseed.restore_tile(tile_id)
		_on_coarse_preseed_failed(request_id, tile_id, slot,
			ERR_INVALID_DATA, "preseed_ack_identity")
		return
	_pending_preseeds.erase(request_id)
	var volume := float(report.get("represented_volume_m3", 0.0))
	_mark_preseed_result(tile_id, volume)
	destination_coarse_seeded.emit(_batch_id, tile_id, slot, volume)
	_maybe_start_jobs_after_preseed()


func _on_coarse_preseed_failed(request_id: int, tile_id: int, slot: int,
		_error: Error, stage: String) -> void:
	if not _busy or not _pending_preseeds.has(request_id):
		return
	var expected: Dictionary = _pending_preseeds[request_id]
	var destination_id := int(expected.get("destination_id", tile_id))
	var destination_slot := int(expected.get("slot", slot))
	_pending_preseeds.erase(request_id)
	if coarse_preseed != null and coarse_preseed.has_provisional_tile(destination_id):
		coarse_preseed.restore_tile(destination_id)
	var destination := HydroTileKey.unpack(destination_id)
	if destination != null:
		scheduler.cancel_reserved(destination, "coarse_preseed_" + stage)
		_mark_destination_result(destination_id, "coarse_preseed_" + stage)
		destination_cancelled.emit(_batch_id, destination_id, destination_slot,
			"coarse_preseed_" + stage)
	_remove_jobs_for_destination(destination_id)
	_maybe_start_jobs_after_preseed()


func _maybe_start_jobs_after_preseed() -> void:
	if not _pending_preseeds.is_empty():
		return
	if _jobs.is_empty():
		_finish_batch()
	else:
		_job_index = clampi(_job_index, 0, _jobs.size() - 1)
		_start_current_job()


func _remove_jobs_for_destination(destination_id: int) -> void:
	var kept: Array[Dictionary] = []
	for job in _jobs:
		if int(job.get("destination_tile_id", -1)) != destination_id:
			kept.append(job)
	_jobs = kept
	_job_index = 0


func _mark_preseed_result(destination_id: int, volume_m3: float) -> void:
	for result in _results:
		if int(result.get("destination_tile_id", -1)) == destination_id:
			result["coarse_preseed_volume_m3"] = volume_m3
			result["coarse_preseed_committed"] = true


func _start_current_job() -> void:
	if not _busy or not _pending_preseeds.is_empty() \
			or _job_index < 0 or _job_index >= _jobs.size():
		if _pending_preseeds.is_empty():
			_finish_batch()
		return
	var job := _jobs[_job_index]
	var request_id := handoff.seed(
		int(job["source_slot"]), int(job["destination_slot"]),
		int(job["direction"]), int(job["destination_direction"]),
		int(job["edge_orientation"]) < 0,
		_seed_dt_s, _max_seed_fraction, _gravity)
	if request_id < 0:
		_fail_current_job(ERR_BUSY)


func _on_handoff_recorded(_request_id: int, _source_slot: int,
		destination_slot: int) -> void:
	if not _busy or _job_index < 0 or _job_index >= _jobs.size():
		return
	var job := _jobs[_job_index]
	if destination_slot != int(job.get("destination_slot", -1)):
		_fail_current_job(ERR_INVALID_DATA)
		return
	var destination_id := int(job.get("destination_tile_id", -1))
	_handoff_success_by_destination[destination_id] = \
		int(_handoff_success_by_destination.get(destination_id, 0)) + 1

	if bool(job.get("activate_after", false)):
		var destination := HydroTileKey.unpack(destination_id)
		var slot := scheduler.activate_reserved(destination, "frontier_handoff_complete")
		if slot < 0:
			_fail_current_job(ERR_CANT_ACQUIRE_RESOURCE)
			return
		var conn_error := connectivity.sync_pool(scheduler.pool)
		if conn_error != OK:
			# Tile is already published and owns all seeded water. Never release it here;
			# stopping the runtime with stale connectivity is recoverable, deleting the
			# physical parcel is not.
			_finalize_coarse_preseed(destination_id)
			_mark_destination_result(destination_id, "activated_connectivity_failed")
			_fail_batch_preserving_state(conn_error)
			return
		_finalize_coarse_preseed(destination_id)
		destination_activated.emit(_batch_id, destination.packed(), slot)
		_mark_destination_result(destination.packed(), "activated")

	_job_index += 1
	if _job_index >= _jobs.size():
		_finish_batch()
	else:
		_start_current_job()


func _on_handoff_failed(_request_id: int, error: Error) -> void:
	if _busy:
		_fail_current_job(error)


func _fail_current_job(error: Error) -> void:
	if _job_index >= 0 and _job_index < _jobs.size():
		var job := _jobs[_job_index]
		var destination_id := int(job.get("destination_tile_id", -1))
		var destination := HydroTileKey.unpack(destination_id)
		var slot := int(job.get("destination_slot", -1))
		var successful_handoffs := int(_handoff_success_by_destination.get(
			destination_id, 0))
		if destination != null and successful_handoffs > 0:
			# At least one source edge has already been debited. There is no reverse GPU
			# transfer here, so publish the partial destination rather than deleting it.
			var record := scheduler.pool.record(destination)
			var state := int(record.get("state", HydroTilePool.TileState.ALLOCATING))
			if state == HydroTilePool.TileState.ALLOCATING:
				var activated := scheduler.activate_reserved(destination,
					"frontier_partial_after_failure")
				if activated >= 0:
					connectivity.sync_pool(scheduler.pool)
			_finalize_coarse_preseed(destination_id)
			_mark_destination_result(destination_id,
				"activated_after_handoff_failure")
		else:
			# No edge parcel moved. A coarse-only preseed is exactly reversible, so
			# restore it before releasing the still-hidden destination.
			_restore_coarse_preseed(destination_id)
			if destination != null:
				scheduler.cancel_reserved(destination, "handoff_gpu_failed")
				_mark_destination_result(destination_id, "handoff_gpu_failed")
				destination_cancelled.emit(_batch_id, destination_id, slot,
					"handoff_gpu_failed")
	_fail_batch_preserving_state(error)


func _fail_batch_preserving_state(error: Error) -> void:
	var failed_batch := _batch_id
	var cleanup_error := _cleanup_unfinished_destinations()
	if cleanup_error != OK:
		push_error("HydroFrontierActivationPipeline: batch %d cleanup failed (%d) after error %d." % [
			failed_batch, int(cleanup_error), int(error)])
	_busy = false
	batch_failed.emit(failed_batch, error)
	_reset_batch_state()


## Close every destination that was reserved for this batch but never reached a
## normal terminal state. Jobs begin only after all asynchronous coarse preseeds
## have resolved, so at a handoff/connectivity failure there is no seed callback
## still capable of racing this cleanup.
func _cleanup_unfinished_destinations() -> Error:
	if scheduler == null or scheduler.pool == null:
		return ERR_UNCONFIGURED
	var destinations: Dictionary = {} # tile id -> slot
	for job in _jobs:
		var destination_id := int(job.get("destination_tile_id", -1))
		if destination_id >= 0 and not destinations.has(destination_id):
			destinations[destination_id] = int(job.get("destination_slot", -1))

	var cleanup_error := OK
	var connectivity_dirty := false
	for destination_variant: Variant in destinations.keys():
		var destination_id := int(destination_variant)
		var destination := HydroTileKey.unpack(destination_id)
		if destination == null:
			continue
		var record := scheduler.pool.record(destination)
		if record.is_empty():
			# A current-job failure may already have cancelled this tile. If bookkeeping
			# still contains a provisional coarse debit, request its restoration.
			_restore_coarse_preseed(destination_id)
			continue
		var slot := int(record.get("slot", int(destinations[destination_id])))
		var state := int(record.get("state", HydroTilePool.TileState.ALLOCATING))
		var successful_handoffs := int(_handoff_success_by_destination.get(
			destination_id, 0))

		if state != HydroTilePool.TileState.ALLOCATING:
			# Already solver-visible: its fine bytes are authoritative and any coarse
			# preseed must stay debited.
			_finalize_coarse_preseed(destination_id)
			continue

		if successful_handoffs > 0:
			# At least one conservative edge transfer has already reduced a source tile.
			# Publishing the partially constructed destination is the only operation that
			# preserves that transferred water without inventing a reverse GPU handoff.
			var activated := scheduler.activate_reserved(destination,
				"frontier_batch_failure_preserve")
			if activated < 0:
				cleanup_error = ERR_CANT_ACQUIRE_RESOURCE
				continue
			_finalize_coarse_preseed(destination_id)
			_mark_destination_result(destination_id,
				"activated_after_batch_failure_cleanup")
			destination_activated.emit(_batch_id, destination_id, activated)
			connectivity_dirty = true
			continue

		# No edge parcel ever arrived. A provisional coarse seed can be returned
		# exactly, after which the hidden dry/seeded slot may be released safely.
		if coarse_preseed != null and coarse_preseed.has_provisional_tile(destination_id):
			var restore := coarse_preseed.restore_tile(destination_id)
			var restore_error := int(restore.get("error", FAILED))
			if restore_error != OK and restore_error != ERR_BUSY:
				if cleanup_error == OK:
					cleanup_error = restore_error
				continue
		if scheduler.cancel_reserved(destination, "frontier_batch_failure_cleanup"):
			_mark_destination_result(destination_id, "cancelled_after_batch_failure")
			destination_cancelled.emit(_batch_id, destination_id, slot,
				"frontier_batch_failure_cleanup")
		else:
			if cleanup_error == OK:
				cleanup_error = ERR_CANT_ACQUIRE_RESOURCE

	if connectivity_dirty and connectivity != null:
		var conn_error := connectivity.sync_pool(scheduler.pool)
		if conn_error != OK and cleanup_error == OK:
			cleanup_error = conn_error
	return cleanup_error


func _finalize_coarse_preseed(destination_id: int) -> void:
	if coarse_preseed != null and coarse_preseed.has_provisional_tile(destination_id):
		coarse_preseed.finalize_tile(destination_id)


func _restore_coarse_preseed(destination_id: int) -> void:
	if coarse_preseed != null and coarse_preseed.has_provisional_tile(destination_id):
		coarse_preseed.restore_tile(destination_id)


func _mark_destination_result(destination_id: int, reason: String) -> void:
	for result in _results:
		if int(result.get("destination_tile_id", -1)) != destination_id:
			continue
		result["reason"] = reason
		result["reserved"] = false
		result["needs_handoff"] = false
		result["accepted"] = reason.begins_with("activated")


func _finish_batch() -> void:
	var batch_id := _batch_id
	var published := _results.duplicate(true)
	_busy = false
	_reset_batch_state()
	batch_completed.emit(batch_id, published)


func _reset_batch_state() -> void:
	_batch_id = -1
	_results = []
	_jobs = []
	_job_index = 0
	_pending_preseeds.clear()
	_handoff_success_by_destination.clear()


func _on_handoff_initialized() -> void:
	_initialized = true
	initialized.emit()


func _on_handoff_initialization_failed(error: Error) -> void:
	_initialized = false
	initialization_failed.emit(error)


func _disconnect_coarse_preseed() -> void:
	if coarse_preseed != null:
		if coarse_preseed.preseed_completed.is_connected(_on_coarse_preseed_completed):
			coarse_preseed.preseed_completed.disconnect(_on_coarse_preseed_completed)
		if coarse_preseed.preseed_failed.is_connected(_on_coarse_preseed_failed):
			coarse_preseed.preseed_failed.disconnect(_on_coarse_preseed_failed)
	coarse_preseed = null


func release() -> void:
	_disconnect_coarse_preseed()
	if handoff != null and is_instance_valid(handoff):
		handoff.release()
		handoff.queue_free()
	_initialized = false
	_busy = false
	_reset_batch_state()
	scheduler = null
	atlas = null
	connectivity = null
	identity_bridge = null
	resolver = null
	handoff = null
	released.emit()


func _exit_tree() -> void:
	release()