class_name WorldAuthoringLiveEditorPhase7
extends "res://scripts/world_authoring/world_authoring_editor_live_phase6.gd"
## Phase 7 terrain shaping: smooth and flatten-to-height.
##
## Both tools continue to write only the authoritative sparse Deltas lattice. A
## complete stamp first snapshots the current edit layer, evaluates pristine
## generated terrain independently, computes every destination offset from that
## immutable source, and only then writes results. This makes smoothing
## order-independent and keeps flattening a final-height operation rather than a
## misleading "make all delta values equal" operation.
##
## The synchronous pristine sampler is the deterministic generated terrain
## envelope. Resident GPU contact samples remain the source for the picked flatten
## target itself; high-frequency visual detail is therefore preserved rather than
## destructively baked into the saved edit layer.

const SCULPT_SMOOTH_MODE: int = 7
const SCULPT_FLATTEN_MODE: int = 8
const SCULPT_MIN_OFFSET_M: float = -10000.0
const SCULPT_MAX_OFFSET_M: float = 10000.0
const SMOOTH_MAX_NEIGHBOR_RADIUS_M: float = 4.0

var _smooth_strength: float = 0.34
var _flatten_strength: float = 0.52
var _flatten_target_height_m: float = 0.0
var _flatten_target_valid: bool = false


func _build_terrain_page() -> void:
	super._build_terrain_page()
	if _world_host == null:
		return
	_section("Shape terrain")
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 7)
	_workspace.add_child(row)
	var smooth_button := Button.new()
	smooth_button.text = "STOP SMOOTH" if _placement_mode == SCULPT_SMOOTH_MODE else "SMOOTH"
	smooth_button.tooltip_text = "Relax final generated+authored height toward a local neighborhood mean."
	smooth_button.pressed.connect(func() -> void:
		_set_placement_mode(PlacementMode.NONE if _placement_mode == SCULPT_SMOOTH_MODE else SCULPT_SMOOTH_MODE)
		_refresh_current_category()
	)
	row.add_child(smooth_button)
	var flatten_button := Button.new()
	flatten_button.text = "STOP FLATTEN" if _placement_mode == SCULPT_FLATTEN_MODE else "FLATTEN"
	flatten_button.tooltip_text = "The first LMB hit of each drag captures a fixed final terrain height; the rest of that drag converges toward it."
	flatten_button.pressed.connect(func() -> void:
		_set_placement_mode(PlacementMode.NONE if _placement_mode == SCULPT_FLATTEN_MODE else SCULPT_FLATTEN_MODE)
		_refresh_current_category()
	)
	row.add_child(flatten_button)
	var target_label := Label.new()
	target_label.text = ("target %.2f m MSL" % _flatten_target_height_m) if _flatten_target_valid else "flatten target: first click"
	target_label.modulate = Color(0.64, 0.76, 0.86)
	row.add_child(target_label)
	_add_number_field("Smooth strength", _smooth_strength, 0.01, 1.0, 0.01, "", func(value: float) -> void:
		_smooth_strength = clampf(value, 0.01, 1.0)
	)
	_add_number_field("Flatten strength", _flatten_strength, 0.01, 1.0, 0.01, "", func(value: float) -> void:
		_flatten_strength = clampf(value, 0.01, 1.0)
	)
	_add_note("SMOOTH and FLATTEN use the same radius/hardness as Raise/Lower. Each stamp reads an immutable sparse-delta snapshot before writing, so results do not depend on lattice traversal order. FLATTEN captures one MSL target from the first click and keeps it for the full drag.")
	_add_note("These tools reshape the persistent generated terrain envelope without baking transient deformation or procedural high-frequency visual detail into saves. Rendering/contact still consume the resulting Deltas through the authoritative terrain stack.")


func _is_sculpt_placement() -> bool:
	return super._is_sculpt_placement() \
		or _placement_mode == SCULPT_SMOOTH_MODE \
		or _placement_mode == SCULPT_FLATTEN_MODE


func _continuous_drag_mode() -> bool:
	return super._continuous_drag_mode() \
		or _placement_mode == SCULPT_SMOOTH_MODE \
		or _placement_mode == SCULPT_FLATTEN_MODE


func _placement_status_text() -> String:
	if _placement_mode == SCULPT_SMOOTH_MODE:
		return "SCULPT SMOOTH — LMB drag • wheel radius • RMB/Esc stop • TAB navigate"
	if _placement_mode == SCULPT_FLATTEN_MODE:
		var target := "first click picks target" if not _flatten_target_valid else "target %.2f m MSL" % _flatten_target_height_m
		return "SCULPT FLATTEN — %s • LMB drag • wheel radius • RMB/Esc stop" % target
	return super._placement_status_text()


func _set_placement_mode(mode: int) -> void:
	if _placement_mode == SCULPT_FLATTEN_MODE and mode != SCULPT_FLATTEN_MODE:
		_flatten_target_valid = false
	super._set_placement_mode(mode)


func _commit_sculpt_transaction() -> void:
	var reset_flatten_target: bool = _placement_mode == SCULPT_FLATTEN_MODE
	super._commit_sculpt_transaction()
	if reset_flatten_target:
		_flatten_target_valid = false
		_update_preview()


func _discard_interactive_transactions() -> void:
	_flatten_target_valid = false
	super._discard_interactive_transactions()


func _place_current_hit(continuous: bool) -> void:
	if _placement_mode != SCULPT_SMOOTH_MODE and _placement_mode != SCULPT_FLATTEN_MODE:
		super._place_current_hit(continuous)
		return
	if _last_hit.is_empty():
		_set_status("Viewport pick did not intersect terrain.")
		return
	var direction: Vector3 = _last_hit.get("dir", Vector3.ZERO)
	if direction.length_squared() < 0.99:
		return
	if _placement_mode == SCULPT_SMOOTH_MODE:
		_place_smooth_stroke(direction, continuous)
	else:
		if not _flatten_target_valid:
			_flatten_target_height_m = float(_last_hit.get("height", 0.0))
			_flatten_target_valid = true
		_place_flatten_stroke(direction, continuous)
	_update_preview()


func _place_smooth_stroke(direction: Vector3, continuous: bool) -> void:
	if Planet.cfg == null or not Planet.ready_state:
		return
	var planet_radius: float = maxf(float(Planet.cfg.planet_radius), 1.0)
	if _skip_redundant_sculpt_stamp(direction, continuous, planet_radius):
		return
	var changed: int = _smooth_final_height_brush(direction, planet_radius)
	if changed <= 0:
		return
	_last_sculpt_dir = direction
	_set_status("Smoothed terrain: %d samples • %.1f m radius • %.0f%% strength." % [changed, _sculpt_radius_m, _smooth_strength * 100.0])


func _place_flatten_stroke(direction: Vector3, continuous: bool) -> void:
	if Planet.cfg == null or not Planet.ready_state or not _flatten_target_valid:
		return
	var planet_radius: float = maxf(float(Planet.cfg.planet_radius), 1.0)
	if _skip_redundant_sculpt_stamp(direction, continuous, planet_radius):
		return
	var changed: int = _flatten_final_height_brush(direction, planet_radius, _flatten_target_height_m)
	if changed <= 0:
		return
	_last_sculpt_dir = direction
	_set_status("Flattened terrain: %d samples • target %.2f m MSL • %.0f%% strength." % [changed, _flatten_target_height_m, _flatten_strength * 100.0])


func _skip_redundant_sculpt_stamp(direction: Vector3, continuous: bool, planet_radius: float) -> bool:
	if not continuous or _last_sculpt_dir.length_squared() <= 0.99:
		return false
	var arc_distance: float = acos(clampf(_last_sculpt_dir.dot(direction), -1.0, 1.0)) * planet_radius
	return arc_distance < maxf(_sculpt_radius_m * 0.16, Deltas.sample_spacing(planet_radius) * 1.5)


func _flatten_final_height_brush(center_dir: Vector3, planet_radius: float, target_height_m: float) -> int:
	var samples: Array[Dictionary] = _collect_sculpt_samples(center_dir, planet_radius)
	if samples.is_empty():
		return 0
	var spacing_m: float = maxf(Deltas.sample_spacing(planet_radius), 0.001)
	var angular_radius: float = (_sculpt_radius_m + spacing_m * 3.0) / planet_radius
	var snap: Dictionary = Deltas.snapshot_for_bounds(center_dir.normalized(), angular_radius)
	var writes: Array[Dictionary] = []
	for sample: Dictionary in samples:
		var sample_dir: Vector3 = sample["dir"]
		var weight: float = float(sample["weight"])
		var pristine: float = _generated_pristine_height(sample_dir)
		var before_delta: float = Deltas.offset_at_snapshot(sample_dir, snap)
		var current_height: float = pristine + before_delta
		var amount: float = clampf(_flatten_strength * weight, 0.0, 1.0)
		var desired_height: float = lerpf(current_height, target_height_m, amount)
		var desired_delta: float = clampf(desired_height - pristine, SCULPT_MIN_OFFSET_M, SCULPT_MAX_OFFSET_M)
		if absf(desired_delta - before_delta) <= 1e-6:
			continue
		writes.append({"address": sample["address"], "value": desired_delta})
	return _apply_absolute_delta_writes(writes, center_dir, planet_radius)


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
		var pristine: float = _generated_pristine_height(sample_dir)
		var current_height: float = pristine + before_delta
		var mean_height: float = _neighbor_mean_height(address, neighbor_step, snap, height_cache)
		var amount: float = clampf(_smooth_strength * float(sample["weight"]), 0.0, 1.0)
		var desired_height: float = lerpf(current_height, mean_height, amount)
		var desired_delta: float = clampf(desired_height - pristine, SCULPT_MIN_OFFSET_M, SCULPT_MAX_OFFSET_M)
		if absf(desired_delta - before_delta) <= 1e-6:
			continue
		writes.append({"address": address, "value": desired_delta})
	return _apply_absolute_delta_writes(writes, center_dir, planet_radius)


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
			var weight: float = 1.0
			if normalized_distance > hard:
				var edge_t: float = (normalized_distance - hard) / maxf(1.0 - hard, 0.001)
				weight = 1.0 - smoothstep(0.0, 1.0, edge_t)
			if weight <= 0.0001:
				continue
			out.append({"address": address, "dir": sample_dir, "weight": weight})
	return out


func _neighbor_mean_height(center_address: Vector3i, step: int, snap: Dictionary, cache: Dictionary) -> float:
	var weighted_sum: float = 0.0
	var weight_sum: float = 0.0
	for oy: int in [-step, 0, step]:
		for ox: int in [-step, 0, step]:
			var address: Vector3i = Deltas.canonical_address(center_address.x, center_address.y + ox, center_address.z + oy)
			if address.x < 0:
				continue
			var axis_weight_x: float = 2.0 if ox == 0 else 1.0
			var axis_weight_y: float = 2.0 if oy == 0 else 1.0
			var weight: float = axis_weight_x * axis_weight_y
			weighted_sum += _snapshot_final_height(address, snap, cache) * weight
			weight_sum += weight
	if weight_sum <= 0.0:
		return _snapshot_final_height(center_address, snap, cache)
	return weighted_sum / weight_sum


func _snapshot_final_height(address: Vector3i, snap: Dictionary, cache: Dictionary) -> float:
	var key: String = _address_key(address)
	if cache.has(key):
		return float(cache[key])
	var direction: Vector3 = Deltas.lattice_to_dir(address.x, float(address.y), float(address.z))
	var height: float = _generated_pristine_height(direction) + Deltas.offset_at_snapshot(direction, snap)
	cache[key] = height
	return height


func _generated_pristine_height(direction: Vector3) -> float:
	if not Planet.ready_state or Planet.cfg == null:
		return 0.0
	return float(Planet.pristine_height(direction.normalized()))


func _apply_absolute_delta_writes(writes: Array[Dictionary], center_dir: Vector3, planet_radius: float) -> int:
	var changed: int = 0
	for write: Dictionary in writes:
		var address: Vector3i = write["address"]
		var desired: float = float(write["value"])
		var current: float = Deltas.get_offset(address.x, address.y, address.z)
		var applied: float = Deltas.add_offset(
			address.x, address.y, address.z,
			desired - current,
			SCULPT_MIN_OFFSET_M,
			SCULPT_MAX_OFFSET_M)
		if absf(applied) > 1e-7:
			changed += 1
	if changed > 0:
		var spacing_m: float = maxf(Deltas.sample_spacing(planet_radius), 0.001)
		Deltas.notify_changed(center_dir.normalized(), _sculpt_radius_m + spacing_m * 4.0)
	return changed


func _address_key(address: Vector3i) -> String:
	return "%d:%d:%d" % [address.x, address.y, address.z]


func _update_preview() -> void:
	if _placement_mode != SCULPT_SMOOTH_MODE and _placement_mode != SCULPT_FLATTEN_MODE:
		super._update_preview()
		return
	if _preview_mesh == null:
		return
	_preview_mesh.clear_surfaces()
	if _navigation_active:
		return
	if not _last_hit.is_empty():
		var direction: Vector3 = _last_hit.get("dir", Vector3.ZERO)
		var height: float = float(_last_hit.get("height", 0.0))
		var color := Color(0.28, 0.78, 1.0, 1.0) if _placement_mode == SCULPT_SMOOTH_MODE else Color(0.86, 0.74, 0.24, 1.0)
		_draw_surface_ring(direction, height, _sculpt_radius_m, color)
		if _sculpt_hardness > 0.02:
			_draw_surface_ring(direction, height, maxf(0.1, _sculpt_radius_m * _sculpt_hardness), Color(color.r, color.g, color.b, 0.55))
	_draw_selected_water_feature()
