class_name WorldAuthoringLiveEditorPhase11
extends "res://scripts/world_authoring/world_authoring_editor_live_phase10.gd"
## Phase 11: conservative thermal erosion / talus relaxation.
##
## One interactive stamp performs one explicit relaxation pass from an immutable
## final-height snapshot. Cells steeper than the selected talus angle transfer
## material to lower cardinal neighbors. Every source subtraction is accumulated
## with an equal recipient deposition before any sparse write occurs, making the
## pass traversal-order independent and locally height-volume conserving on the
## fixed edit lattice (except if the global safety offset clamp is reached).

const SCULPT_THERMAL_MODE: int = 9
const THERMAL_NEIGHBORS: Array[Vector2i] = [
	Vector2i(1, 0), Vector2i(-1, 0), Vector2i(0, 1), Vector2i(0, -1),
]

var _thermal_talus_deg: float = 34.0
var _thermal_strength: float = 0.30


func _build_terrain_page() -> void:
	super._build_terrain_page()
	if _world_host == null:
		return
	_section("Thermal erosion")
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 7)
	_workspace.add_child(row)
	var button := Button.new()
	button.text = "STOP THERMAL" if _placement_mode == SCULPT_THERMAL_MODE else "THERMAL ERODE"
	button.tooltip_text = "Relax slopes above the talus angle while depositing the removed material downhill."
	button.pressed.connect(func() -> void:
		_set_placement_mode(PlacementMode.NONE if _placement_mode == SCULPT_THERMAL_MODE else SCULPT_THERMAL_MODE)
		_refresh_current_category()
	)
	row.add_child(button)
	var info := Label.new()
	info.text = "one conservative relaxation pass per stamp"
	info.modulate = Color(0.64, 0.76, 0.86)
	row.add_child(info)
	_add_number_field("Talus angle", _thermal_talus_deg, 0.0, 89.0, 0.1, "°", func(value: float) -> void:
		_thermal_talus_deg = clampf(value, 0.0, 89.0)
	)
	_add_number_field("Thermal strength", _thermal_strength, 0.01, 1.0, 0.01, "", func(value: float) -> void:
		_thermal_strength = clampf(value, 0.01, 1.0)
	)
	_add_note("Only slopes steeper than the talus angle move. Material removed from each source sample is deposited into its lower cardinal neighbors in the same immutable-snapshot pass. The active hardness/falloff weights the source transfer, and repeated drag stamps naturally iterate the relaxation.")
	_add_note("Mouse wheel changes radius; Shift+wheel changes thermal strength. The stamp remains part of the same sparse Deltas layer, so rendering, collision/contact, presets and one-drag/one-Undo history stay authoritative.")


func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventMouseButton and _placement_mode == SCULPT_THERMAL_MODE:
		var wheel := event as InputEventMouseButton
		if wheel.pressed and wheel.shift_pressed and _is_live_viewport_point(wheel.position) \
				and (wheel.button_index == MOUSE_BUTTON_WHEEL_UP or wheel.button_index == MOUSE_BUTTON_WHEEL_DOWN):
			var scale: float = 1.12 if wheel.button_index == MOUSE_BUTTON_WHEEL_UP else 1.0 / 1.12
			_thermal_strength = clampf(_thermal_strength * scale, 0.01, 1.0)
			_set_status("Thermal strength %.3f." % _thermal_strength)
			get_viewport().set_input_as_handled()
			return
	super._unhandled_input(event)


func _is_sculpt_placement() -> bool:
	return super._is_sculpt_placement() or _placement_mode == SCULPT_THERMAL_MODE


func _continuous_drag_mode() -> bool:
	return super._continuous_drag_mode() or _placement_mode == SCULPT_THERMAL_MODE


func _placement_status_text() -> String:
	if _placement_mode == SCULPT_THERMAL_MODE:
		return "THERMAL EROSION — LMB drag • wheel radius • Shift+wheel strength • RMB/Esc stop"
	return super._placement_status_text()


func _place_current_hit(continuous: bool) -> void:
	if _placement_mode != SCULPT_THERMAL_MODE:
		super._place_current_hit(continuous)
		return
	if _last_hit.is_empty():
		_set_status("Viewport pick did not intersect terrain.")
		return
	var direction: Vector3 = _last_hit.get("dir", Vector3.ZERO)
	if direction.length_squared() < 0.99:
		return
	_place_thermal_stroke(direction, continuous)
	_update_preview()


func _place_thermal_stroke(direction: Vector3, continuous: bool) -> void:
	_begin_sculpt_telemetry("Thermal")
	if Planet.cfg == null or not Planet.ready_state:
		_finish_sculpt_telemetry()
		return
	var planet_radius: float = maxf(float(Planet.cfg.planet_radius), 1.0)
	if _skip_redundant_sculpt_stamp(direction, continuous, planet_radius):
		_finish_sculpt_telemetry()
		return
	var changed: int = _thermal_erosion_brush(direction, planet_radius)
	if changed > 0:
		_last_sculpt_dir = direction
		_set_status("Thermal erosion: %d samples changed • %.1f m radius • %.1f° talus • %.0f%% strength." % [
			changed, _sculpt_radius_m, _thermal_talus_deg, _thermal_strength * 100.0])
	_finish_sculpt_telemetry()


func _thermal_erosion_brush(center_dir: Vector3, planet_radius: float) -> int:
	var writes: Array[Dictionary] = _thermal_erosion_writes(center_dir, planet_radius)
	return _apply_absolute_delta_writes(writes, center_dir, planet_radius)


func _thermal_erosion_writes(center_dir: Vector3, planet_radius: float) -> Array[Dictionary]:
	var writes: Array[Dictionary] = []
	var samples: Array[Dictionary] = _collect_sculpt_samples(center_dir, planet_radius)
	if samples.is_empty():
		return writes
	var spacing_m: float = maxf(Deltas.sample_spacing(planet_radius), 0.001)
	var angular_radius: float = (_sculpt_radius_m + spacing_m * 5.0) / maxf(planet_radius, 1.0)
	var snap: Dictionary = Deltas.snapshot_for_bounds(center_dir.normalized(), angular_radius)
	var height_cache: Dictionary = {}
	var changes: Dictionary = {}
	var addresses: Dictionary = {}
	var talus_drop_m: float = tan(deg_to_rad(clampf(_thermal_talus_deg, 0.0, 89.0))) * spacing_m

	for sample: Dictionary in samples:
		var source: Vector3i = sample["address"]
		var source_height: float = _snapshot_final_height(source, snap, height_cache)
		var source_weight: float = clampf(float(sample["weight"]), 0.0, 1.0)
		if source_weight <= 0.0001:
			continue
		var downhill: Array[Dictionary] = []
		for offset: Vector2i in THERMAL_NEIGHBORS:
			var neighbor: Vector3i = Deltas.canonical_address(
				source.x, source.y + offset.x, source.z + offset.y)
			if neighbor.x < 0 or neighbor == source:
				continue
			var neighbor_height: float = _snapshot_final_height(neighbor, snap, height_cache)
			var excess_m: float = source_height - neighbor_height - talus_drop_m
			if excess_m > 1e-5:
				downhill.append({"address": neighbor, "excess": excess_m})
		if downhill.is_empty():
			continue

		# For N equally low neighbors, excess/(N+1) per edge is the simultaneous
		# equalizing transfer without overshooting the source below its recipients.
		# Strength and brush falloff scale that stable one-pass relaxation.
		var relaxation: float = clampf(_thermal_strength * source_weight, 0.0, 1.0)
		var denominator: float = float(downhill.size() + 1)
		for edge: Dictionary in downhill:
			var transfer_m: float = float(edge["excess"]) * relaxation / denominator
			if transfer_m <= 1e-7:
				continue
			var neighbor: Vector3i = edge["address"]
			_accumulate_thermal_change(changes, addresses, source, -transfer_m)
			_accumulate_thermal_change(changes, addresses, neighbor, transfer_m)

	for key_value: Variant in changes.keys():
		var key: String = String(key_value)
		var address: Vector3i = addresses[key]
		var change_m: float = float(changes[key])
		if absf(change_m) <= 1e-7:
			continue
		var direction: Vector3 = Deltas.lattice_to_dir(address.x, float(address.y), float(address.z))
		var before_delta: float = Deltas.offset_at_snapshot(direction, snap)
		var desired_delta: float = clampf(
			before_delta + change_m,
			SCULPT_MIN_OFFSET_M,
			SCULPT_MAX_OFFSET_M)
		if absf(desired_delta - before_delta) > 1e-7:
			writes.append({"address": address, "value": desired_delta})
	return writes


func _accumulate_thermal_change(changes: Dictionary, addresses: Dictionary,
		address: Vector3i, delta_m: float) -> void:
	var key: String = _address_key(address)
	changes[key] = float(changes.get(key, 0.0)) + delta_m
	addresses[key] = address


func _update_preview() -> void:
	if _placement_mode != SCULPT_THERMAL_MODE:
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
		var color := Color(0.84, 0.48, 0.20, 1.0)
		_draw_surface_ring(direction, height, _sculpt_radius_m, color)
		if _sculpt_hardness > 0.02:
			_draw_surface_ring(direction, height,
				maxf(0.1, _sculpt_radius_m * _sculpt_hardness),
				Color(color.r, color.g, color.b, 0.55))
	_draw_selected_water_feature()
