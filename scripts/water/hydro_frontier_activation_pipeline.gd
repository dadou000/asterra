class_name HydroFrontierActivationPipeline
extends Node
## End-to-end CPU policy coordinator for conservative sparse frontier expansion.
##
## Pipeline for one GPU frontier batch:
##   resolve candidates with defer_activation=true
##   -> reserve destination slots (ALLOCATING, GPU occupancy still 0)
##   -> stage each destination's own dry terrain state into A+B exactly once
##   -> perform one or more conservative source-edge handoffs into that destination
##   -> activate_reserved() only after the final seed
##   -> identity bridge publishes metadata/occupancy
##   -> rebuild resident connectivity
##
## Destination state provider contract:
##   PackedFloat32Array provider(destination_key, destination_slot)
## containing tile_res*tile_res vec4(h,hu,hv,bed). Normally h/hu/hv are zero and
## bed is reconstructed from the terrain system. Invalid/missing data FAILS CLOSED.

signal initialized
signal initialization_failed(error: Error)
signal batch_started(batch_id: int, reserved_destinations: int, handoffs: int)
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

var _initialized := false
var _busy := false
var _next_batch_id := 1
var _batch_id := -1
var _results: Array[Dictionary] = []
var _jobs: Array[Dictionary] = []
var _job_index := 0
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


## Returns batch ID, or -1 when rejected. Multiple candidates targeting the same
## new tile are grouped: destination state is staged once, every incoming edge can
## contribute a conservative parcel, and activation occurs after the final seed.
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
	var groups: Dictionary = {} # destination packed ID -> Array[Dictionary]
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
	var reserved_destinations := 0
	for destination_variant in groups.keys():
		var destination_id := int(destination_variant)
		var incoming: Array = groups[destination_id]
		if incoming.is_empty():
			continue
		var first := incoming[0] as Dictionary
		var destination_slot := int(first.get("destination_slot", -1))
		var destination := HydroTileKey.unpack(destination_id)
		var state_variant: Variant = destination_state_provider.call(
			destination, destination_slot)
		if not (state_variant is PackedFloat32Array):
			_cancel_group(batch_id, destination, destination_slot,
				"destination_initializer_missing", resolved)
			continue
		var state := state_variant as PackedFloat32Array
		if state.size() != atlas.cells_per_tile() * SparseHydroAtlasGPU.STATE_FLOATS:
			_cancel_group(batch_id, destination, destination_slot,
				"destination_initializer_size", resolved)
			continue
		var stage_error := atlas.stage_slot_state(destination_slot, state)
		if stage_error != OK:
			_cancel_group(batch_id, destination, destination_slot,
				"destination_stage_failed", resolved)
			continue

		reserved_destinations += 1
		for i in incoming.size():
			var r := (incoming[i] as Dictionary).duplicate(true)
			r["activate_after"] = i == incoming.size() - 1
			jobs.append(r)

	_results = resolved
	_jobs = jobs
	_job_index = 0
	_batch_id = batch_id
	_seed_dt_s = seed_dt_s
	_max_seed_fraction = clampf(max_seed_fraction, 0.0, 0.5)
	_gravity = maxf(gravity, 1.0e-4)
	_busy = not jobs.is_empty()
	batch_started.emit(batch_id, reserved_destinations, jobs.size())

	if jobs.is_empty():
		# No new destinations may simply mean all candidates already had resident
		# neighbors or were blocked. That is a successful empty activation batch.
		call_deferred("_finish_batch")
	else:
		_start_current_job()
	return batch_id


func _cancel_group(batch_id: int, destination: HydroTileKey, slot: int,
		reason: String, resolved: Array[Dictionary]) -> void:
	if destination != null:
		scheduler.cancel_reserved(destination, reason)
	for result in resolved:
		if int(result.get("destination_tile_id", -1)) == destination.packed():
			result["accepted"] = false
			result["reserved"] = false
			result["needs_handoff"] = false
			result["reason"] = reason
	destination_cancelled.emit(batch_id, destination.packed(), slot, reason)


func _start_current_job() -> void:
	if not _busy or _job_index < 0 or _job_index >= _jobs.size():
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

	if bool(job.get("activate_after", false)):
		var destination := HydroTileKey.unpack(int(job["destination_tile_id"]))
		var slot := scheduler.activate_reserved(destination, "frontier_handoff_complete")
		if slot < 0:
			_fail_current_job(ERR_CANT_ACQUIRE_RESOURCE)
			return
		# tile_woken synchronously schedules identity publication through the bound
		# bridge. Queue connectivity sync afterwards so render-thread ordering is:
		# handoff compute -> metadata/occupancy publish -> connectivity publish.
		var conn_error := connectivity.sync_pool(scheduler.pool)
		if conn_error != OK:
			_fail_current_job(conn_error)
			return
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
		var destination := HydroTileKey.unpack(int(job.get("destination_tile_id", -1)))
		var slot := int(job.get("destination_slot", -1))
		if destination != null:
			scheduler.cancel_reserved(destination, "handoff_gpu_failed")
			_mark_destination_result(destination.packed(), "handoff_gpu_failed")
			destination_cancelled.emit(_batch_id, destination.packed(), slot,
				"handoff_gpu_failed")
	_busy = false
	batch_failed.emit(_batch_id, error)
	_reset_batch_state()


func _mark_destination_result(destination_id: int, reason: String) -> void:
	for result in _results:
		if int(result.get("destination_tile_id", -1)) != destination_id:
			continue
		result["reason"] = reason
		result["reserved"] = false
		result["needs_handoff"] = false
		result["accepted"] = reason == "activated"


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


func _on_handoff_initialized() -> void:
	_initialized = true
	initialized.emit()


func _on_handoff_initialization_failed(error: Error) -> void:
	_initialized = false
	initialization_failed.emit(error)


func release() -> void:
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
