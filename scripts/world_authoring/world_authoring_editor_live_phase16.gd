class_name WorldAuthoringLiveEditorPhase16
extends "res://scripts/world_authoring/world_authoring_editor_live_phase15.gd"
## Phase 16: compact brush samples from collection through publication.
##
## Phase 15 removed the second target-write Dictionary array. The remaining large
## allocation was the first brush footprint itself: one Dictionary containing
## address/direction/weight per accepted lattice sample. This layer represents one
## complete stamp with only three dense buffers:
##   PackedInt64Array  canonical lattice addresses
##   PackedVector3Array unit directions
##   PackedFloat32Array falloff weights
##
## Raise/Lower/Erase consume those buffers directly. Flatten/Smooth/Thermal queue
## them unchanged and use GPUPristineTerrainBatchQuery.request_packed(), so a large
## brush no longer expands accepted samples into Variant objects before compute.
## Legacy dictionary helpers remain inherited for deterministic compatibility tests
## and for older call sites; live Phase-16 placement routes through compact paths.

var _phase16_last_sample_count: int = 0
var _phase16_last_collect_ms: float = 0.0
var _phase16_last_examined: int = 0
var _phase16_last_compact_bytes: int = 0


func _build_terrain_page() -> void:
	super._build_terrain_page()
	if _world_host == null:
		return
	_section("Compact brush footprint")
	var label := Label.new()
	label.modulate = Color(0.64, 0.76, 0.86)
	if _phase16_last_sample_count <= 0:
		label.text = "No compact live brush footprint measured yet."
	else:
		label.text = "%d samples • %d examined • %.3f ms • %.2f MiB dense buffers" % [
			_phase16_last_sample_count,
			_phase16_last_examined,
			_phase16_last_collect_ms,
			float(_phase16_last_compact_bytes) / (1024.0 * 1024.0),
		]
	_workspace.add_child(label)
	_add_note("Live sculpt footprints use int64 address + Vector3 direction + float weight packed arrays. No per-sample Dictionary is allocated on the normal Phase-16 path. Forward+/Mobile pristine queries upload PackedVector3Array directly; CPU fallback consumes the same compact buffers.")


func _collect_sculpt_samples_compact(center_dir: Vector3, planet_radius: float) -> Dictionary:
	var started_us: int = Time.get_ticks_usec()
	var addresses := PackedInt64Array()
	var directions := PackedVector3Array()
	var weights := PackedFloat32Array()
	_phase16_last_examined = 0
	if center_dir.length_squared() < 0.5 or _sculpt_radius_m <= 0.0 or planet_radius <= 1.0:
		_record_compact_collection(started_us, 0, 0)
		return {"addresses": addresses, "directions": directions, "weights": weights}

	var center: Vector3 = center_dir.normalized()
	var spacing_m: float = maxf(Deltas.sample_spacing(planet_radius), 0.001)
	var center_lattice: Array = Deltas.dir_to_lattice(center)
	var face: int = int(center_lattice[0])
	var center_i: int = int(round(float(center_lattice[1])))
	var center_j: int = int(round(float(center_lattice[2])))
	var extent: int = maxi(1, int(ceil(_sculpt_radius_m / spacing_m)) + 2)
	var hard: float = clampf(_sculpt_hardness, 0.0, 0.98)
	var angular_radius: float = minf(_sculpt_radius_m / planet_radius, PI)
	var minimum_dot: float = cos(angular_radius)
	var inverse_angular_radius: float = 1.0 / maxf(angular_radius, 1e-12)
	var fast_arc: bool = angular_radius <= FAST_ARC_MAX_ANGLE_RAD
	var visited: Dictionary = {}

	for source_j: int in range(center_j - extent, center_j + extent + 1):
		for source_i: int in range(center_i - extent, center_i + extent + 1):
			_phase16_last_examined += 1
			var address: Vector3i = Deltas.canonical_address(face, source_i, source_j)
			if address.x < 0:
				continue
			var packed_address: int = Deltas.pack_address(address)
			if packed_address < 0 or visited.has(packed_address):
				continue
			visited[packed_address] = true
			var sample_dir: Vector3 = Deltas.lattice_to_dir(
				address.x, float(address.y), float(address.z))
			var dot_value: float = clampf(center.dot(sample_dir), -1.0, 1.0)
			if dot_value < minimum_dot:
				continue

			var angle_rad: float
			if fast_arc:
				var chord_sq: float = maxf(0.0, 2.0 - 2.0 * dot_value)
				var chord: float = sqrt(chord_sq)
				var chord2: float = chord_sq
				angle_rad = chord * (1.0 + chord2 * (1.0 / 24.0) \
					+ chord2 * chord2 * (3.0 / 640.0))
			else:
				angle_rad = acos(dot_value)
			var normalized_distance: float = clampf(
				angle_rad * inverse_angular_radius, 0.0, 1.0)
			var weight: float = _sculpt_profile_weight(normalized_distance, hard)
			if weight <= 0.0001:
				continue
			addresses.append(packed_address)
			directions.append(sample_dir)
			weights.append(weight)

	_record_compact_collection(started_us, addresses.size(), _phase16_last_examined)
	# Preserve Phase-10's candidate telemetry contract for the live compact path.
	if not _telemetry_active_tool.is_empty():
		_telemetry_candidate_count = addresses.size()
	return {"addresses": addresses, "directions": directions, "weights": weights}


func _record_compact_collection(started_us: int, sample_count: int,
		examined_count: int) -> void:
	_phase16_last_sample_count = sample_count
	_phase16_last_examined = examined_count
	_phase16_last_collect_ms = float(maxi(0, Time.get_ticks_usec() - started_us)) / 1000.0
	# PackedVector3Array stores three float32 components in its serialized payload.
	_phase16_last_compact_bytes = sample_count * (8 + 12 + 4)


func _place_sculpt_stroke(direction: Vector3, continuous: bool, sign_value: float) -> void:
	if Planet.cfg == null or not Planet.ready_state:
		return
	var effective_sign: float = -sign_value if Input.is_key_pressed(KEY_SHIFT) else sign_value
	var planet_radius: float = maxf(float(Planet.cfg.planet_radius), 1.0)
	if _skip_redundant_sculpt_stamp(direction, continuous, planet_radius):
		return
	var samples: Dictionary = _collect_sculpt_samples_compact(direction, planet_radius)
	var sample_addresses: PackedInt64Array = samples["addresses"]
	var sample_directions: PackedVector3Array = samples["directions"]
	var sample_weights: PackedFloat32Array = samples["weights"]
	if sample_addresses.is_empty():
		return
	var spacing_m: float = maxf(Deltas.sample_spacing(planet_radius), 0.001)
	var snap: Dictionary = Deltas.snapshot_for_bounds(
		direction.normalized(),
		(_sculpt_radius_m + spacing_m * 3.0) / planet_radius)
	var target_addresses := PackedInt64Array()
	var target_values := PackedFloat32Array()
	for index: int in sample_addresses.size():
		var sample_dir: Vector3 = sample_directions[index]
		var before: float = Deltas.offset_at_snapshot(sample_dir, snap)
		var desired: float = clampf(
			before + _sculpt_strength_m * effective_sign * sample_weights[index],
			SCULPT_MIN_OFFSET_M,
			SCULPT_MAX_OFFSET_M)
		if absf(desired - before) <= 1e-7:
			continue
		target_addresses.append(sample_addresses[index])
		target_values.append(desired)
	var changed: int = _apply_packed_absolute_delta_writes(
		target_addresses, target_values, direction, planet_radius, _sculpt_radius_m)
	if changed <= 0:
		return
	_last_sculpt_dir = direction
	var verb: String = "Raised" if effective_sign > 0.0 else "Lowered"
	_set_status("%s terrain: %d samples • %.1f m radius • %.2f m stamp • %s falloff • compact footprint." % [
		verb, changed, _sculpt_radius_m, _sculpt_strength_m,
		FALLOFF_NAMES[_sculpt_falloff_profile]])


func _place_erase_stroke(direction: Vector3, continuous: bool) -> void:
	if Planet.cfg == null or not Planet.ready_state or Deltas.is_empty():
		return
	var planet_radius: float = maxf(float(Planet.cfg.planet_radius), 1.0)
	if _skip_redundant_sculpt_stamp(direction, continuous, planet_radius):
		return
	var samples: Dictionary = _collect_sculpt_samples_compact(direction, planet_radius)
	var sample_addresses: PackedInt64Array = samples["addresses"]
	var sample_directions: PackedVector3Array = samples["directions"]
	var sample_weights: PackedFloat32Array = samples["weights"]
	if sample_addresses.is_empty():
		return
	var spacing_m: float = maxf(Deltas.sample_spacing(planet_radius), 0.001)
	var snap: Dictionary = Deltas.snapshot_for_bounds(
		direction.normalized(),
		(_sculpt_radius_m + spacing_m * 3.0) / planet_radius)
	var target_addresses := PackedInt64Array()
	var target_values := PackedFloat32Array()
	for index: int in sample_addresses.size():
		var sample_dir: Vector3 = sample_directions[index]
		var before: float = Deltas.offset_at_snapshot(sample_dir, snap)
		if absf(before) <= 1e-7:
			continue
		var amount: float = clampf(_erase_strength * sample_weights[index], 0.0, 1.0)
		var desired: float = lerpf(before, 0.0, amount)
		if absf(desired) < 1e-5:
			desired = 0.0
		if absf(desired - before) <= 1e-7:
			continue
		target_addresses.append(sample_addresses[index])
		target_values.append(desired)
	var changed: int = _apply_packed_absolute_delta_writes(
		target_addresses, target_values, direction, planet_radius, _sculpt_radius_m)
	if changed <= 0:
		return
	_last_sculpt_dir = direction
	_set_status("Erased terrain edits: %d samples • %.1f m radius • %.0f%% • %s falloff • compact footprint." % [
		changed, _sculpt_radius_m, _erase_strength * 100.0,
		FALLOFF_NAMES[_sculpt_falloff_profile]])


func _place_flatten_stroke(direction: Vector3, continuous: bool) -> void:
	if Planet.cfg == null or not Planet.ready_state or not _flatten_target_valid:
		return
	var planet_radius: float = maxf(float(Planet.cfg.planet_radius), 1.0)
	if _skip_redundant_gpu_enqueue(direction, continuous, planet_radius):
		return
	var samples: Dictionary = _collect_sculpt_samples_compact(direction, planet_radius)
	var sample_addresses: PackedInt64Array = samples["addresses"]
	if sample_addresses.is_empty():
		return
	var job := {
		"serial": _gpu_flatten_serial,
		"body_id": _active_body_id(),
		"center": direction.normalized(),
		"planet_radius": planet_radius,
		"radius_m": _sculpt_radius_m,
		"strength": _flatten_strength,
		"target_height_m": _flatten_target_height_m,
		"sample_addresses": sample_addresses,
		"sample_directions": samples["directions"],
		"sample_weights": samples["weights"],
		"enqueued_us": Time.get_ticks_usec(),
	}
	_gpu_flatten_serial += 1
	_gpu_last_enqueued_dir = direction.normalized()
	if not _gpu_flatten_supported():
		job["delta_snapshot"] = _snapshot_compact_job(job, 3.0)
		_apply_flatten_compact_cpu(job)
		return
	_gpu_flatten_queue.append(job)
	_set_status("Flatten queued: %d compact samples • %d stamp(s) pending GPU evaluation." % [
		sample_addresses.size(),
		_gpu_flatten_queue.size() + (0 if _gpu_flatten_active.is_empty() else 1)])
	_start_next_gpu_flatten_job()


func _start_next_gpu_flatten_job() -> void:
	if not _gpu_flatten_active.is_empty() or _gpu_flatten_queue.is_empty():
		_finish_deferred_gpu_commit_if_ready()
		return
	if _gpu_query_busy():
		return
	var job: Dictionary = _gpu_flatten_queue.pop_front()
	if String(job["body_id"]) != _active_body_id():
		_start_next_gpu_flatten_job()
		return
	job["delta_snapshot"] = _snapshot_compact_job(job, 3.0)
	job["dispatch_us"] = Time.get_ticks_usec()
	_gpu_flatten_active = job
	var serial: int = int(job["serial"])
	var callback: Callable = Callable(self, "_on_gpu_flatten_result").bind(serial)
	var directions: PackedVector3Array = job["sample_directions"]
	if _pristine_batch_query == null \
			or not bool(_pristine_batch_query.call("request_packed", directions, callback)):
		var fallback_job: Dictionary = _gpu_flatten_active
		_gpu_flatten_active = {}
		_apply_flatten_compact_cpu(fallback_job)
		_start_next_gpu_flatten_job()


func _on_gpu_flatten_result(success: bool, heights: PackedFloat32Array, serial: int) -> void:
	if _gpu_flatten_active.is_empty() or int(_gpu_flatten_active.get("serial", -1)) != serial:
		return
	var job: Dictionary = _gpu_flatten_active
	_gpu_flatten_active = {}
	if String(job["body_id"]) != _active_body_id():
		_start_next_gpu_flatten_job()
		return
	var sample_addresses: PackedInt64Array = job["sample_addresses"]
	if not _valid_gpu_heights(success, heights, sample_addresses.size()):
		_apply_flatten_compact_cpu(job)
		_start_next_gpu_flatten_job()
		return
	var finalize_started_us: int = Time.get_ticks_usec()
	var changed: int = _apply_flatten_job_with_pristine(job, heights)
	_record_gpu_shape_metrics("Flatten", job, heights.size(), finalize_started_us, false)
	if changed > 0:
		_last_sculpt_dir = job["center"]
		_set_status("Flattened terrain asynchronously: %d samples changed • target %.2f m MSL • GPU %.3f ms + finalize %.3f ms • compact." % [
			changed,
			float(job["target_height_m"]),
			_gpu_last_latency_ms,
			_gpu_last_finalize_ms,
		])
	_start_next_gpu_flatten_job()


func _apply_flatten_job_with_pristine(job: Dictionary,
		heights: PackedFloat32Array) -> int:
	if not job.has("sample_addresses"):
		return super._apply_flatten_job_with_pristine(job, heights)
	var sample_addresses: PackedInt64Array = job["sample_addresses"]
	var sample_directions: PackedVector3Array = job["sample_directions"]
	var sample_weights: PackedFloat32Array = job["sample_weights"]
	var snap: Dictionary = job["delta_snapshot"]
	var target_height_m: float = float(job["target_height_m"])
	var strength: float = float(job["strength"])
	var target_addresses := PackedInt64Array()
	var target_values := PackedFloat32Array()
	var count: int = mini(sample_addresses.size(), heights.size())
	for index: int in count:
		var sample_dir: Vector3 = sample_directions[index]
		var pristine: float = heights[index]
		var before_delta: float = Deltas.offset_at_snapshot(sample_dir, snap)
		var current_height: float = pristine + before_delta
		var amount: float = clampf(strength * sample_weights[index], 0.0, 1.0)
		var desired_height: float = lerpf(current_height, target_height_m, amount)
		var desired_delta: float = clampf(
			desired_height - pristine,
			SCULPT_MIN_OFFSET_M,
			SCULPT_MAX_OFFSET_M)
		if absf(desired_delta - before_delta) <= 1e-6:
			continue
		target_addresses.append(sample_addresses[index])
		target_values.append(desired_delta)
	return _apply_packed_absolute_delta_writes(
		target_addresses,
		target_values,
		job["center"],
		float(job["planet_radius"]),
		float(job["radius_m"]))


func _apply_flatten_compact_cpu(job: Dictionary) -> void:
	var directions: PackedVector3Array = job["sample_directions"]
	var started_us: int = Time.get_ticks_usec()
	var heights: PackedFloat32Array = _cpu_pristine_for_packed_directions(directions)
	var changed: int = _apply_flatten_job_with_pristine(job, heights)
	_record_cpu_shape_fallback("Flatten", heights.size(), started_us)
	if changed > 0:
		_last_sculpt_dir = job["center"]
		_set_status("Flatten compact CPU fallback: %d samples changed • %.3f ms." % [
			changed, _gpu_last_finalize_ms])


func _place_smooth_stroke(direction: Vector3, continuous: bool) -> void:
	if Planet.cfg == null or not Planet.ready_state:
		return
	var planet_radius: float = maxf(float(Planet.cfg.planet_radius), 1.0)
	if _skip_redundant_gpu_enqueue(direction, continuous, planet_radius):
		return
	var samples: Dictionary = _collect_sculpt_samples_compact(direction, planet_radius)
	var sample_addresses: PackedInt64Array = samples["addresses"]
	if sample_addresses.is_empty():
		return
	var spacing_m: float = maxf(Deltas.sample_spacing(planet_radius), 0.001)
	var neighbor_radius_m: float = clampf(
		_sculpt_radius_m * 0.12, spacing_m, SMOOTH_MAX_NEIGHBOR_RADIUS_M)
	var neighbor_step: int = maxi(1, int(round(neighbor_radius_m / spacing_m)))
	var job := {
		"serial": _gpu_shape_serial,
		"body_id": _active_body_id(),
		"center": direction.normalized(),
		"planet_radius": planet_radius,
		"radius_m": _sculpt_radius_m,
		"strength": _smooth_strength,
		"neighbor_step": neighbor_step,
		"sample_addresses": sample_addresses,
		"sample_directions": samples["directions"],
		"sample_weights": samples["weights"],
	}
	_gpu_shape_serial += 1
	_gpu_last_enqueued_dir = direction.normalized()
	if not _gpu_flatten_supported():
		job["delta_snapshot"] = _snapshot_compact_job(job, float(neighbor_step + 3))
		_prepare_compact_smooth_query(job)
		_apply_smooth_compact_cpu(job)
		return
	_gpu_smooth_queue.append(job)
	_set_status("Smooth queued: %d compact samples • %d stamp(s) pending." % [
		sample_addresses.size(),
		_gpu_smooth_queue.size() + (0 if _gpu_smooth_active.is_empty() else 1)])
	_start_next_gpu_smooth_job()


func _start_next_gpu_smooth_job() -> void:
	if not _gpu_smooth_active.is_empty() or _gpu_smooth_queue.is_empty():
		_finish_deferred_gpu_commit_if_ready()
		return
	if _gpu_query_busy():
		return
	var job: Dictionary = _gpu_smooth_queue.pop_front()
	if String(job["body_id"]) != _active_body_id():
		_start_next_gpu_smooth_job()
		return
	var neighbor_step: int = int(job["neighbor_step"])
	job["delta_snapshot"] = _snapshot_compact_job(job, float(neighbor_step + 3))
	_prepare_compact_smooth_query(job)
	job["dispatch_us"] = Time.get_ticks_usec()
	_gpu_smooth_active = job
	var serial: int = int(job["serial"])
	var callback: Callable = Callable(self, "_on_gpu_smooth_result").bind(serial)
	var directions: PackedVector3Array = job["query_directions"]
	if _pristine_batch_query == null \
			or not bool(_pristine_batch_query.call("request_packed", directions, callback)):
		var fallback_job: Dictionary = _gpu_smooth_active
		_gpu_smooth_active = {}
		_apply_smooth_compact_cpu(fallback_job)
		_start_next_gpu_smooth_job()


func _prepare_compact_smooth_query(job: Dictionary) -> void:
	var source_addresses: PackedInt64Array = job["sample_addresses"]
	var step: int = int(job["neighbor_step"])
	var query_addresses := PackedInt64Array()
	var query_directions := PackedVector3Array()
	var query_index: Dictionary = {}
	for packed_source: int in source_addresses:
		var source: Vector3i = Deltas.unpack_address(packed_source)
		for oy: int in [-step, 0, step]:
			for ox: int in [-step, 0, step]:
				var address: Vector3i = Deltas.canonical_address(
					source.x, source.y + ox, source.z + oy)
				var packed: int = Deltas.pack_address(address)
				if packed < 0 or query_index.has(packed):
					continue
				query_index[packed] = query_addresses.size()
				query_addresses.append(packed)
				query_directions.append(Deltas.lattice_to_dir(
					address.x, float(address.y), float(address.z)))
	job["query_addresses"] = query_addresses
	job["query_directions"] = query_directions
	job["query_index"] = query_index


func _on_gpu_smooth_result(success: bool, heights: PackedFloat32Array, serial: int) -> void:
	if _gpu_smooth_active.is_empty() or int(_gpu_smooth_active.get("serial", -1)) != serial:
		return
	var job: Dictionary = _gpu_smooth_active
	_gpu_smooth_active = {}
	if String(job["body_id"]) != _active_body_id():
		_start_next_gpu_smooth_job()
		return
	var query_addresses: PackedInt64Array = job["query_addresses"]
	if not _valid_gpu_heights(success, heights, query_addresses.size()):
		_apply_smooth_compact_cpu(job)
		_start_next_gpu_smooth_job()
		return
	var finalize_started_us: int = Time.get_ticks_usec()
	var changed: int = _apply_smooth_job_with_pristine(job, heights)
	_record_gpu_shape_metrics("Smooth", job, heights.size(), finalize_started_us, false)
	if changed > 0:
		_last_sculpt_dir = job["center"]
		_set_status("Smoothed terrain asynchronously: %d samples changed • GPU %.3f ms + finalize %.3f ms • compact." % [
			changed, _gpu_last_latency_ms, _gpu_last_finalize_ms])
	_start_next_gpu_smooth_job()


func _apply_smooth_job_with_pristine(job: Dictionary,
		heights: PackedFloat32Array) -> int:
	if not job.has("sample_addresses"):
		return super._apply_smooth_job_with_pristine(job, heights)
	var sample_addresses: PackedInt64Array = job["sample_addresses"]
	var sample_directions: PackedVector3Array = job["sample_directions"]
	var sample_weights: PackedFloat32Array = job["sample_weights"]
	var query_directions: PackedVector3Array = job["query_directions"]
	var query_index: Dictionary = job["query_index"]
	var snap: Dictionary = job["delta_snapshot"]
	var step: int = int(job["neighbor_step"])
	var strength: float = float(job["strength"])
	var target_addresses := PackedInt64Array()
	var target_values := PackedFloat32Array()

	for sample_index: int in sample_addresses.size():
		var packed_source: int = sample_addresses[sample_index]
		if not query_index.has(packed_source):
			continue
		var source: Vector3i = Deltas.unpack_address(packed_source)
		var source_query_index: int = int(query_index[packed_source])
		var source_dir: Vector3 = sample_directions[sample_index]
		var pristine: float = heights[source_query_index]
		var before_delta: float = Deltas.offset_at_snapshot(source_dir, snap)
		var current_height: float = pristine + before_delta
		var weighted_sum: float = 0.0
		var weight_sum: float = 0.0
		for oy: int in [-step, 0, step]:
			for ox: int in [-step, 0, step]:
				var neighbor: Vector3i = Deltas.canonical_address(
					source.x, source.y + ox, source.z + oy)
				var packed_neighbor: int = Deltas.pack_address(neighbor)
				if packed_neighbor < 0 or not query_index.has(packed_neighbor):
					continue
				var neighbor_index: int = int(query_index[packed_neighbor])
				var neighbor_dir: Vector3 = query_directions[neighbor_index]
				var final_height: float = heights[neighbor_index] \
					+ Deltas.offset_at_snapshot(neighbor_dir, snap)
				var axis_weight_x: float = 2.0 if ox == 0 else 1.0
				var axis_weight_y: float = 2.0 if oy == 0 else 1.0
				var neighbor_weight: float = axis_weight_x * axis_weight_y
				weighted_sum += final_height * neighbor_weight
				weight_sum += neighbor_weight
		if weight_sum <= 0.0:
			continue
		var mean_height: float = weighted_sum / weight_sum
		var amount: float = clampf(
			strength * sample_weights[sample_index], 0.0, 1.0)
		var desired_height: float = lerpf(current_height, mean_height, amount)
		var desired_delta: float = clampf(
			desired_height - pristine,
			SCULPT_MIN_OFFSET_M,
			SCULPT_MAX_OFFSET_M)
		if absf(desired_delta - before_delta) <= 1e-6:
			continue
		target_addresses.append(packed_source)
		target_values.append(desired_delta)
	return _apply_packed_absolute_delta_writes(
		target_addresses,
		target_values,
		job["center"],
		float(job["planet_radius"]),
		float(job["radius_m"]))


func _apply_smooth_compact_cpu(job: Dictionary) -> void:
	var directions: PackedVector3Array = job["query_directions"]
	var started_us: int = Time.get_ticks_usec()
	var heights: PackedFloat32Array = _cpu_pristine_for_packed_directions(directions)
	var changed: int = _apply_smooth_job_with_pristine(job, heights)
	_record_cpu_shape_fallback("Smooth", heights.size(), started_us)
	if changed > 0:
		_last_sculpt_dir = job["center"]
		_set_status("Smooth compact CPU fallback: %d samples changed • %.3f ms." % [
			changed, _gpu_last_finalize_ms])


func _place_thermal_stroke(direction: Vector3, continuous: bool) -> void:
	if Planet.cfg == null or not Planet.ready_state:
		return
	var planet_radius: float = maxf(float(Planet.cfg.planet_radius), 1.0)
	if _skip_redundant_gpu_enqueue(direction, continuous, planet_radius):
		return
	var samples: Dictionary = _collect_sculpt_samples_compact(direction, planet_radius)
	var sample_addresses: PackedInt64Array = samples["addresses"]
	if sample_addresses.is_empty():
		return
	var job := {
		"serial": _gpu_shape_serial,
		"body_id": _active_body_id(),
		"center": direction.normalized(),
		"planet_radius": planet_radius,
		"radius_m": _sculpt_radius_m,
		"talus_deg": _thermal_talus_deg,
		"strength": _thermal_strength,
		"sample_addresses": sample_addresses,
		"sample_directions": samples["directions"],
		"sample_weights": samples["weights"],
	}
	_gpu_shape_serial += 1
	_gpu_last_enqueued_dir = direction.normalized()
	if not _gpu_flatten_supported():
		job["delta_snapshot"] = _snapshot_compact_job(job, 5.0)
		job["spacing_m"] = maxf(Deltas.sample_spacing(planet_radius), 0.001)
		_prepare_compact_thermal_query(job)
		_apply_thermal_compact_cpu(job)
		return
	_gpu_thermal_queue.append(job)
	_set_status("Thermal queued: %d compact samples • %d stamp(s) pending." % [
		sample_addresses.size(),
		_gpu_thermal_queue.size() + (0 if _gpu_thermal_active.is_empty() else 1)])
	_start_next_gpu_thermal_job()


func _start_next_gpu_thermal_job() -> void:
	if not _gpu_thermal_active.is_empty() or _gpu_thermal_queue.is_empty():
		_finish_deferred_gpu_commit_if_ready()
		return
	if _gpu_query_busy():
		return
	var job: Dictionary = _gpu_thermal_queue.pop_front()
	if String(job["body_id"]) != _active_body_id():
		_start_next_gpu_thermal_job()
		return
	var planet_radius: float = float(job["planet_radius"])
	job["spacing_m"] = maxf(Deltas.sample_spacing(planet_radius), 0.001)
	job["delta_snapshot"] = _snapshot_compact_job(job, 5.0)
	_prepare_compact_thermal_query(job)
	job["dispatch_us"] = Time.get_ticks_usec()
	_gpu_thermal_active = job
	var serial: int = int(job["serial"])
	var callback: Callable = Callable(self, "_on_gpu_thermal_result").bind(serial)
	var directions: PackedVector3Array = job["query_directions"]
	if _pristine_batch_query == null \
			or not bool(_pristine_batch_query.call("request_packed", directions, callback)):
		var fallback_job: Dictionary = _gpu_thermal_active
		_gpu_thermal_active = {}
		_apply_thermal_compact_cpu(fallback_job)
		_start_next_gpu_thermal_job()


func _prepare_compact_thermal_query(job: Dictionary) -> void:
	var source_addresses: PackedInt64Array = job["sample_addresses"]
	var query_addresses := PackedInt64Array()
	var query_directions := PackedVector3Array()
	var query_index: Dictionary = {}
	for packed_source: int in source_addresses:
		var source: Vector3i = Deltas.unpack_address(packed_source)
		if not query_index.has(packed_source):
			query_index[packed_source] = query_addresses.size()
			query_addresses.append(packed_source)
			query_directions.append(Deltas.lattice_to_dir(
				source.x, float(source.y), float(source.z)))
		for offset: Vector2i in THERMAL_NEIGHBORS:
			var neighbor: Vector3i = Deltas.canonical_address(
				source.x, source.y + offset.x, source.z + offset.y)
			var packed_neighbor: int = Deltas.pack_address(neighbor)
			if packed_neighbor < 0 or query_index.has(packed_neighbor):
				continue
			query_index[packed_neighbor] = query_addresses.size()
			query_addresses.append(packed_neighbor)
			query_directions.append(Deltas.lattice_to_dir(
				neighbor.x, float(neighbor.y), float(neighbor.z)))
	job["query_addresses"] = query_addresses
	job["query_directions"] = query_directions
	job["query_index"] = query_index


func _on_gpu_thermal_result(success: bool, heights: PackedFloat32Array, serial: int) -> void:
	if _gpu_thermal_active.is_empty() or int(_gpu_thermal_active.get("serial", -1)) != serial:
		return
	var job: Dictionary = _gpu_thermal_active
	_gpu_thermal_active = {}
	if String(job["body_id"]) != _active_body_id():
		_start_next_gpu_thermal_job()
		return
	var query_addresses: PackedInt64Array = job["query_addresses"]
	if not _valid_gpu_heights(success, heights, query_addresses.size()):
		_apply_thermal_compact_cpu(job)
		_start_next_gpu_thermal_job()
		return
	var finalize_started_us: int = Time.get_ticks_usec()
	var changed: int = _apply_thermal_job_with_pristine(job, heights)
	_record_gpu_shape_metrics("Thermal", job, heights.size(), finalize_started_us, false)
	if changed > 0:
		_last_sculpt_dir = job["center"]
		_set_status("Thermal erosion asynchronously: %d samples changed • GPU %.3f ms + finalize %.3f ms • compact." % [
			changed, _gpu_last_latency_ms, _gpu_last_finalize_ms])
	_start_next_gpu_thermal_job()


func _apply_thermal_job_with_pristine(job: Dictionary,
		heights: PackedFloat32Array) -> int:
	if not job.has("sample_addresses"):
		return super._apply_thermal_job_with_pristine(job, heights)
	var sample_addresses: PackedInt64Array = job["sample_addresses"]
	var sample_weights: PackedFloat32Array = job["sample_weights"]
	var query_directions: PackedVector3Array = job["query_directions"]
	var query_index: Dictionary = job["query_index"]
	var snap: Dictionary = job["delta_snapshot"]
	var spacing_m: float = float(job["spacing_m"])
	var talus_drop_m: float = tan(deg_to_rad(
		clampf(float(job["talus_deg"]), 0.0, 89.0))) * spacing_m
	var strength: float = float(job["strength"])
	# Packed address -> accumulated conservative height transfer.
	var changes: Dictionary = {}

	for sample_index: int in sample_addresses.size():
		var packed_source: int = sample_addresses[sample_index]
		if not query_index.has(packed_source):
			continue
		var source: Vector3i = Deltas.unpack_address(packed_source)
		var source_query_index: int = int(query_index[packed_source])
		var source_dir: Vector3 = query_directions[source_query_index]
		var source_height: float = heights[source_query_index] \
			+ Deltas.offset_at_snapshot(source_dir, snap)
		var source_weight: float = clampf(sample_weights[sample_index], 0.0, 1.0)
		if source_weight <= 0.0001:
			continue

		var downhill_count: int = 0
		for offset: Vector2i in THERMAL_NEIGHBORS:
			var neighbor: Vector3i = Deltas.canonical_address(
				source.x, source.y + offset.x, source.z + offset.y)
			var packed_neighbor: int = Deltas.pack_address(neighbor)
			if packed_neighbor < 0 or packed_neighbor == packed_source \
					or not query_index.has(packed_neighbor):
				continue
			var neighbor_index: int = int(query_index[packed_neighbor])
			var neighbor_height: float = heights[neighbor_index] \
				+ Deltas.offset_at_snapshot(query_directions[neighbor_index], snap)
			if source_height - neighbor_height - talus_drop_m > 1e-5:
				downhill_count += 1
		if downhill_count <= 0:
			continue

		var relaxation: float = clampf(strength * source_weight, 0.0, 1.0)
		var denominator: float = float(downhill_count + 1)
		for offset: Vector2i in THERMAL_NEIGHBORS:
			var neighbor: Vector3i = Deltas.canonical_address(
				source.x, source.y + offset.x, source.z + offset.y)
			var packed_neighbor: int = Deltas.pack_address(neighbor)
			if packed_neighbor < 0 or packed_neighbor == packed_source \
					or not query_index.has(packed_neighbor):
				continue
			var neighbor_index: int = int(query_index[packed_neighbor])
			var neighbor_height: float = heights[neighbor_index] \
				+ Deltas.offset_at_snapshot(query_directions[neighbor_index], snap)
			var excess_m: float = source_height - neighbor_height - talus_drop_m
			if excess_m <= 1e-5:
				continue
			var transfer_m: float = excess_m * relaxation / denominator
			if transfer_m <= 1e-7:
				continue
			changes[packed_source] = float(changes.get(packed_source, 0.0)) - transfer_m
			changes[packed_neighbor] = float(changes.get(packed_neighbor, 0.0)) + transfer_m

	var target_addresses := PackedInt64Array()
	var target_values := PackedFloat32Array()
	for packed_value: Variant in changes.keys():
		var packed_address: int = int(packed_value)
		var change_m: float = float(changes[packed_address])
		if absf(change_m) <= 1e-7:
			continue
		var address: Vector3i = Deltas.unpack_address(packed_address)
		var direction: Vector3 = Deltas.lattice_to_dir(
			address.x, float(address.y), float(address.z))
		var before_delta: float = Deltas.offset_at_snapshot(direction, snap)
		var desired_delta: float = clampf(
			before_delta + change_m,
			SCULPT_MIN_OFFSET_M,
			SCULPT_MAX_OFFSET_M)
		if absf(desired_delta - before_delta) <= 1e-7:
			continue
		target_addresses.append(packed_address)
		target_values.append(desired_delta)
	return _apply_packed_absolute_delta_writes(
		target_addresses,
		target_values,
		job["center"],
		float(job["planet_radius"]),
		float(job["radius_m"]))


func _apply_thermal_compact_cpu(job: Dictionary) -> void:
	var directions: PackedVector3Array = job["query_directions"]
	var started_us: int = Time.get_ticks_usec()
	var heights: PackedFloat32Array = _cpu_pristine_for_packed_directions(directions)
	var changed: int = _apply_thermal_job_with_pristine(job, heights)
	_record_cpu_shape_fallback("Thermal", heights.size(), started_us)
	if changed > 0:
		_last_sculpt_dir = job["center"]
		_set_status("Thermal compact CPU fallback: %d samples changed • %.3f ms." % [
			changed, _gpu_last_finalize_ms])


func _cpu_pristine_for_packed_directions(
		directions: PackedVector3Array) -> PackedFloat32Array:
	var heights := PackedFloat32Array()
	heights.resize(directions.size())
	for index: int in directions.size():
		heights[index] = _generated_pristine_height(directions[index])
	return heights


func _snapshot_compact_job(job: Dictionary, extra_spacing_samples: float) -> Dictionary:
	var planet_radius: float = float(job["planet_radius"])
	var spacing_m: float = maxf(Deltas.sample_spacing(planet_radius), 0.001)
	return Deltas.snapshot_for_bounds(
		(job["center"] as Vector3).normalized(),
		(float(job["radius_m"]) + spacing_m * extra_spacing_samples) / planet_radius)
