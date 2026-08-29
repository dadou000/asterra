class_name WorldAuthoringLiveEditorPhase6
extends "res://scripts/world_authoring/world_authoring_editor_live_phase5.gd"
## Phase 6 river-shape editing.
##
## The selected river knot exposes its hydrodynamic scalars and cubic Bezier
## handle offsets directly in Planet Studio. A dedicated viewport mode manipulates
## handle endpoints in the knot's local tangent plane, so shaping a river does not
## accidentally push control handles radially through the planet. As with Phase 5
## point dragging, live motion mutates only disposable staged geometry and release
## commits exactly one Undo/Redo transaction.

const RIVER_HANDLE_MODE: int = 101
const HANDLE_PICK_RADIUS_PX: float = 24.0
const MAX_HANDLE_LENGTH_M: float = 100000.0

var _selected_river_knot_index: int = 0
var _handle_drag_active: bool = false
var _handle_drag_changed: bool = false
var _handle_drag_feature_id: String = ""
var _handle_drag_knot_index: int = -1
var _handle_drag_key: String = ""
var _handle_drag_before: Array = []


func _build_water_page() -> void:
	super._build_water_page()
	if _world_host == null or _session == null:
		return
	var water: Resource = _session.active_water_profile() as Resource
	if water == null or _selected_water_feature_id.is_empty():
		return
	var feature: Resource = water.call("find_feature", _selected_water_feature_id) as Resource
	if feature == null or int(feature.get(&"feature_type")) != WATER_FEATURE_SCRIPT.FeatureType.RIVER:
		return
	var knots: Array = feature.get(&"river_knots") as Array
	if knots.is_empty():
		_add_note("Add at least one river knot before editing Bezier handles or per-knot hydrodynamics.")
		return
	_selected_river_knot_index = clampi(_selected_river_knot_index, 0, knots.size() - 1)

	_section("Selected river knot")
	var selector := OptionButton.new()
	selector.custom_minimum_size.x = 260.0
	for index: int in knots.size():
		selector.add_item("Knot %d" % (index + 1))
		selector.set_item_metadata(index, index)
	selector.select(_selected_river_knot_index)
	selector.item_selected.connect(func(index: int) -> void:
		_selected_river_knot_index = int(selector.get_item_metadata(index))
		_update_preview()
		_refresh_current_category()
	)
	_add_control_row("Knot", selector)

	var knot: Dictionary = knots[_selected_river_knot_index]
	_add_number_field("Knot width", float(knot.get("width_m", 10.0)), 0.05, 100000.0, 0.05, " m", func(value: float) -> void:
		_stage_selected_river_knot_scalar("width_m", maxf(value, 0.05), "Change river knot width")
	)
	_add_number_field("Knot depth", float(knot.get("depth_m", float(feature.get(&"default_depth_m")))), 0.0, 100000.0, 0.05, " m", func(value: float) -> void:
		_stage_selected_river_knot_scalar("depth_m", maxf(value, 0.0), "Change river knot depth")
	)
	_add_number_field("Knot current", float(knot.get("current_m_s", 1.0)), 0.0, 250.0, 0.01, " m/s", func(value: float) -> void:
		_stage_selected_river_knot_scalar("current_m_s", maxf(value, 0.0), "Change river knot current")
	)
	_add_handle_vector_editor("Handle in", "handle_in_offset_m", Vector3(knot.get("handle_in_offset_m", Vector3.ZERO)))
	_add_handle_vector_editor("Handle out", "handle_out_offset_m", Vector3(knot.get("handle_out_offset_m", Vector3.ZERO)))

	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 7)
	_workspace.add_child(row)
	var handle_button := Button.new()
	handle_button.text = "STOP HANDLE EDIT" if _placement_mode == RIVER_HANDLE_MODE else "EDIT BEZIER HANDLES"
	handle_button.pressed.connect(func() -> void:
		if _placement_mode == RIVER_HANDLE_MODE:
			_finish_handle_drag()
			_set_placement_mode(PlacementMode.NONE)
		else:
			_set_placement_mode(RIVER_HANDLE_MODE)
		_refresh_current_category()
	)
	row.add_child(handle_button)
	var auto_button := Button.new()
	auto_button.text = "Auto Tangents"
	auto_button.tooltip_text = "Initialize this knot's Bezier handles from neighboring knots, projected into the local tangent plane."
	auto_button.pressed.connect(_auto_selected_river_handles)
	row.add_child(auto_button)
	var zero_button := Button.new()
	zero_button.text = "Zero Handles"
	zero_button.pressed.connect(_zero_selected_river_handles)
	row.add_child(zero_button)
	_add_note("Handle mode draws the selected knot's in/out arms. Drag an endpoint in the local surface tangent plane; release creates one Undo step. Width/depth/current remain attached to the knot and the feature Current scale is applied once after interpolation.")


func _add_handle_vector_editor(label_text: String, key: String, value: Vector3) -> void:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 5)
	_workspace.add_child(row)
	var label := Label.new()
	label.text = label_text
	label.custom_minimum_size.x = 260.0
	row.add_child(label)
	var values: Array[float] = [value.x, value.y, value.z]
	for axis: int in 3:
		var spin := _small_spin(-MAX_HANDLE_LENGTH_M, MAX_HANDLE_LENGTH_M, 0.1, values[axis])
		spin.tooltip_text = ["Body-space X offset", "Body-space Y offset", "Body-space Z offset"][axis]
		spin.value_changed.connect(func(next_value: float) -> void:
			_stage_selected_river_handle_component(key, axis, next_value)
		)
		row.add_child(spin)


func _stage_selected_river_knot_scalar(key: String, value: float, action_name: String) -> void:
	var feature_id: String = _selected_water_feature_id
	var knot_index: int = _selected_river_knot_index
	_session.stage_action(action_name, func() -> void:
		var water: Resource = _session.active_water_profile() as Resource
		var feature: Resource = water.call("find_feature", feature_id) as Resource if water != null else null
		if feature == null:
			return
		var knots: Array = (feature.get(&"river_knots") as Array).duplicate(true)
		if knot_index < 0 or knot_index >= knots.size():
			return
		var knot: Dictionary = (knots[knot_index] as Dictionary).duplicate(true)
		knot[key] = value
		knots[knot_index] = knot
		feature.set(&"river_knots", knots)
		feature.call("ensure_valid")
	, WorldAuthoringSession.ApplyScope.TILES)


func _stage_selected_river_handle_component(key: String, axis: int, value: float) -> void:
	var feature_id: String = _selected_water_feature_id
	var knot_index: int = _selected_river_knot_index
	_session.stage_action("Change river Bezier handle", func() -> void:
		var water: Resource = _session.active_water_profile() as Resource
		var feature: Resource = water.call("find_feature", feature_id) as Resource if water != null else null
		if feature == null:
			return
		var knots: Array = (feature.get(&"river_knots") as Array).duplicate(true)
		if knot_index < 0 or knot_index >= knots.size():
			return
		var knot: Dictionary = (knots[knot_index] as Dictionary).duplicate(true)
		var offset: Vector3 = knot.get(key, Vector3.ZERO)
		offset[axis] = value
		if offset.length() > MAX_HANDLE_LENGTH_M:
			offset = offset.normalized() * MAX_HANDLE_LENGTH_M
		knot[key] = offset
		knots[knot_index] = knot
		feature.set(&"river_knots", knots)
		feature.call("ensure_valid")
	, WorldAuthoringSession.ApplyScope.TILES)
	_update_preview()


func _auto_selected_river_handles() -> void:
	var water: Resource = _session.active_water_profile() as Resource
	var feature: Resource = water.call("find_feature", _selected_water_feature_id) as Resource if water != null else null
	if feature == null:
		return
	var knots: Array = feature.get(&"river_knots") as Array
	var index: int = clampi(_selected_river_knot_index, 0, knots.size() - 1)
	if knots.size() < 2:
		return
	var before: Array = knots.duplicate(true)
	var current: Vector3 = (knots[index] as Dictionary).get("position_body_m", Vector3.ZERO)
	if current.length_squared() <= 1.0:
		return
	var previous: Vector3 = current
	var following: Vector3 = current
	if index > 0:
		previous = (knots[index - 1] as Dictionary).get("position_body_m", current)
	if index + 1 < knots.size():
		following = (knots[index + 1] as Dictionary).get("position_body_m", current)
	var tangent: Vector3
	if index == 0:
		tangent = following - current
	elif index == knots.size() - 1:
		tangent = current - previous
	else:
		tangent = following - previous
	var normal := current.normalized()
	tangent -= normal * tangent.dot(normal)
	if tangent.length_squared() < 1e-8:
		return
	tangent = tangent.normalized()
	var previous_distance: float = current.distance_to(previous) if index > 0 else INF
	var next_distance: float = current.distance_to(following) if index + 1 < knots.size() else INF
	var handle_length: float = minf(previous_distance, next_distance)
	if not is_finite(handle_length):
		handle_length = previous_distance if is_finite(previous_distance) else next_distance
	handle_length = clampf(handle_length / 3.0, 0.0, MAX_HANDLE_LENGTH_M)
	var final_knots: Array = before.duplicate(true)
	var knot: Dictionary = (final_knots[index] as Dictionary).duplicate(true)
	knot["handle_in_offset_m"] = -tangent * handle_length if index > 0 else Vector3.ZERO
	knot["handle_out_offset_m"] = tangent * handle_length if index + 1 < final_knots.size() else Vector3.ZERO
	final_knots[index] = knot
	var feature_id: String = _selected_water_feature_id
	_session.stage_action("Auto river Bezier tangents", func() -> void:
		var active_water: Resource = _session.active_water_profile() as Resource
		var active_feature: Resource = active_water.call("find_feature", feature_id) as Resource if active_water != null else null
		if active_feature != null:
			active_feature.set(&"river_knots", final_knots.duplicate(true))
			active_feature.call("ensure_valid")
	, WorldAuthoringSession.ApplyScope.TILES)
	_update_preview()
	_refresh_current_category()


func _zero_selected_river_handles() -> void:
	var feature_id: String = _selected_water_feature_id
	var knot_index: int = _selected_river_knot_index
	_session.stage_action("Zero river Bezier handles", func() -> void:
		var water: Resource = _session.active_water_profile() as Resource
		var feature: Resource = water.call("find_feature", feature_id) as Resource if water != null else null
		if feature == null:
			return
		var knots: Array = (feature.get(&"river_knots") as Array).duplicate(true)
		if knot_index < 0 or knot_index >= knots.size():
			return
		var knot: Dictionary = (knots[knot_index] as Dictionary).duplicate(true)
		knot["handle_in_offset_m"] = Vector3.ZERO
		knot["handle_out_offset_m"] = Vector3.ZERO
		knots[knot_index] = knot
		feature.set(&"river_knots", knots)
	, WorldAuthoringSession.ApplyScope.TILES)
	_update_preview()
	_refresh_current_category()


func _begin_water_drag(screen_position: Vector2) -> void:
	var picked: Dictionary = _pick_water_control_point(screen_position)
	if not picked.is_empty() and bool(picked.get("is_river", false)):
		_selected_river_knot_index = int(picked.get("index", _selected_river_knot_index))
	super._begin_water_drag(screen_position)


func _unhandled_input(event: InputEvent) -> void:
	if _placement_mode != RIVER_HANDLE_MODE:
		super._unhandled_input(event)
		return
	if _world_host == null:
		return

	if event is InputEventKey:
		var key_event := event as InputEventKey
		if key_event.pressed and not key_event.echo and key_event.keycode == KEY_TAB:
			_finish_handle_drag()
			_set_navigation(not _navigation_active)
			get_viewport().set_input_as_handled()
			return
		if _navigation_active:
			return
		if key_event.pressed and not key_event.echo and key_event.keycode == KEY_ESCAPE:
			_cancel_handle_drag()
			_set_placement_mode(PlacementMode.NONE)
			get_viewport().set_input_as_handled()
			return
	if _navigation_active:
		return

	if event is InputEventMouseButton:
		var mouse_button := event as InputEventMouseButton
		if mouse_button.button_index == MOUSE_BUTTON_RIGHT and mouse_button.pressed:
			_cancel_handle_drag()
			_set_placement_mode(PlacementMode.NONE)
			get_viewport().set_input_as_handled()
			return
		if mouse_button.button_index == MOUSE_BUTTON_LEFT:
			if mouse_button.pressed:
				if _is_live_viewport_point(mouse_button.position):
					_begin_handle_drag(mouse_button.position)
					get_viewport().set_input_as_handled()
				return
			if _handle_drag_active:
				_finish_handle_drag()
				get_viewport().set_input_as_handled()
				return

	if event is InputEventMouseMotion and _handle_drag_active:
		var motion := event as InputEventMouseMotion
		if (motion.button_mask & MOUSE_BUTTON_MASK_LEFT) != 0 and _is_live_viewport_point(motion.position):
			_move_handle_drag(motion.position)
			get_viewport().set_input_as_handled()


func _placement_status_text() -> String:
	if _placement_mode == RIVER_HANDLE_MODE:
		if _handle_drag_active:
			return "RIVER HANDLE DRAG — tangent-plane move • release commit • RMB/Esc cancel"
		return "RIVER HANDLE EDIT — drag cyan/orange endpoint • RMB/Esc stop • TAB navigate"
	return super._placement_status_text()


func _set_placement_mode(mode: int) -> void:
	if _placement_mode == RIVER_HANDLE_MODE and mode != RIVER_HANDLE_MODE:
		_finish_handle_drag()
	super._set_placement_mode(mode)


func _discard_interactive_transactions() -> void:
	_cancel_handle_drag()
	super._discard_interactive_transactions()


func _begin_handle_drag(screen_position: Vector2) -> void:
	_cancel_handle_drag()
	var picked: Dictionary = _pick_selected_river_handle(screen_position)
	if picked.is_empty():
		_set_status("No Bezier handle endpoint inside %.0f px. Use Auto Tangents or numeric handle values to initialize zero-length handles." % HANDLE_PICK_RADIUS_PX)
		return
	var water: Resource = _session.active_water_profile() as Resource
	var feature: Resource = water.call("find_feature", _selected_water_feature_id) as Resource if water != null else null
	if feature == null:
		return
	_handle_drag_active = true
	_handle_drag_changed = false
	_handle_drag_feature_id = _selected_water_feature_id
	_handle_drag_knot_index = int(picked.get("index", -1))
	_handle_drag_key = String(picked.get("key", ""))
	_handle_drag_before = (feature.get(&"river_knots") as Array).duplicate(true)
	_set_status("Dragging %s on river knot %d." % ["IN" if _handle_drag_key == "handle_in_offset_m" else "OUT", _handle_drag_knot_index + 1])
	_update_preview()


func _move_handle_drag(screen_position: Vector2) -> void:
	if not _handle_drag_active or _camera == null:
		return
	var water: Resource = _session.active_water_profile() as Resource
	var feature: Resource = water.call("find_feature", _handle_drag_feature_id) as Resource if water != null else null
	if feature == null:
		_cancel_handle_drag()
		return
	var knots: Array = (feature.get(&"river_knots") as Array).duplicate(true)
	if _handle_drag_knot_index < 0 or _handle_drag_knot_index >= knots.size():
		_cancel_handle_drag()
		return
	var knot: Dictionary = (knots[_handle_drag_knot_index] as Dictionary).duplicate(true)
	var knot_position: Vector3 = knot.get("position_body_m", Vector3.ZERO)
	var endpoint: Variant = _screen_to_knot_tangent_plane(screen_position, knot_position)
	if not (endpoint is Vector3):
		return
	var offset: Vector3 = endpoint as Vector3 - knot_position
	if offset.length() > MAX_HANDLE_LENGTH_M:
		offset = offset.normalized() * MAX_HANDLE_LENGTH_M
	knot[_handle_drag_key] = offset
	knots[_handle_drag_knot_index] = knot
	feature.set(&"river_knots", knots)
	_handle_drag_changed = true
	water_preview_changed.emit()
	_update_preview()


func _finish_handle_drag() -> void:
	if not _handle_drag_active:
		_clear_handle_drag_state()
		return
	var water: Resource = _session.active_water_profile() as Resource
	var feature: Resource = water.call("find_feature", _handle_drag_feature_id) as Resource if water != null else null
	if feature == null or not _handle_drag_changed:
		_clear_handle_drag_state()
		return
	var feature_id: String = _handle_drag_feature_id
	var final_knots: Array = (feature.get(&"river_knots") as Array).duplicate(true)
	feature.set(&"river_knots", _handle_drag_before.duplicate(true))
	_clear_handle_drag_state()
	_session.stage_action("Move river Bezier handle", func() -> void:
		var active_water: Resource = _session.active_water_profile() as Resource
		var active_feature: Resource = active_water.call("find_feature", feature_id) as Resource if active_water != null else null
		if active_feature != null:
			active_feature.set(&"river_knots", final_knots.duplicate(true))
			active_feature.call("ensure_valid")
	, WorldAuthoringSession.ApplyScope.TILES)
	water_preview_changed.emit()
	_set_status("Moved river Bezier handle • one Undo transaction committed.")
	_update_preview()


func _cancel_handle_drag() -> void:
	if _handle_drag_active:
		var water: Resource = _session.active_water_profile() as Resource
		var feature: Resource = water.call("find_feature", _handle_drag_feature_id) as Resource if water != null else null
		if feature != null and not _handle_drag_before.is_empty():
			feature.set(&"river_knots", _handle_drag_before.duplicate(true))
			water_preview_changed.emit()
	_clear_handle_drag_state()
	_update_preview()


func _clear_handle_drag_state() -> void:
	_handle_drag_active = false
	_handle_drag_changed = false
	_handle_drag_feature_id = ""
	_handle_drag_knot_index = -1
	_handle_drag_key = ""
	_handle_drag_before.clear()


func _pick_selected_river_handle(screen_position: Vector2) -> Dictionary:
	var water: Resource = _session.active_water_profile() as Resource
	var feature: Resource = water.call("find_feature", _selected_water_feature_id) as Resource if water != null else null
	if feature == null or int(feature.get(&"feature_type")) != WATER_FEATURE_SCRIPT.FeatureType.RIVER:
		return {}
	var knots: Array = feature.get(&"river_knots") as Array
	if knots.is_empty():
		return {}
	var index: int = clampi(_selected_river_knot_index, 0, knots.size() - 1)
	var knot: Dictionary = knots[index]
	var position: Vector3 = knot.get("position_body_m", Vector3.ZERO)
	var best_distance: float = HANDLE_PICK_RADIUS_PX
	var best_key: String = ""
	for key: String in ["handle_in_offset_m", "handle_out_offset_m"]:
		var offset: Vector3 = knot.get(key, Vector3.ZERO)
		if offset.length_squared() < 0.0001:
			continue
		var distance_px: float = _screen_distance_to_body_point(screen_position, position + offset)
		if distance_px <= best_distance:
			best_distance = distance_px
			best_key = key
	if best_key.is_empty():
		return {}
	return {"index": index, "key": best_key, "distance_px": best_distance}


func _screen_to_knot_tangent_plane(screen_position: Vector2, knot_position: Vector3) -> Variant:
	if _camera == null or knot_position.length_squared() <= 1.0:
		return null
	var plane_point: Vector3 = Frames.to_render(Vec3D.from_v3(knot_position))
	var normal: Vector3 = knot_position.normalized()
	var ray_origin: Vector3 = _camera.global_position
	var ray_direction: Vector3 = _camera.project_ray_normal(screen_position).normalized()
	var denominator: float = ray_direction.dot(normal)
	if absf(denominator) < 0.02:
		return null
	var distance: float = (plane_point - ray_origin).dot(normal) / denominator
	if distance <= 0.0:
		return null
	var endpoint_render: Vector3 = ray_origin + ray_direction * distance
	var endpoint_world: Vec3D = Frames.to_world(endpoint_render)
	return endpoint_world.to_v3()


func _draw_selected_water_feature() -> void:
	super._draw_selected_water_feature()
	if _preview_mesh == null or _session == null:
		return
	var water: Resource = _session.active_water_profile() as Resource
	var feature: Resource = water.call("find_feature", _selected_water_feature_id) as Resource if water != null else null
	if feature == null or int(feature.get(&"feature_type")) != WATER_FEATURE_SCRIPT.FeatureType.RIVER:
		return
	var knots: Array = feature.get(&"river_knots") as Array
	if knots.is_empty():
		return
	var index: int = clampi(_selected_river_knot_index, 0, knots.size() - 1)
	var knot: Dictionary = knots[index]
	var position: Vector3 = knot.get("position_body_m", Vector3.ZERO)
	if position.length_squared() <= 1.0:
		return
	var in_offset: Vector3 = knot.get("handle_in_offset_m", Vector3.ZERO)
	var out_offset: Vector3 = knot.get("handle_out_offset_m", Vector3.ZERO)
	_draw_handle_arm(position, position + in_offset, Color(0.20, 0.82, 1.0, 1.0), in_offset.length_squared() > 0.0001)
	_draw_handle_arm(position, position + out_offset, Color(1.0, 0.58, 0.16, 1.0), out_offset.length_squared() > 0.0001)


func _draw_handle_arm(origin_body: Vector3, endpoint_body: Vector3, color: Color, visible_arm: bool) -> void:
	if not visible_arm:
		return
	var origin_render: Vector3 = Frames.to_render(Vec3D.from_v3(origin_body))
	var endpoint_render: Vector3 = Frames.to_render(Vec3D.from_v3(endpoint_body))
	_preview_mesh.surface_begin(Mesh.PRIMITIVE_LINES)
	_preview_mesh.surface_set_color(color)
	_preview_mesh.surface_add_vertex(origin_render)
	_preview_mesh.surface_set_color(color)
	_preview_mesh.surface_add_vertex(endpoint_render)
	var camera_distance: float = endpoint_render.distance_to(_camera.global_position) if _camera != null else 10.0
	var marker_radius: float = clampf(camera_distance * 0.004, 0.08, 6.0)
	var up: Vector3 = endpoint_body.normalized()
	var basis: Array = CubeSphere.tangent_basis(up)
	var right: Vector3 = basis[0] * marker_radius
	var tangent_up: Vector3 = basis[1] * marker_radius
	for axis: Vector3 in [right, tangent_up]:
		_preview_mesh.surface_set_color(color)
		_preview_mesh.surface_add_vertex(endpoint_render - axis)
		_preview_mesh.surface_set_color(color)
		_preview_mesh.surface_add_vertex(endpoint_render + axis)
	_preview_mesh.surface_end()
