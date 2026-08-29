class_name WorldAuthoringLiveEditorPhase5
extends "res://scripts/world_authoring/world_authoring_editor_live_phase4.gd"
## Phase 5 direct 3D editing for authored water control points.
##
## MOVE mode picks the selected feature's lake vertices / river knots in screen
## space, then drags the chosen point across rendered terrain. During the drag the
## staged resource is updated directly for immediate preview, but no history or
## recovery save is generated. Mouse release captures the final geometry, restores
## the pre-drag state, and commits that final geometry through one stage_action;
## therefore a complete drag is exactly one Undo/Redo transaction.

signal water_preview_changed

const WATER_MOVE_MODE: int = 100
const DEFAULT_HANDLE_PICK_RADIUS_PX: float = 26.0

var _water_handle_pick_radius_px: float = DEFAULT_HANDLE_PICK_RADIUS_PX
var _water_drag_active: bool = false
var _water_drag_changed: bool = false
var _water_drag_feature_id: String = ""
var _water_drag_index: int = -1
var _water_drag_is_river: bool = false
var _water_drag_before: Variant = null


func _build_water_page() -> void:
	super._build_water_page()
	if _world_host == null:
		return
	var water: Resource = _session.active_water_profile() as Resource
	if water == null or _selected_water_feature_id.is_empty():
		return
	var feature: Resource = water.call("find_feature", _selected_water_feature_id) as Resource
	if feature == null:
		return

	_section("Direct control-point editing")
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 7)
	_workspace.add_child(row)
	var move_button := Button.new()
	move_button.text = "STOP MOVING POINTS" if _placement_mode == WATER_MOVE_MODE else "MOVE POINTS IN VIEWPORT"
	move_button.pressed.connect(func() -> void:
		if _placement_mode == WATER_MOVE_MODE:
			_cancel_water_drag()
			_set_placement_mode(PlacementMode.NONE)
		else:
			_set_placement_mode(WATER_MOVE_MODE)
		_refresh_current_category()
	)
	row.add_child(move_button)
	var kind: String = "river knots" if int(feature.get(&"feature_type")) == 1 else "lake vertices"
	var info := Label.new()
	info.text = "Select and drag %s" % kind
	info.modulate = Color(0.64, 0.76, 0.86)
	row.add_child(info)
	_add_number_field("Handle pick radius", _water_handle_pick_radius_px,
		8.0, 80.0, 1.0, " px", func(value: float) -> void:
			_water_handle_pick_radius_px = value
	)
	_add_note("LMB near a magenta control point selects it; hold and drag across terrain to move it. River handle offsets, width, depth and current stay attached to the knot. Release commits one Undo step. RMB/Esc cancels an active drag and restores its original geometry.")


func _unhandled_input(event: InputEvent) -> void:
	if _placement_mode != WATER_MOVE_MODE:
		super._unhandled_input(event)
		return
	if _world_host == null:
		return

	if event is InputEventKey:
		var key_event := event as InputEventKey
		if key_event.pressed and not key_event.echo and key_event.keycode == KEY_TAB:
			_finish_water_drag()
			_set_navigation(true)
			get_viewport().set_input_as_handled()
			return
		if key_event.pressed and not key_event.echo and key_event.keycode == KEY_ESCAPE:
			_cancel_water_drag()
			_set_placement_mode(PlacementMode.NONE)
			get_viewport().set_input_as_handled()
			return

	if event is InputEventMouseButton:
		var mouse_button := event as InputEventMouseButton
		if mouse_button.button_index == MOUSE_BUTTON_RIGHT and mouse_button.pressed:
			_cancel_water_drag()
			_set_placement_mode(PlacementMode.NONE)
			get_viewport().set_input_as_handled()
			return
		if mouse_button.button_index == MOUSE_BUTTON_LEFT:
			if mouse_button.pressed:
				if _is_live_viewport_point(mouse_button.position):
					_begin_water_drag(mouse_button.position)
					get_viewport().set_input_as_handled()
				return
			if _water_drag_active:
				_finish_water_drag()
				get_viewport().set_input_as_handled()
				return

	if event is InputEventMouseMotion and _water_drag_active:
		var motion := event as InputEventMouseMotion
		if (motion.button_mask & MOUSE_BUTTON_MASK_LEFT) != 0 \
				and _is_live_viewport_point(motion.position):
			_last_hit = _screen_aim(motion.position)
			_move_water_drag_to_hit()
			get_viewport().set_input_as_handled()


func _placement_status_text() -> String:
	if _placement_mode == WATER_MOVE_MODE:
		if _water_drag_active:
			return "WATER POINT DRAG — move across terrain • release commit • RMB/Esc cancel"
		return "WATER POINT MOVE — LMB near magenta point and drag • RMB/Esc stop • TAB navigate"
	return super._placement_status_text()


func _set_placement_mode(mode: int) -> void:
	if _placement_mode == WATER_MOVE_MODE and mode != WATER_MOVE_MODE:
		_finish_water_drag()
	super._set_placement_mode(mode)


func _discard_interactive_transactions() -> void:
	_cancel_water_drag()
	super._discard_interactive_transactions()


func _begin_water_drag(screen_position: Vector2) -> void:
	_cancel_water_drag()
	var picked: Dictionary = _pick_water_control_point(screen_position)
	if picked.is_empty():
		_set_status("No water control point inside %.0f px. Increase Handle pick radius if needed." % _water_handle_pick_radius_px)
		return
	var water: Resource = _session.active_water_profile() as Resource
	if water == null:
		return
	var feature: Resource = water.call("find_feature", String(picked.get("feature_id", ""))) as Resource
	if feature == null:
		return
	_water_drag_feature_id = String(feature.get(&"feature_id"))
	_water_drag_index = int(picked.get("index", -1))
	_water_drag_is_river = bool(picked.get("is_river", false))
	_water_drag_before = _capture_feature_geometry(feature, _water_drag_is_river)
	_water_drag_active = _water_drag_index >= 0 and _water_drag_before != null
	_water_drag_changed = false
	if _water_drag_active:
		_set_status("Selected %s %d — drag across terrain, release to commit." % [
			"river knot" if _water_drag_is_river else "lake vertex", _water_drag_index + 1])
		if _live_status_label != null:
			_live_status_label.text = _placement_status_text()


func _move_water_drag_to_hit() -> void:
	if not _water_drag_active or _last_hit.is_empty():
		return
	var world: Vec3D = _last_hit.get("world") as Vec3D
	if world == null:
		return
	var water: Resource = _session.active_water_profile() as Resource
	if water == null:
		return
	var feature: Resource = water.call("find_feature", _water_drag_feature_id) as Resource
	if feature == null:
		_cancel_water_drag()
		return
	var point := Vector3(float(world.x), float(world.y), float(world.z))
	if _water_drag_is_river:
		var knots: Array = (feature.get(&"river_knots") as Array).duplicate(true)
		if _water_drag_index < 0 or _water_drag_index >= knots.size():
			_cancel_water_drag()
			return
		var knot: Dictionary = (knots[_water_drag_index] as Dictionary).duplicate(true)
		knot["position_body_m"] = point
		knots[_water_drag_index] = knot
		feature.set(&"river_knots", knots)
	else:
		if not bool(feature.call("set_lake_point", _water_drag_index, point)):
			_cancel_water_drag()
			return
	_water_drag_changed = true
	water_preview_changed.emit()
	_update_preview()


func _finish_water_drag() -> void:
	if not _water_drag_active:
		_clear_water_drag_state()
		return
	var water: Resource = _session.active_water_profile() as Resource
	var feature: Resource = water.call("find_feature", _water_drag_feature_id) as Resource if water != null else null
	if feature == null:
		_clear_water_drag_state()
		return
	if not _water_drag_changed:
		_clear_water_drag_state()
		return

	var feature_id: String = _water_drag_feature_id
	var was_river: bool = _water_drag_is_river
	var final_geometry: Variant = _capture_feature_geometry(feature, was_river)
	_restore_feature_geometry(feature, was_river, _water_drag_before)
	_clear_water_drag_state()
	_session.stage_action("Move water control point", func() -> void:
		var active_water: Resource = _session.active_water_profile() as Resource
		if active_water == null:
			return
		var active_feature: Resource = active_water.call("find_feature", feature_id) as Resource
		if active_feature != null:
			_restore_feature_geometry(active_feature, was_river, final_geometry)
	, WorldAuthoringSession.ApplyScope.TILES)
	water_preview_changed.emit()
	_set_status("Moved water control point • one Undo transaction committed.")


func _cancel_water_drag() -> void:
	if _water_drag_active:
		var water: Resource = _session.active_water_profile() as Resource
		var feature: Resource = water.call("find_feature", _water_drag_feature_id) as Resource if water != null else null
		if feature != null and _water_drag_before != null:
			_restore_feature_geometry(feature, _water_drag_is_river, _water_drag_before)
			water_preview_changed.emit()
	_clear_water_drag_state()
	_update_preview()


func _clear_water_drag_state() -> void:
	_water_drag_active = false
	_water_drag_changed = false
	_water_drag_feature_id = ""
	_water_drag_index = -1
	_water_drag_is_river = false
	_water_drag_before = null


func _capture_feature_geometry(feature: Resource, is_river: bool) -> Variant:
	if is_river:
		return (feature.get(&"river_knots") as Array).duplicate(true)
	return (feature.get(&"lake_polygon_body_m") as PackedVector3Array).duplicate()


func _restore_feature_geometry(feature: Resource, is_river: bool, geometry: Variant) -> void:
	if feature == null:
		return
	if is_river and geometry is Array:
		feature.set(&"river_knots", (geometry as Array).duplicate(true))
		feature.call("ensure_valid")
	elif not is_river and geometry is PackedVector3Array:
		feature.set(&"lake_polygon_body_m", (geometry as PackedVector3Array).duplicate())


func _pick_water_control_point(screen_position: Vector2) -> Dictionary:
	if _camera == null or _session == null:
		return {}
	var water: Resource = _session.active_water_profile() as Resource
	if water == null:
		return {}
	var feature: Resource = water.call("find_feature", _selected_water_feature_id) as Resource
	if feature == null:
		return {}
	var is_river: bool = int(feature.get(&"feature_type")) == 1
	var nearest_px: float = _water_handle_pick_radius_px
	var nearest_index: int = -1
	if is_river:
		var knots: Array = feature.get(&"river_knots") as Array
		for index: int in knots.size():
			var knot: Dictionary = knots[index]
			var point: Vector3 = knot.get("position_body_m", Vector3.ZERO)
			var distance_px: float = _screen_distance_to_body_point(screen_position, point)
			if distance_px <= nearest_px:
				nearest_px = distance_px
				nearest_index = index
	else:
		var polygon: PackedVector3Array = feature.get(&"lake_polygon_body_m")
		for index: int in polygon.size():
			var distance_px: float = _screen_distance_to_body_point(screen_position, polygon[index])
			if distance_px <= nearest_px:
				nearest_px = distance_px
				nearest_index = index
	if nearest_index < 0:
		return {}
	return {
		"feature_id": String(feature.get(&"feature_id")),
		"index": nearest_index,
		"is_river": is_river,
		"distance_px": nearest_px,
	}


func _screen_distance_to_body_point(screen_position: Vector2, body_point: Vector3) -> float:
	if body_point.length_squared() <= 1.0 or _camera == null:
		return INF
	var render_point: Vector3 = Frames.to_render(Vec3D.from_v3(body_point))
	var camera_forward: Vector3 = -_camera.global_transform.basis.z.normalized()
	if (render_point - _camera.global_position).dot(camera_forward) <= 0.0:
		return INF
	var projected: Vector2 = _camera.unproject_position(render_point)
	return projected.distance_to(screen_position)
