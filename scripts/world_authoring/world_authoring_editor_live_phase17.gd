class_name WorldAuthoringLiveEditorPhase17
extends "res://scripts/world_authoring/world_authoring_editor_live_phase15.gd"
## Phase 17: editor-style viewport interaction and a quieter authoring shell.
##
## Planet Studio keeps the system pointer visible at all times. Holding RMB over
## the live viewport rotates the existing Asterra camera without switching input
## modes or capturing/warping the pointer. TAB controls keyboard movement only.
## The authoring panel can be hidden independently with the floating eye button.

const COMPACT_PANEL_WIDTH_PX: float = 760.0
const CAMERA_LOOK_SENS: float = 0.0022
const EYE_BUTTON_SIZE := Vector2(38.0, 34.0)
const EYE_BUTTON_MARGIN_PX: float = 8.0

enum MoreAction {
	SAVE_PRESET = 1,
	LOAD_PRESET = 2,
	REVERT = 3,
}

var _authoring_panel: PanelContainer
var _eye_button: Button
var _camera_look_active: bool = false


func _ready() -> void:
	super._ready()
	Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
	if _player != null and _player.has_method("set_mouse_captured"):
		_player.call("set_mouse_captured", false)
	_rewrite_legacy_navigation_hints()
	_sync_eye_button_position()


func _build_shell() -> void:
	super._build_shell()
	if _world_host == null:
		return
	for child: Node in get_children():
		if child is PanelContainer:
			_authoring_panel = child as PanelContainer
			break
	if _authoring_panel != null:
		_authoring_panel.offset_right = COMPACT_PANEL_WIDTH_PX
		_hide_legacy_navigation_button(_authoring_panel)
	_create_eye_button()
	_rewrite_legacy_navigation_hints()


func _build_live_toolbar() -> HBoxContainer:
	var toolbar := HBoxContainer.new()
	toolbar.custom_minimum_size.y = 40.0
	toolbar.add_theme_constant_override("separation", 4)

	var back := _compact_toolbar_button("Back", 52.0)
	back.tooltip_text = "Return to the start menu"
	back.pressed.connect(func() -> void: get_tree().change_scene_to_file(START_MENU_SCENE))
	toolbar.add_child(back)

	var title := Label.new()
	title.text = "PLANET STUDIO"
	title.custom_minimum_size.x = 118.0
	title.add_theme_font_size_override("font_size", 16)
	toolbar.add_child(title)

	_body_selector = OptionButton.new()
	_body_selector.custom_minimum_size.x = 132.0
	_body_selector.item_selected.connect(_on_body_selected)
	toolbar.add_child(_body_selector)

	_dirty_label = Label.new()
	_dirty_label.custom_minimum_size.x = 92.0
	toolbar.add_child(_dirty_label)

	var spacer := Control.new()
	spacer.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	toolbar.add_child(spacer)

	_undo_button = _compact_toolbar_button("Undo", 52.0)
	_undo_button.tooltip_text = "Undo the last authoring action"
	_undo_button.pressed.connect(_on_undo_pressed)
	toolbar.add_child(_undo_button)

	_redo_button = _compact_toolbar_button("Redo", 52.0)
	_redo_button.tooltip_text = "Redo the last authoring action"
	_redo_button.pressed.connect(_on_redo_pressed)
	toolbar.add_child(_redo_button)

	_apply_button = _compact_toolbar_button("Apply", 58.0)
	_apply_button.tooltip_text = "Apply only the runtime subsystems that changed"
	_apply_button.pressed.connect(_on_apply_pressed)
	toolbar.add_child(_apply_button)

	# The base editor expects this field for toolbar-state updates. Revert itself is
	# moved into the overflow menu to keep the primary toolbar compact.
	_revert_button = Button.new()
	_revert_button.pressed.connect(_on_revert_pressed)

	var more := MenuButton.new()
	more.text = "⋯"
	more.tooltip_text = "Preset and revert actions"
	more.custom_minimum_size = Vector2(38.0, 34.0)
	var popup: PopupMenu = more.get_popup()
	popup.add_item("Save preset…", MoreAction.SAVE_PRESET)
	popup.add_item("Load preset…", MoreAction.LOAD_PRESET)
	popup.add_separator()
	popup.add_item("Revert staged changes", MoreAction.REVERT)
	popup.id_pressed.connect(_on_more_action)
	toolbar.add_child(more)
	return toolbar


func _on_more_action(id: int) -> void:
	match id:
		MoreAction.SAVE_PRESET:
			_save_dialog.popup_centered_ratio(0.72)
		MoreAction.LOAD_PRESET:
			_load_dialog.popup_centered_ratio(0.72)
		MoreAction.REVERT:
			_on_revert_pressed()


func _create_eye_button() -> void:
	if _eye_button != null:
		return
	_eye_button = Button.new()
	_eye_button.name = "AuthoringVisibilityButton"
	_eye_button.text = "👁"
	_eye_button.tooltip_text = "Hide authoring panel"
	_eye_button.custom_minimum_size = EYE_BUTTON_SIZE
	_eye_button.mouse_filter = Control.MOUSE_FILTER_STOP
	_eye_button.pressed.connect(_toggle_authoring_panel)
	add_child(_eye_button)
	_sync_eye_button_position()


func _toggle_authoring_panel() -> void:
	if _authoring_panel == null:
		return
	_authoring_panel.visible = not _authoring_panel.visible
	_eye_button.tooltip_text = "Hide authoring panel" if _authoring_panel.visible \
		else "Show authoring panel"
	_sync_eye_button_position()
	_last_hit.clear()
	_update_preview()


func _sync_eye_button_position() -> void:
	if _eye_button == null:
		return
	var x := EYE_BUTTON_MARGIN_PX
	if _authoring_panel != null and _authoring_panel.visible:
		x = COMPACT_PANEL_WIDTH_PX + EYE_BUTTON_MARGIN_PX
	_eye_button.set_anchors_preset(Control.PRESET_TOP_LEFT)
	_eye_button.offset_left = x
	_eye_button.offset_top = EYE_BUTTON_MARGIN_PX
	_eye_button.offset_right = x + EYE_BUTTON_SIZE.x
	_eye_button.offset_bottom = EYE_BUTTON_MARGIN_PX + EYE_BUTTON_SIZE.y


func _hide_legacy_navigation_button(node: Node) -> void:
	for child: Node in node.get_children():
		if child is Button:
			var button := child as Button
			if button.text.begins_with("NAVIGATE"):
				button.visible = false
				continue
		_hide_legacy_navigation_button(child)


func _unhandled_input(event: InputEvent) -> void:
	if _world_host == null:
		return

	# RMB is globally reserved for camera look in Planet Studio. Intercept it before
	# older water/sculpt layers can interpret RMB as cancel/stop.
	if event is InputEventMouseButton:
		var button := event as InputEventMouseButton
		if button.button_index == MOUSE_BUTTON_RIGHT:
			if button.pressed:
				if _is_live_viewport_point(button.position):
					_camera_look_active = true
					get_viewport().set_input_as_handled()
				return
			if _camera_look_active:
				_camera_look_active = false
				get_viewport().set_input_as_handled()
				return

	if event is InputEventMouseMotion and _camera_look_active:
		var motion := event as InputEventMouseMotion
		_rotate_camera(motion.relative)
		get_viewport().set_input_as_handled()
		return

	super._unhandled_input(event)


func _rotate_camera(relative: Vector2) -> void:
	if _player == null:
		return
	var next_yaw: float = float(_player.get("yaw")) + relative.x * CAMERA_LOOK_SENS
	var next_pitch: float = clampf(
		float(_player.get("pitch")) - relative.y * CAMERA_LOOK_SENS,
		-1.55,
		1.55)
	_player.set("yaw", next_yaw)
	_player.set("pitch", next_pitch)
	# Authoring normally has movement disabled, so update the camera immediately
	# rather than waiting for the player's movement physics tick.
	if _player.has_method("_sync_transform"):
		_player.call("_sync_transform")
	_last_hit.clear()
	_update_preview()


func _set_navigation(enabled: bool) -> void:
	# Navigation now means keyboard movement only. Pointer state never changes and
	# RMB camera look works in both authoring and movement states.
	_navigation_active = enabled
	_camera_look_active = false
	if _player != null:
		_player.set("input_enabled", enabled)
		if _player.has_method("set_mouse_captured"):
			_player.call("set_mouse_captured", false)
	Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
	if enabled:
		_last_hit.clear()
		_update_preview()
	if _live_status_label != null:
		_live_status_label.text = "MOVE ENABLED — WASD • hold RMB to look • TAB returns to authoring" \
			if enabled else _placement_status_text()


func _placement_status_text() -> String:
	match _placement_mode:
		PlacementMode.BIOME:
			return "BIOME PAINT — LMB paint • RMB look • Esc stop • TAB move"
		PlacementMode.LAKE:
			return "LAKE POLYGON — LMB points • RMB look • Esc stop • TAB move"
		PlacementMode.RIVER:
			return "RIVER BÉZIER — LMB knots • RMB look • Esc stop • TAB move"
		PlacementMode.SCULPT_RAISE:
			return "RAISE — LMB sculpt • RMB look • Esc stop • TAB move"
		PlacementMode.SCULPT_LOWER:
			return "LOWER — LMB sculpt • RMB look • Esc stop • TAB move"
		_:
			return "LIVE VIEW — hold RMB to look • TAB toggles movement"


func _is_live_viewport_point(point: Vector2) -> bool:
	var viewport_size := get_viewport_rect().size
	if point.x < 0.0 or point.y < 0.0 or point.x >= viewport_size.x or point.y >= viewport_size.y:
		return false
	if _authoring_panel != null and _authoring_panel.visible:
		return point.x > COMPACT_PANEL_WIDTH_PX + 2.0
	return true


func _refresh_current_category() -> void:
	super._refresh_current_category()
	_rewrite_legacy_navigation_hints()


func _rewrite_legacy_navigation_hints() -> void:
	if _workspace == null:
		return
	_rewrite_label_tree(_workspace)


func _rewrite_label_tree(node: Node) -> void:
	for child: Node in node.get_children():
		if child is Label:
			var label := child as Label
			label.text = label.text.replace("RMB/Esc stops the brush", "Esc stops the brush; hold RMB to look")
			label.text = label.text.replace("RMB/Esc stop", "Esc stop • RMB look")
			label.text = label.text.replace("RMB/Esc cancels", "Esc cancels; RMB looks")
			label.text = label.text.replace("TAB navigate", "TAB move")
		_rewrite_label_tree(child)
