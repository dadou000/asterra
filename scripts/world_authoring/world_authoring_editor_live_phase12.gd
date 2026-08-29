class_name WorldAuthoringLiveEditorPhase12
extends "res://scripts/world_authoring/world_authoring_editor_live_phase11.gd"
## Phase 12: asynchronous persistent-envelope shaping.
##
## Flatten no longer needs one synchronous Planet.pristine_height() call per edit
## lattice sample when the Forward+ batch service is available. Candidate samples
## are collected on the main thread, then Planet.global_height_texture (macro +
## coast profile, explicitly excluding analytic visible geomorph detail) is sampled
## in one asynchronous compute dispatch. Sparse Deltas are still written only after
## a complete immutable source snapshot has been evaluated, so traversal order,
## one-drag/one-Undo persistence and authoritative render/contact behavior remain
## unchanged.
##
## This pass also removes a redundant pristine evaluation in Smooth by deriving the
## center pristine value from the already cached final-height snapshot.

const PRISTINE_BATCH_QUERY_SCRIPT := preload("res://scripts/terrain/gpu_terrain_pristine_batch_query.gd")

var _pristine_batch_query: Node
var _gpu_flatten_queue: Array[Dictionary] = []
var _gpu_flatten_active: Dictionary = {}
var _gpu_flatten_serial: int = 1
var _gpu_sculpt_commit_deferred: bool = false
var _gpu_last_enqueued_dir: Vector3 = Vector3.ZERO
var _gpu_last_query_count: int = 0
var _gpu_last_latency_ms: float = 0.0
var _gpu_last_finalize_ms: float = 0.0
var _gpu_last_fallback: bool = false


func _ready() -> void:
	super._ready()
	if _world_host == null:
		return
	var query: Node = PRISTINE_BATCH_QUERY_SCRIPT.new()
	query.name = "PlanetStudioPristineBatchQuery"
	add_child(query)
	_pristine_batch_query = query


func _build_terrain_page() -> void:
	super._build_terrain_page()
	if _world_host == null:
		return
	_section("Persistent envelope batch path")
	var state := "initializing"
	if _pristine_batch_query != null:
		if bool(_pristine_batch_query.get("failed")):
			state = "failed — CPU fallback"
		elif bool(_pristine_batch_query.get("ready_state")):
			state = "ready"
		elif not bool(_pristine_batch_query.get("supported")):
			state = "unsupported renderer — CPU fallback"
	var label := Label.new()
	label.modulate = Color(0.64, 0.76, 0.86)
	if _gpu_last_query_count > 0:
		label.text = "GPU envelope: %s • last %d heights • %.3f ms dispatch/readback • %.3f ms finalize%s" % [
			state,
			_gpu_last_query_count,
			_gpu_last_latency_ms,
			_gpu_last_finalize_ms,
			" • CPU fallback" if _gpu_last_fallback else "",
		]
	else:
		label.text = "GPU envelope: %s • no asynchronous Flatten stamp yet" % state
	_workspace.add_child(label)
	_add_note("Flatten uses the resident global macro+coast height texture in one asynchronous GPU batch when available. It intentionally does not query the contact geomorph shader, so high-frequency visible detail is not baked into saved Deltas. If the batch service is unavailable, the deterministic CPU pristine sampler remains the fallback.")


func _place_flatten_stroke(direction: Vector3, continuous: bool) -> void:
	if Planet.cfg == null or not Planet.ready_state or not _flatten_target_valid:
		return
	if not _gpu_flatten_supported():
		super._place_flatten_stroke(direction, continuous)
		return
	var planet_radius: float = maxf(float(Planet.cfg.planet_radius), 1.0)
	if _skip_redundant_gpu_enqueue(direction, continuous, planet_radius):
		return
	var samples: Array[Dictionary] = _collect_sculpt_samples(direction, planet_radius)
	if samples.is_empty():
		return
	var serial: int = _gpu_flatten_serial
	_gpu_flatten_serial += 1
	_gpu_flatten_queue.append({
		"serial": serial,
		"body_id": _active_body_id(),
		"center": direction.normalized(),
		"planet_radius": planet_radius,
		"radius_m": _sculpt_radius_m,
		"strength": _flatten_strength,
		"target_height_m": _flatten_target_height_m,
		"samples": samples,
		"enqueued_us": Time.get_ticks_usec(),
	})
	_gpu_last_enqueued_dir = direction.normalized()
	_set_status("Flatten queued: %d envelope samples • %d stamp(s) pending GPU evaluation." % [
		samples.size(), _gpu_flatten_queue.size() + (0 if _gpu_flatten_active.is_empty() else 1)])
	_start_next_gpu_flatten_job()


func _smooth_final_height_brush(center_dir: Vector3, planet_radius: float) -> int:
	var samples: Array[Dictionary] = _collect_sculpt_samples(center_dir, planet_radius)
	if samples.is_empty():
		return 0
	var spacing_m: float = maxf(Deltas.sample_spacing(planet_radius), 0.001)
	var neighbor_radius_m: float = clampf(_sculpt_radius_m * 0.12, spacing_m, SMOOTH_MAX_NEIGHBOR_RADIUS_M)
	var neighbor_step: int = maxi(1, int(round(neighbor_radius_m / spacing_m)))
	var angular_radius: float = (_sculpt_radius_m + float(neighbor_step + 3) * spacing_m) / planet_radius
	var snap: Dictionary = Deltas.snapshot_for_bounds(center_dir.normalized(), angular_radius)
	var height_cache: Dictionary = {}
	var writes: Array[Dictionary] = []
	for sample: Dictionary in samples:
		var address: Vector3i = sample["address"]
		var sample_dir: Vector3 = sample["dir"]
		var before_delta: float = Deltas.offset_at_snapshot(sample_dir, snap)
		# _snapshot_final_height() caches pristine+delta for the center as part of the
		# same cache used by the neighborhood. Derive pristine from that value instead
		# of evaluating Planet.pristine_height() a second time for every brush sample.
		var current_height: float = _snapshot_final_height(address, snap, height_cache)
		var pristine: float = current_height - before_delta
		var mean_height: float = _neighbor_mean_height(address, neighbor_step, snap, height_cache)
		var amount: float = clampf(_smooth_strength * float(sample["weight"]), 0.0, 1.0)
		var desired_height: float = lerpf(current_height, mean_height, amount)
		var desired_delta: float = clampf(
			desired_height - pristine,
			SCULPT_MIN_OFFSET_M,
			SCULPT_MAX_OFFSET_M)
		if absf(desired_delta - before_delta) <= 1e-6:
			continue
		writes.append({"address": address, "value": desired_delta})
	return _apply_absolute_delta_writes(writes, center_dir, planet_radius)


func _gpu_flatten_supported() -> bool:
	if _pristine_batch_query == null:
		return false
	return bool(_pristine_batch_query.get("supported")) \
		and bool(_pristine_batch_query.get("ready_state")) \
		and not bool(_pristine_batch_query.get("failed")) \
		and Planet.global_height_texture != null \
		and Planet.global_height_face_res > 0


func _skip_redundant_gpu_enqueue(direction: Vector3, continuous: bool,
		planet_radius: float) -> bool:
	if not continuous:
		return false
	var reference: Vector3 = _gpu_last_enqueued_dir
	if reference.length_squared() <= 0.99:
		reference = _last_sculpt_dir
	if reference.length_squared() <= 0.99:
		return false
	var arc_distance: float = acos(clampf(reference.dot(direction.normalized()), -1.0, 1.0)) * planet_radius
	return arc_distance < maxf(_sculpt_radius_m * 0.16, Deltas.sample_spacing(planet_radius) * 1.5)


func _start_next_gpu_flatten_job() -> void:
	if not _gpu_flatten_active.is_empty() or _gpu_flatten_queue.is_empty():
		_finish_deferred_gpu_commit_if_ready()
		return
	var job: Dictionary = _gpu_flatten_queue.pop_front()
	if String(job["body_id"]) != _active_body_id():
		_start_next_gpu_flatten_job()
		return
	var center: Vector3 = job["center"]
	var planet_radius: float = float(job["planet_radius"])
	var radius_m: float = float(job["radius_m"])
	var spacing_m: float = maxf(Deltas.sample_spacing(planet_radius), 0.001)
	job["delta_snapshot"] = Deltas.snapshot_for_bounds(
		center,
		(radius_m + spacing_m * 3.0) / planet_radius)
	var directions: Array[Vector3] = []
	var samples: Array = job["samples"]
	directions.resize(samples.size())
	for index: int in samples.size():
		directions[index] = (samples[index] as Dictionary)["dir"]
	job["dispatch_us"] = Time.get_ticks_usec()
	_gpu_flatten_active = job
	var serial: int = int(job["serial"])
	var callback: Callable = Callable(self, "_on_gpu_flatten_result").bind(serial)
	if _pristine_batch_query == null \
			or not bool(_pristine_batch_query.call("request", directions, callback)):
		var fallback_job: Dictionary = _gpu_flatten_active
		_gpu_flatten_active = {}
		_apply_flatten_job_cpu(fallback_job)
		_start_next_gpu_flatten_job()


func _on_gpu_flatten_result(success: bool, heights: PackedFloat32Array, serial: int) -> void:
	if _gpu_flatten_active.is_empty() or int(_gpu_flatten_active.get("serial", -1)) != serial:
		return
	var job: Dictionary = _gpu_flatten_active
	_gpu_flatten_active = {}
	if String(job["body_id"]) != _active_body_id():
		_start_next_gpu_flatten_job()
		return
	var samples: Array = job["samples"]
	var valid_result: bool = success and heights.size() == samples.size()
	if valid_result:
		for height: float in heights:
			if not is_finite(height):
				valid_result = false
				break
	if not valid_result:
		_apply_flatten_job_cpu(job)
		_start_next_gpu_flatten_job()
		return

	var finalize_started_us: int = Time.get_ticks_usec()
	var changed: int = _apply_flatten_job_with_pristine(job, heights)
	_gpu_last_query_count = heights.size()
	_gpu_last_latency_ms = float(maxi(0, finalize_started_us - int(job["dispatch_us"]))) / 1000.0
	_gpu_last_finalize_ms = float(maxi(0, Time.get_ticks_usec() - finalize_started_us)) / 1000.0
	_gpu_last_fallback = false
	if changed > 0:
		_last_sculpt_dir = job["center"]
		_set_status("Flattened terrain asynchronously: %d samples changed • target %.2f m MSL • GPU %.3f ms + finalize %.3f ms." % [
			changed,
			float(job["target_height_m"]),
			_gpu_last_latency_ms,
			_gpu_last_finalize_ms,
		])
	_start_next_gpu_flatten_job()


func _apply_flatten_job_with_pristine(job: Dictionary,
		heights: PackedFloat32Array) -> int:
	var samples: Array = job["samples"]
	var snap: Dictionary = job["delta_snapshot"]
	var target_height_m: float = float(job["target_height_m"])
	var strength: float = float(job["strength"])
	var writes: Array[Dictionary] = []
	for index: int in samples.size():
		var sample: Dictionary = samples[index]
		var sample_dir: Vector3 = sample["dir"]
		var pristine: float = heights[index]
		var before_delta: float = Deltas.offset_at_snapshot(sample_dir, snap)
		var current_height: float = pristine + before_delta
		var amount: float = clampf(strength * float(sample["weight"]), 0.0, 1.0)
		var desired_height: float = lerpf(current_height, target_height_m, amount)
		var desired_delta: float = clampf(
			desired_height - pristine,
			SCULPT_MIN_OFFSET_M,
			SCULPT_MAX_OFFSET_M)
		if absf(desired_delta - before_delta) <= 1e-6:
			continue
		writes.append({"address": sample["address"], "value": desired_delta})
	return _apply_absolute_delta_writes(
		writes,
		job["center"],
		float(job["planet_radius"]))


func _apply_flatten_job_cpu(job: Dictionary) -> void:
	var samples: Array = job["samples"]
	var heights := PackedFloat32Array()
	heights.resize(samples.size())
	var started_us: int = Time.get_ticks_usec()
	for index: int in samples.size():
		var sample: Dictionary = samples[index]
		heights[index] = _generated_pristine_height(sample["dir"])
	var changed: int = _apply_flatten_job_with_pristine(job, heights)
	_gpu_last_query_count = samples.size()
	_gpu_last_latency_ms = 0.0
	_gpu_last_finalize_ms = float(maxi(0, Time.get_ticks_usec() - started_us)) / 1000.0
	_gpu_last_fallback = true
	if changed > 0:
		_last_sculpt_dir = job["center"]
		_set_status("Flatten GPU request fell back to CPU: %d samples changed • %.3f ms." % [
			changed, _gpu_last_finalize_ms])


func _has_pending_gpu_sculpt() -> bool:
	return not _gpu_flatten_active.is_empty() or not _gpu_flatten_queue.is_empty()


func _commit_interactive_transactions() -> void:
	if _has_pending_gpu_sculpt():
		if _sculpt_transaction_active:
			_gpu_sculpt_commit_deferred = true
		_commit_biome_transaction()
		_set_status("Finishing queued GPU terrain shaping before committing this sculpt drag…")
		return
	super._commit_interactive_transactions()


func _finish_deferred_gpu_commit_if_ready() -> void:
	if _has_pending_gpu_sculpt():
		return
	_gpu_last_enqueued_dir = Vector3.ZERO
	if not _gpu_sculpt_commit_deferred:
		return
	_gpu_sculpt_commit_deferred = false
	_commit_sculpt_transaction()
	_set_status("GPU terrain shaping finished and the complete drag was committed as one history action.")


func _set_placement_mode(mode: int) -> void:
	if _has_pending_gpu_sculpt() and mode != _placement_mode:
		_set_status("Finish the queued GPU terrain stamp before switching authoring tools.")
		return
	super._set_placement_mode(mode)


func _clear_sculpt_layer() -> void:
	if _has_pending_gpu_sculpt():
		_set_status("Finish the queued GPU terrain stamp before clearing the sculpt layer.")
		return
	super._clear_sculpt_layer()


func _on_undo_pressed() -> void:
	if _has_pending_gpu_sculpt():
		_set_status("Finish the queued GPU terrain stamp before Undo.")
		return
	super._on_undo_pressed()


func _on_redo_pressed() -> void:
	if _has_pending_gpu_sculpt():
		_set_status("Finish the queued GPU terrain stamp before Redo.")
		return
	super._on_redo_pressed()


func _on_apply_pressed() -> void:
	if _has_pending_gpu_sculpt():
		_set_status("Finish the queued GPU terrain stamp before Apply.")
		return
	super._on_apply_pressed()


func _on_revert_pressed() -> void:
	if _has_pending_gpu_sculpt():
		_set_status("Finish the queued GPU terrain stamp before Revert.")
		return
	super._on_revert_pressed()


func _on_body_selected(index: int) -> void:
	if _has_pending_gpu_sculpt():
		_set_status("Finish the queued GPU terrain stamp before changing celestial body.")
		return
	super._on_body_selected(index)


func _on_preset_selected(path: String) -> void:
	if _has_pending_gpu_sculpt():
		_set_status("Finish the queued GPU terrain stamp before loading another preset.")
		return
	super._on_preset_selected(path)


func _discard_interactive_transactions() -> void:
	if _pristine_batch_query != null:
		_pristine_batch_query.call("cancel_pending")
	_gpu_flatten_queue.clear()
	_gpu_flatten_active.clear()
	_gpu_sculpt_commit_deferred = false
	_gpu_last_enqueued_dir = Vector3.ZERO
	super._discard_interactive_transactions()
