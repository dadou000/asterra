class_name WorldAuthoringLiveEditorPhase9
extends "res://scripts/world_authoring/world_authoring_editor_live_phase8.gd"
## Phase 9: shared sculpt falloff profiles.
##
## Hardness remains the radius of the full-strength inner core. Outside that core
## every persistent sculpt mode uses the same selectable edge profile, so Raise,
## Lower, Erase, Smooth and Flatten no longer have subtly different brush shapes.
## Relative Raise/Lower and Erase are expressed as absolute targets from an
## immutable Deltas snapshot and published through Phase 8's one-lock batch path.

enum SculptFalloff {
	SMOOTH,
	LINEAR,
	COSINE,
	GAUSSIAN,
}

const FALLOFF_NAMES: Array[String] = ["Smooth", "Linear", "Cosine", "Gaussian"]
const GAUSSIAN_EDGE_K: float = 4.5

var _sculpt_falloff_profile: int = SculptFalloff.SMOOTH


func _build_terrain_page() -> void:
	super._build_terrain_page()
	if _world_host == null:
		return
	_section("Brush profile")
	var picker := OptionButton.new()
	picker.custom_minimum_size.x = 220.0
	for index: int in FALLOFF_NAMES.size():
		picker.add_item(FALLOFF_NAMES[index])
		picker.set_item_metadata(index, index)
	picker.select(clampi(_sculpt_falloff_profile, 0, FALLOFF_NAMES.size() - 1))
	picker.item_selected.connect(func(index: int) -> void:
		_sculpt_falloff_profile = int(picker.get_item_metadata(index))
		_set_status("Sculpt falloff: %s • hardness %.2f." % [FALLOFF_NAMES[_sculpt_falloff_profile], _sculpt_hardness])
		_update_preview()
	)
	_add_control_row("Edge falloff", picker)
	_add_note("Hardness defines the full-strength core. Smooth is the previous cubic edge; Linear gives a constant gradient; Cosine has zero slope at both ends; Gaussian concentrates influence near the core while still reaching exactly zero at the brush edge. The profile is shared by every persistent terrain sculpt mode.")


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
	var writes: Array[Dictionary] = []
	for sample: Dictionary in samples:
		var sample_dir: Vector3 = sample["dir"]
		var before: float = Deltas.offset_at_snapshot(sample_dir, snap)
		var desired: float = clampf(
			before + _sculpt_strength_m * effective_sign * float(sample["weight"]),
			SCULPT_MIN_OFFSET_M,
			SCULPT_MAX_OFFSET_M)
		if absf(desired - before) > 1e-7:
			writes.append({"address": sample["address"], "value": desired})
	var changed: int = _apply_absolute_delta_writes(writes, direction, planet_radius)
	if changed <= 0:
		return
	_last_sculpt_dir = direction
	var verb: String = "Raised" if effective_sign > 0.0 else "Lowered"
	_set_status("%s terrain: %d samples • %.1f m radius • %.2f m stamp • %s falloff." % [
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
	var writes: Array[Dictionary] = []
	for sample: Dictionary in samples:
		var sample_dir: Vector3 = sample["dir"]
		var before: float = Deltas.offset_at_snapshot(sample_dir, snap)
		if absf(before) <= 1e-7:
			continue
		var amount: float = clampf(_erase_strength * float(sample["weight"]), 0.0, 1.0)
		var desired: float = lerpf(before, 0.0, amount)
		if absf(desired) < 1e-5:
			desired = 0.0
		writes.append({"address": sample["address"], "value": desired})
	var changed: int = _apply_absolute_delta_writes(writes, direction, planet_radius)
	if changed <= 0:
		return
	_last_sculpt_dir = direction
	_set_status("Erased terrain edits: %d samples • %.1f m radius • %.0f%% • %s falloff." % [
		changed, _sculpt_radius_m, _erase_strength * 100.0,
		FALLOFF_NAMES[_sculpt_falloff_profile]])


func _collect_sculpt_samples(center_dir: Vector3, planet_radius: float) -> Array[Dictionary]:
	var out: Array[Dictionary] = []
	if center_dir.length_squared() < 0.5 or _sculpt_radius_m <= 0.0:
		return out
	var center: Vector3 = center_dir.normalized()
	var spacing_m: float = maxf(Deltas.sample_spacing(planet_radius), 0.001)
	var center_lattice: Array = Deltas.dir_to_lattice(center)
	var face: int = int(center_lattice[0])
	var center_i: int = int(round(float(center_lattice[1])))
	var center_j: int = int(round(float(center_lattice[2])))
	var extent: int = maxi(1, int(ceil(_sculpt_radius_m / spacing_m)) + 2)
	var hard: float = clampf(_sculpt_hardness, 0.0, 0.98)
	var visited: Dictionary = {}
	for source_j: int in range(center_j - extent, center_j + extent + 1):
		for source_i: int in range(center_i - extent, center_i + extent + 1):
			var address: Vector3i = Deltas.canonical_address(face, source_i, source_j)
			if address.x < 0:
				continue
			var key: String = _address_key(address)
			if visited.has(key):
				continue
			visited[key] = true
			var sample_dir: Vector3 = Deltas.lattice_to_dir(address.x, float(address.y), float(address.z))
			var distance_m: float = acos(clampf(center.dot(sample_dir), -1.0, 1.0)) * planet_radius
			if distance_m > _sculpt_radius_m:
				continue
			var normalized_distance: float = distance_m / maxf(_sculpt_radius_m, 0.001)
			var weight: float = _sculpt_profile_weight(normalized_distance, hard)
			if weight <= 0.0001:
				continue
			out.append({"address": address, "dir": sample_dir, "weight": weight})
	return out


func _sculpt_profile_weight(normalized_distance: float, hardness: float) -> float:
	var d: float = clampf(normalized_distance, 0.0, 1.0)
	var hard: float = clampf(hardness, 0.0, 0.98)
	if d <= hard:
		return 1.0
	if d >= 1.0:
		return 0.0
	var t: float = clampf((d - hard) / maxf(1.0 - hard, 0.001), 0.0, 1.0)
	match _sculpt_falloff_profile:
		SculptFalloff.LINEAR:
			return 1.0 - t
		SculptFalloff.COSINE:
			return 0.5 + 0.5 * cos(PI * t)
		SculptFalloff.GAUSSIAN:
			var edge_value: float = exp(-GAUSSIAN_EDGE_K)
			return clampf((exp(-GAUSSIAN_EDGE_K * t * t) - edge_value) /
				maxf(1.0 - edge_value, 1e-6), 0.0, 1.0)
		_:
			return 1.0 - smoothstep(0.0, 1.0, t)
