extends "res://scripts/world_authoring/world_authoring_editor_live_phase14.gd"
## Phase 15: packed sparse target publication.
##
## Internal inheritance layer: intentionally loaded by resource path instead of a
## global `class_name`. This avoids stale Godot global-script-class cache entries
## resolving the moving Planet Studio phase chain as a cyclic reference.
##
## Phase 14 removed String/trigonometry overhead from candidate collection. This
## layer removes the second large Array[Dictionary] previously allocated while
## turning brush samples into absolute sparse-delta targets. High-volume
## Raise/Lower/Erase and asynchronous Flatten/Smooth/Thermal finalizers now append
## one int64 canonical address and one float target per changed sample, then hand
## those packed arrays to Deltas under a single mutex acquisition.

var _phase15_last_packed_targets: int = 0
var _phase15_last_pack_ms: float = 0.0


func _build_terrain_page() -> void:
	super._build_terrain_page()
	if _world_host == null:
		return
	_section("Packed sculpt publication")
	var label := Label.new()
	label.modulate = Color(0.64, 0.76, 0.86)
	if _phase15_last_packed_targets <= 0:
		label.text = "No packed target batch measured yet."
	else:
		label.text = "%d packed targets • %.3f ms target publication" % [
			_phase15_last_packed_targets, _phase15_last_pack_ms]
	_workspace.add_child(label)
	_add_note("Changed lattice targets are published as PackedInt64Array addresses + PackedFloat32Array values. This removes the large second write-Dictionary array from normal sculpting and GPU readback finalization.")


func _place_sculpt_stroke(direction: Vector3, continuous: bool, sign_value: float) -> void:
	if Planet.cfg == null or not Planet.ready_state:
		return
	var effective_sign: float = -sign_value if Input.is_key_pressed(KEY_SHIFT) else sign_value
	var planet_radius: float = maxf(float(Planet.cfg.planet_radius), 1.0)
	if _skip_redundant_sculpt_stamp(direction, continuous, planet_radius):
		return
	var samples: Array[Dictionary] = _collect_sculpt_samples(direction, planet_radius)
	if samples.is_empty():
		return
	var spacing_m: float = maxf(Deltas.sample_spacing(planet_radius), 0.001)
	var snap: Dictionary = Deltas.snapshot_for_bounds(
		direction.normalized(),
		(_sculpt_radius_m + spacing_m * 3.0) / planet_radius)
	var addresses := PackedInt64Array()
	var values := PackedFloat32Array()
	for sample: Dictionary in samples:
		var sample_dir: Vector3 = sample["dir"]
		var before: float = Deltas.offset_at_snapshot(sample_dir, snap)
		var desired: float = clampf(
			before + _sculpt_strength_m * effective_sign * float(sample["weight"]),
			SCULPT_MIN_OFFSET_M,
			SCULPT_MAX_OFFSET_M)
		if absf(desired - before) <= 1e-7:
			continue
		var packed: int = Deltas.pack_address(sample["address"])
		if packed < 0:
			continue
		addresses.append(packed)
		values.append(desired)
	var changed: int = _apply_packed_absolute_delta_writes(
		addresses, values, direction, planet_radius, _sculpt_radius_m)
	if changed <= 0:
		return
	_last_sculpt_dir = direction
	var verb: String = "Raised" if effective_sign > 0.0 else "Lowered"
	_set_status("%s terrain: %d samples • %.1f m radius • %.2f m stamp • %s falloff • packed targets." % [
		verb, changed, _sculpt_radius_m, _sculpt_strength_m,
		FALLOFF_NAMES[_sculpt_falloff_profile]])


func _place_erase_stroke(direction: Vector3, continuous: bool) -> void:
	if Planet.cfg == null or not Planet.ready_state or Deltas.is_empty():
		return
	var planet_radius: float = maxf(float(Planet.cfg.planet_radius), 1.0)
	if _skip_redundant_sculpt_stamp(direction, continuous, planet_radius):
		return
	var samples: Array[Dictionary] = _collect_sculpt_samples(direction, planet_radius)
	if samples.is_empty():
		return
	var spacing_m: float = maxf(Deltas.sample_spacing(planet_radius), 0.001)
	var snap: Dictionary = Deltas.snapshot_for_bounds(
		direction.normalized(),
		(_sculpt_radius_m + spacing_m * 3.0) / planet_radius)
	var addresses := PackedInt64Array()
	var values := PackedFloat32Array()
	for sample: Dictionary in samples:
		var sample_dir: Vector3 = sample["dir"]
		var before: float = Deltas.offset_at_snapshot(sample_dir, snap)
		if absf(before) <= 1e-7:
			continue
		var amount: float = clampf(_erase_strength * float(sample["weight"]), 0.0, 1.0)
		var desired: float = lerpf(before, 0.0, amount)
		if absf(desired) < 1e-5:
			desired = 0.0
		if absf(desired - before) <= 1e-7:
			continue
		var packed: int = Deltas.pack_address(sample["address"])
		if packed < 0:
			continue
		addresses.append(packed)
		values.append(desired)
	var changed: int = _apply_packed_absolute_delta_writes(
		addresses, values, direction, planet_radius, _sculpt_radius_m)
	if changed <= 0:
		return
	_last_sculpt_dir = direction
	_set_status("Erased terrain edits: %d samples • %.1f m radius • %.0f%% • %s falloff • packed targets." % [
		changed, _sculpt_radius_m, _erase_strength * 100.0,
		FALLOFF_NAMES[_sculpt_falloff_profile]])


## Compatibility funnel for older synchronous shaping helpers. They may still
## build write dictionaries, but the actual sparse-store publication uses the same
## packed API. Async Phase-15 finalizers below avoid those dictionaries entirely.
func _apply_absolute_delta_writes(writes: Array[Dictionary], center_dir: Vector3,
		planet_radius: float) -> int:
	if writes.is_empty():
		return 0
	var addresses := PackedInt64Array()
	var values := PackedFloat32Array()
	for write: Dictionary in writes:
		var raw_address: Variant = write.get("address")
		if not (raw_address is Vector3i):
			continue
		var packed: int = Deltas.pack_address(raw_address as Vector3i)
		if packed < 0:
			continue
		addresses.append(packed)
		values.append(float(write.get("value", 0.0)))
	return _apply_packed_absolute_delta_writes(
		addresses, values, center_dir, planet_radius, _sculpt_radius_m)


func _apply_flatten_job_with_pristine(job: Dictionary,
		heights: PackedFloat32Array) -> int:
	var samples: Array = job["samples"]
	var snap: Dictionary = job["delta_snapshot"]
	var target_height_m: float = float(job["target_height_m"])
	var strength: float = float(job["strength"])
	var addresses := PackedInt64Array()
	var values := PackedFloat32Array()
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
		var packed: int = Deltas.pack_address(sample["address"])
		if packed < 0:
			continue
		addresses.append(packed)
		values.append(desired_delta)
	return _apply_packed_absolute_delta_writes(
		addresses,
		values,
		job["center"],
		float(job["planet_radius"]),
		float(job.get("radius_m", _sculpt_radius_m)))


func _apply_smooth_job_with_pristine(job: Dictionary,
		heights: PackedFloat32Array) -> int:
	var samples: Array = job["samples"]
	var address_index: Dictionary = job["address_index"]
	var snap: Dictionary = job["delta_snapshot"]
	var step: int = int(job["neighbor_step"])
	var strength: float = float(job["strength"])
	var addresses := PackedInt64Array()
	var values := PackedFloat32Array()
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
				var neighbor_index: int = int(address_index[key])
				var direction: Vector3 = Deltas.lattice_to_dir(
					address.x, float(address.y), float(address.z))
				var final_height: float = heights[neighbor_index] + Deltas.offset_at_snapshot(direction, snap)
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
		var packed: int = Deltas.pack_address(source)
		if packed < 0:
			continue
		addresses.append(packed)
		values.append(desired_delta)
	return _apply_packed_absolute_delta_writes(
		addresses,
		values,
		job["center"],
		float(job["planet_radius"]),
		float(job.get("radius_m", _sculpt_radius_m)))


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
		var downhill_addresses: Array[Vector3i] = []
		var downhill_excess := PackedFloat32Array()
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
				downhill_addresses.append(neighbor)
				downhill_excess.append(excess_m)
		if downhill_addresses.is_empty():
			continue
		var relaxation: float = clampf(strength * source_weight, 0.0, 1.0)
		var denominator: float = float(downhill_addresses.size() + 1)
		for edge_index: int in downhill_addresses.size():
			var transfer_m: float = downhill_excess[edge_index] * relaxation / denominator
			if transfer_m <= 1e-7:
				continue
			var neighbor: Vector3i = downhill_addresses[edge_index]
			_accumulate_thermal_change(changes, changed_addresses, source, -transfer_m)
			_accumulate_thermal_change(changes, changed_addresses, neighbor, transfer_m)

	var addresses := PackedInt64Array()
	var values := PackedFloat32Array()
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
		if absf(desired_delta - before_delta) <= 1e-7:
			continue
		var packed: int = Deltas.pack_address(address)
		if packed < 0:
			continue
		addresses.append(packed)
		values.append(desired_delta)
	return _apply_packed_absolute_delta_writes(
		addresses,
		values,
		job["center"],
		float(job["planet_radius"]),
		float(job.get("radius_m", _sculpt_radius_m)))


func _apply_packed_absolute_delta_writes(addresses: PackedInt64Array,
		values: PackedFloat32Array, center_dir: Vector3, planet_radius: float,
		dirty_radius_m: float) -> int:
	if addresses.is_empty() or values.is_empty():
		_phase15_last_packed_targets = 0
		_phase15_last_pack_ms = 0.0
		return 0
	var started_us: int = Time.get_ticks_usec()
	var changed: int = Deltas.set_packed_offsets_batch(
		addresses, values, SCULPT_MIN_OFFSET_M, SCULPT_MAX_OFFSET_M)
	_phase15_last_packed_targets = mini(addresses.size(), values.size())
	_phase15_last_pack_ms = float(maxi(0, Time.get_ticks_usec() - started_us)) / 1000.0
	if changed > 0:
		var spacing_m: float = maxf(Deltas.sample_spacing(planet_radius), 0.001)
		Deltas.notify_changed(
			center_dir.normalized(),
			maxf(0.0, dirty_radius_m) + spacing_m * 4.0)
	return changed
