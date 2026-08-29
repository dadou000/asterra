class_name WorldAuthoringLiveEditorPhase13
extends "res://scripts/world_authoring/world_authoring_editor_live_phase12.gd"
## Phase 13: move every pristine-heavy shaping brush onto the asynchronous
## persistent-envelope query path.
##
## Smooth batches the unique center + 3x3 neighborhood addresses. Thermal batches
## each source + its four cardinal neighbors. The GPU only supplies pristine macro
## + coast heights; all Deltas snapshots, smoothing math, conservative material
## transfers and sparse writes remain CPU-side and deterministic. Jobs are executed
## sequentially so queued drag stamps observe all preceding edits in order.

var _gpu_smooth_queue: Array[Dictionary] = []
var _gpu_smooth_active: Dictionary = {}
var _gpu_thermal_queue: Array[Dictionary] = []
var _gpu_thermal_active: Dictionary = {}
var _gpu_shape_serial: int = 1000000
var _gpu_last_shape_tool: String = "Flatten"


func _build_terrain_page() -> void:
	super._build_terrain_page()
	if _world_host == null:
		return
	_section("Asynchronous shaping coverage")
	var label := Label.new()
	label.modulate = Color(0.64, 0.76, 0.86)
	label.text = "GPU pristine path: Flatten + Smooth + Thermal%s" % [
		(" • last " + _gpu_last_shape_tool) if _gpu_last_query_count > 0 else ""]
	_workspace.add_child(label)
	_add_note("Smooth queries each unique neighborhood address once, and Thermal queries each unique source/cardinal-neighbor address once. CPU fallback remains available when Forward+ compute is unavailable. Raise/Lower/Erase never need pristine terrain sampling and stay on the direct one-lock sparse-write path.")


func _place_smooth_stroke(direction: Vector3, continuous: bool) -> void:
	if Planet.cfg == null or not Planet.ready_state:
		return
	if not _gpu_flatten_supported():
		super._place_smooth_stroke(direction, continuous)
		return
	var planet_radius: float = maxf(float(Planet.cfg.planet_radius), 1.0)
	if _skip_redundant_gpu_enqueue(direction, continuous, planet_radius):
		return
	var samples: Array[Dictionary] = _collect_sculpt_samples(direction, planet_radius)
	if samples.is_empty():
		return
	var spacing_m: float = maxf(Deltas.sample_spacing(planet_radius), 0.001)
	var neighbor_radius_m: float = clampf(_sculpt_radius_m * 0.12, spacing_m, SMOOTH_MAX_NEIGHBOR_RADIUS_M)
	var neighbor_step: int = maxi(1, int(round(neighbor_radius_m / spacing_m)))
	var serial: int = _gpu_shape_serial
	_gpu_shape_serial += 1
	_gpu_smooth_queue.append({
		"serial": serial,
		"body_id": _active_body_id(),
		"center": direction.normalized(),
		"planet_radius": planet_radius,
		"radius_m": _sculpt_radius_m,
		"strength": _smooth_strength,
		"neighbor_step": neighbor_step,
		"samples": samples,
	})
	_gpu_last_enqueued_dir = direction.normalized()
	_set_status("Smooth queued: %d brush samples • %d stamp(s) pending." % [
		samples.size(), _gpu_smooth_queue.size() + (0 if _gpu_smooth_active.is_empty() else 1)])
	_start_next_gpu_smooth_job()


func _place_thermal_stroke(direction: Vector3, continuous: bool) -> void:
	if Planet.cfg == null or not Planet.ready_state:
		return
	if not _gpu_flatten_supported():
		super._place_thermal_stroke(direction, continuous)
		return
	var planet_radius: float = maxf(float(Planet.cfg.planet_radius), 1.0)
	if _skip_redundant_gpu_enqueue(direction, continuous, planet_radius):
		return
	var samples: Array[Dictionary] = _collect_sculpt_samples(direction, planet_radius)
	if samples.is_empty():
		return
	var serial: int = _gpu_shape_serial
	_gpu_shape_serial += 1
	_gpu_thermal_queue.append({
		"serial": serial,
		"body_id": _active_body_id(),
		"center": direction.normalized(),
		"planet_radius": planet_radius,
		"radius_m": _sculpt_radius_m,
		"talus_deg": _thermal_talus_deg,
		"strength": _thermal_strength,
		"samples": samples,
	})
	_gpu_last_enqueued_dir = direction.normalized()
	_set_status("Thermal queued: %d brush samples • %d stamp(s) pending." % [
		samples.size(), _gpu_thermal_queue.size() + (0 if _gpu_thermal_active.is_empty() else 1)])
	_start_next_gpu_thermal_job()


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
	var center: Vector3 = job["center"]
	var planet_radius: float = float(job["planet_radius"])
	var spacing_m: float = maxf(Deltas.sample_spacing(planet_radius), 0.001)
	var neighbor_step: int = int(job["neighbor_step"])
	job["delta_snapshot"] = Deltas.snapshot_for_bounds(
		center,
		(float(job["radius_m"]) + float(neighbor_step + 3) * spacing_m) / planet_radius)
	var prepared: Dictionary = _prepare_smooth_query_addresses(job)
	var directions: Array[Vector3] = prepared["directions"]
	job["addresses"] = prepared["addresses"]
	job["address_index"] = prepared["address_index"]
	job["dispatch_us"] = Time.get_ticks_usec()
	_gpu_smooth_active = job
	var serial: int = int(job["serial"])
	var callback: Callable = Callable(self, "_on_gpu_smooth_result").bind(serial)
	if _pristine_batch_query == null \
			or not bool(_pristine_batch_query.call("request", directions, callback)):
		var fallback_job: Dictionary = _gpu_smooth_active
		_gpu_smooth_active = {}
		_apply_smooth_job_cpu(fallback_job)
		_start_next_gpu_smooth_job()


func _prepare_smooth_query_addresses(job: Dictionary) -> Dictionary:
	var addresses: Array = []
	var address_index: Dictionary = {}
	var samples: Array = job["samples"]
	var step: int = int(job["neighbor_step"])
	for sample_value: Variant in samples:
		var sample: Dictionary = sample_value as Dictionary
		var center_address: Vector3i = sample["address"]
		for oy: int in [-step, 0, step]:
			for ox: int in [-step, 0, step]:
				var address: Vector3i = Deltas.canonical_address(
					center_address.x,
					center_address.y + ox,
					center_address.z + oy)
				_append_unique_gpu_address(addresses, address_index, address)
	var directions: Array[Vector3] = []
	directions.resize(addresses.size())
	for index: int in addresses.size():
		var address: Vector3i = addresses[index]
		directions[index] = Deltas.lattice_to_dir(
			address.x, float(address.y), float(address.z))
	return {"addresses": addresses, "address_index": address_index, "directions": directions}


func _on_gpu_smooth_result(success: bool, heights: PackedFloat32Array, serial: int) -> void:
	if _gpu_smooth_active.is_empty() or int(_gpu_smooth_active.get("serial", -1)) != serial:
		return
	var job: Dictionary = _gpu_smooth_active
	_gpu_smooth_active = {}
	if String(job["body_id"]) != _active_body_id():
		_start_next_gpu_smooth_job()
		return
	var addresses: Array = job["addresses"]
	if not _valid_gpu_heights(success, heights, addresses.size()):
		_apply_smooth_job_cpu(job)
		_start_next_gpu_smooth_job()
		return
	var finalize_started_us: int = Time.get_ticks_usec()
	var changed: int = _apply_smooth_job_with_pristine(job, heights)
	_record_gpu_shape_metrics("Smooth", job, heights.size(), finalize_started_us, false)
	if changed > 0:
		_last_sculpt_dir = job["center"]
		_set_status("Smoothed terrain asynchronously: %d samples changed • GPU %.3f ms + finalize %.3f ms." % [
			changed, _gpu_last_latency_ms, _gpu_last_finalize_ms])
	_start_next_gpu_smooth_job()


func _apply_smooth_job_with_pristine(job: Dictionary,
		heights: PackedFloat32Array) -> int:
	var samples: Array = job["samples"]
	var address_index: Dictionary = job["address_index"]
	var snap: Dictionary = job["delta_snapshot"]
	var step: int = int(job["neighbor_step"])
	var strength: float = float(job["strength"])
	var writes: Array[Dictionary] = []
	for sample_value: Variant in samples:
		var sample: Dictionary = sample_value as Dictionary
		var source: Vector3i = sample["address"]
		var source_key: String = _address_key(source)
		if not address_index.has(source_key):
			continue
		var source_index: int = int(address_index[source_key])
		var source_dir: Vector3 = sample["dir"]
		var pristine: float = heights[source_index]
		var before_delta: float = Deltas.offset_at_snapshot(source_dir, snap)
		var current_height: float = pristine + before_delta
		var weighted_sum: float = 0.0
		var weight_sum: float = 0.0
		for oy: int in [-step, 0, step]:
			for ox: int in [-step, 0, step]:
				var address: Vector3i = Deltas.canonical_address(
					source.x, source.y + ox, source.z + oy)
				var key: String = _address_key(address)
				if not address_index.has(key):
					continue
				var index: int = int(address_index[key])
				var direction: Vector3 = Deltas.lattice_to_dir(
					address.x, float(address.y), float(address.z))
				var final_height: float = heights[index] + Deltas.offset_at_snapshot(direction, snap)
				var axis_weight_x: float = 2.0 if ox == 0 else 1.0
				var axis_weight_y: float = 2.0 if oy == 0 else 1.0
				var neighbor_weight: float = axis_weight_x * axis_weight_y
				weighted_sum += final_height * neighbor_weight
				weight_sum += neighbor_weight
		if weight_sum <= 0.0:
			continue
		var mean_height: float = weighted_sum / weight_sum
		var amount: float = clampf(strength * float(sample["weight"]), 0.0, 1.0)
		var desired_height: float = lerpf(current_height, mean_height, amount)
		var desired_delta: float = clampf(
			desired_height - pristine,
			SCULPT_MIN_OFFSET_M,
			SCULPT_MAX_OFFSET_M)
		if absf(desired_delta - before_delta) <= 1e-6:
			continue
		writes.append({"address": source, "value": desired_delta})
	return _apply_absolute_delta_writes(writes, job["center"], float(job["planet_radius"]))


func _apply_smooth_job_cpu(job: Dictionary) -> void:
	var addresses: Array = job["addresses"]
	var heights: PackedFloat32Array = _cpu_pristine_for_addresses(addresses)
	var started_us: int = Time.get_ticks_usec()
	var changed: int = _apply_smooth_job_with_pristine(job, heights)
	_record_cpu_shape_fallback("Smooth", addresses.size(), started_us)
	if changed > 0:
		_last_sculpt_dir = job["center"]
		_set_status("Smooth GPU request fell back to CPU: %d samples changed • %.3f ms finalize." % [
			changed, _gpu_last_finalize_ms])


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
	var center: Vector3 = job["center"]
	var planet_radius: float = float(job["planet_radius"])
	var spacing_m: float = maxf(Deltas.sample_spacing(planet_radius), 0.001)
	job["spacing_m"] = spacing_m
	job["delta_snapshot"] = Deltas.snapshot_for_bounds(
		center,
		(float(job["radius_m"]) + spacing_m * 5.0) / planet_radius)
	var prepared: Dictionary = _prepare_thermal_query_addresses(job)
	var directions: Array[Vector3] = prepared["directions"]
	job["addresses"] = prepared["addresses"]
	job["address_index"] = prepared["address_index"]
	job["dispatch_us"] = Time.get_ticks_usec()
	_gpu_thermal_active = job
	var serial: int = int(job["serial"])
	var callback: Callable = Callable(self, "_on_gpu_thermal_result").bind(serial)
	if _pristine_batch_query == null \
			or not bool(_pristine_batch_query.call("request", directions, callback)):
		var fallback_job: Dictionary = _gpu_thermal_active
		_gpu_thermal_active = {}
		_apply_thermal_job_cpu(fallback_job)
		_start_next_gpu_thermal_job()


func _prepare_thermal_query_addresses(job: Dictionary) -> Dictionary:
	var addresses: Array = []
	var address_index: Dictionary = {}
	var samples: Array = job["samples"]
	for sample_value: Variant in samples:
		var sample: Dictionary = sample_value as Dictionary
		var source: Vector3i = sample["address"]
		_append_unique_gpu_address(addresses, address_index, source)
		for offset: Vector2i in THERMAL_NEIGHBORS:
			var neighbor: Vector3i = Deltas.canonical_address(
				source.x, source.y + offset.x, source.z + offset.y)
			_append_unique_gpu_address(addresses, address_index, neighbor)
	var directions: Array[Vector3] = []
	directions.resize(addresses.size())
	for index: int in addresses.size():
		var address: Vector3i = addresses[index]
		directions[index] = Deltas.lattice_to_dir(
			address.x, float(address.y), float(address.z))
	return {"addresses": addresses, "address_index": address_index, "directions": directions}


func _on_gpu_thermal_result(success: bool, heights: PackedFloat32Array, serial: int) -> void:
	if _gpu_thermal_active.is_empty() or int(_gpu_thermal_active.get("serial", -1)) != serial:
		return
	var job: Dictionary = _gpu_thermal_active
	_gpu_thermal_active = {}
	if String(job["body_id"]) != _active_body_id():
		_start_next_gpu_thermal_job()
		return
	var addresses: Array = job["addresses"]
	if not _valid_gpu_heights(success, heights, addresses.size()):
		_apply_thermal_job_cpu(job)
		_start_next_gpu_thermal_job()
		return
	var finalize_started_us: int = Time.get_ticks_usec()
	var changed: int = _apply_thermal_job_with_pristine(job, heights)
	_record_gpu_shape_metrics("Thermal", job, heights.size(), finalize_started_us, false)
	if changed > 0:
		_last_sculpt_dir = job["center"]
		_set_status("Thermal erosion asynchronously: %d samples changed • GPU %.3f ms + finalize %.3f ms." % [
			changed, _gpu_last_latency_ms, _gpu_last_finalize_ms])
	_start_next_gpu_thermal_job()


func _apply_thermal_job_with_pristine(job: Dictionary,
		heights: PackedFloat32Array) -> int:
	var samples: Array = job["samples"]
	var address_index: Dictionary = job["address_index"]
	var snap: Dictionary = job["delta_snapshot"]
	var spacing_m: float = float(job["spacing_m"])
	var talus_drop_m: float = tan(deg_to_rad(clampf(float(job["talus_deg"]), 0.0, 89.0))) * spacing_m
	var strength: float = float(job["strength"])
	var changes: Dictionary = {}
	var changed_addresses: Dictionary = {}

	for sample_value: Variant in samples:
		var sample: Dictionary = sample_value as Dictionary
		var source: Vector3i = sample["address"]
		var source_key: String = _address_key(source)
		if not address_index.has(source_key):
			continue
		var source_index: int = int(address_index[source_key])
		var source_dir: Vector3 = Deltas.lattice_to_dir(
			source.x, float(source.y), float(source.z))
		var source_height: float = heights[source_index] + Deltas.offset_at_snapshot(source_dir, snap)
		var source_weight: float = clampf(float(sample["weight"]), 0.0, 1.0)
		if source_weight <= 0.0001:
			continue
		var downhill: Array[Dictionary] = []
		for offset: Vector2i in THERMAL_NEIGHBORS:
			var neighbor: Vector3i = Deltas.canonical_address(
				source.x, source.y + offset.x, source.z + offset.y)
			var neighbor_key: String = _address_key(neighbor)
			if neighbor.x < 0 or neighbor == source or not address_index.has(neighbor_key):
				continue
			var neighbor_index: int = int(address_index[neighbor_key])
			var neighbor_dir: Vector3 = Deltas.lattice_to_dir(
				neighbor.x, float(neighbor.y), float(neighbor.z))
			var neighbor_height: float = heights[neighbor_index] + Deltas.offset_at_snapshot(neighbor_dir, snap)
			var excess_m: float = source_height - neighbor_height - talus_drop_m
			if excess_m > 1e-5:
				downhill.append({"address": neighbor, "excess": excess_m})
		if downhill.is_empty():
			continue
		var relaxation: float = clampf(strength * source_weight, 0.0, 1.0)
		var denominator: float = float(downhill.size() + 1)
		for edge: Dictionary in downhill:
			var transfer_m: float = float(edge["excess"]) * relaxation / denominator
			if transfer_m <= 1e-7:
				continue
			var neighbor: Vector3i = edge["address"]
			_accumulate_thermal_change(changes, changed_addresses, source, -transfer_m)
			_accumulate_thermal_change(changes, changed_addresses, neighbor, transfer_m)

	var writes: Array[Dictionary] = []
	for key_value: Variant in changes.keys():
		var key: String = String(key_value)
		var address: Vector3i = changed_addresses[key]
		var change_m: float = float(changes[key])
		if absf(change_m) <= 1e-7:
			continue
		var direction: Vector3 = Deltas.lattice_to_dir(
			address.x, float(address.y), float(address.z))
		var before_delta: float = Deltas.offset_at_snapshot(direction, snap)
		var desired_delta: float = clampf(
			before_delta + change_m,
			SCULPT_MIN_OFFSET_M,
			SCULPT_MAX_OFFSET_M)
		if absf(desired_delta - before_delta) > 1e-7:
			writes.append({"address": address, "value": desired_delta})
	return _apply_absolute_delta_writes(writes, job["center"], float(job["planet_radius"]))


func _apply_thermal_job_cpu(job: Dictionary) -> void:
	var addresses: Array = job["addresses"]
	var heights: PackedFloat32Array = _cpu_pristine_for_addresses(addresses)
	var started_us: int = Time.get_ticks_usec()
	var changed: int = _apply_thermal_job_with_pristine(job, heights)
	_record_cpu_shape_fallback("Thermal", addresses.size(), started_us)
	if changed > 0:
		_last_sculpt_dir = job["center"]
		_set_status("Thermal GPU request fell back to CPU: %d samples changed • %.3f ms finalize." % [
			changed, _gpu_last_finalize_ms])


func _append_unique_gpu_address(addresses: Array, address_index: Dictionary,
		address: Vector3i) -> void:
	if address.x < 0:
		return
	var key: String = _address_key(address)
	if address_index.has(key):
		return
	address_index[key] = addresses.size()
	addresses.append(address)


func _cpu_pristine_for_addresses(addresses: Array) -> PackedFloat32Array:
	var heights := PackedFloat32Array()
	heights.resize(addresses.size())
	for index: int in addresses.size():
		var address: Vector3i = addresses[index]
		var direction: Vector3 = Deltas.lattice_to_dir(
			address.x, float(address.y), float(address.z))
		heights[index] = _generated_pristine_height(direction)
	return heights


func _valid_gpu_heights(success: bool, heights: PackedFloat32Array,
		expected_count: int) -> bool:
	if not success or heights.size() != expected_count:
		return false
	for height: float in heights:
		if not is_finite(height):
			return false
	return true


func _record_gpu_shape_metrics(tool_name: String, job: Dictionary, count: int,
		finalize_started_us: int, fallback: bool) -> void:
	_gpu_last_shape_tool = tool_name
	_gpu_last_query_count = count
	_gpu_last_latency_ms = float(maxi(0, finalize_started_us - int(job["dispatch_us"]))) / 1000.0
	_gpu_last_finalize_ms = float(maxi(0, Time.get_ticks_usec() - finalize_started_us)) / 1000.0
	_gpu_last_fallback = fallback


func _record_cpu_shape_fallback(tool_name: String, count: int,
		finalize_started_us: int) -> void:
	_gpu_last_shape_tool = tool_name
	_gpu_last_query_count = count
	_gpu_last_latency_ms = 0.0
	_gpu_last_finalize_ms = float(maxi(0, Time.get_ticks_usec() - finalize_started_us)) / 1000.0
	_gpu_last_fallback = true


func _gpu_query_busy() -> bool:
	return _pristine_batch_query != null and bool(_pristine_batch_query.call("is_busy"))


func _has_pending_gpu_sculpt() -> bool:
	return super._has_pending_gpu_sculpt() \
		or not _gpu_smooth_active.is_empty() \
		or not _gpu_smooth_queue.is_empty() \
		or not _gpu_thermal_active.is_empty() \
		or not _gpu_thermal_queue.is_empty()


func _finish_deferred_gpu_commit_if_ready() -> void:
	# If another queue owns the one-shot service, let its completion callback kick
	# this dispatcher again. This keeps all stamp evaluation strictly sequential.
	if _gpu_query_busy():
		return
	if _gpu_flatten_active.is_empty() and not _gpu_flatten_queue.is_empty():
		_start_next_gpu_flatten_job()
		return
	if _gpu_smooth_active.is_empty() and not _gpu_smooth_queue.is_empty():
		_start_next_gpu_smooth_job()
		return
	if _gpu_thermal_active.is_empty() and not _gpu_thermal_queue.is_empty():
		_start_next_gpu_thermal_job()
		return
	super._finish_deferred_gpu_commit_if_ready()


func _discard_interactive_transactions() -> void:
	_gpu_smooth_queue.clear()
	_gpu_smooth_active.clear()
	_gpu_thermal_queue.clear()
	_gpu_thermal_active.clear()
	super._discard_interactive_transactions()
