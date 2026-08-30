class_name WorldAuthoringLiveEditorPhase19
extends "res://scripts/world_authoring/world_authoring_editor_live_phase18.gd"
## Phase 19: editor-style camera navigation without a separate movement mode.
##
## Planet Studio now behaves like a conventional 3D editor:
## - WASD movement is continuously available while working in the viewport;
## - Space/Ctrl move vertically and Shift keeps the player's existing fast move;
## - RMB rotates the camera while the pointer remains visible/unlocked;
## - LMB remains reserved for the selected authoring tool;
## - keyboard movement is temporarily suspended only while typing into a text field.

var _movement_suspended_for_text_input: bool = false


func _ready() -> void:
	super._ready()
	_enable_continuous_viewport_movement()
	_rewrite_movement_hints()


func _process(delta: float) -> void:
	super._process(delta)
	_update_text_input_movement_gate()


func _unhandled_input(event: InputEvent) -> void:
	# Clicking back into the 3D viewport exits text-entry focus immediately so WASD
	# resumes without requiring an extra click or a mode toggle.
	if event is InputEventMouseButton:
		var button := event as InputEventMouseButton
		if button.pressed and _is_live_viewport_point(button.position):
			get_viewport().gui_release_focus()
			_set_player_movement_enabled(true)

	# TAB used to switch between authoring and navigation. Movement is now always
	# available, so keep TAB harmless and explain the current interaction model.
	if event is InputEventKey:
		var key := event as InputEventKey
		if key.pressed and not key.echo and key.keycode == KEY_TAB:
			_enable_continuous_viewport_movement()
			_set_status("Viewport movement is always enabled: WASD move • Space/Ctrl vertical • Shift fast • hold RMB to look.")
			get_viewport().set_input_as_handled()
			return

	super._unhandled_input(event)


func _set_navigation(_enabled: bool) -> void:
	# Compatibility override for older editor layers that still call this method.
	# There is deliberately no navigation mode anymore: authoring and movement can
	# coexist because mouse-look is explicitly gated by RMB.
	_enable_continuous_viewport_movement()


func _enable_continuous_viewport_movement() -> void:
	_navigation_active = false
	_camera_look_active = false
	Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
	if _player != null and _player.has_method("set_mouse_captured"):
		_player.call("set_mouse_captured", false)
	_update_text_input_movement_gate()
	if _live_status_label != null:
		_live_status_label.text = _placement_status_text()


func _update_text_input_movement_gate() -> void:
	var focus: Control = get_viewport().gui_get_focus_owner()
	var suspend := focus is LineEdit or focus is TextEdit
	if suspend == _movement_suspended_for_text_input:
		# _ready() needs to establish the desired player state even when both values
		# start false, hence still repair a mismatched player flag here.
		if _player != null and bool(_player.get("input_enabled")) != not suspend:
			_set_player_movement_enabled(not suspend)
		return
	_movement_suspended_for_text_input = suspend
	_set_player_movement_enabled(not suspend)


func _set_player_movement_enabled(enabled: bool) -> void:
	if _player == null:
		return
	_player.set("input_enabled", enabled)
	if _player.has_method("set_mouse_captured"):
		_player.call("set_mouse_captured", false)
	Input.mouse_mode = Input.MOUSE_MODE_VISIBLE


func _placement_status_text() -> String:
	match _placement_mode:
		PlacementMode.BIOME:
			return "BIOME PAINT — LMB paint • RMB look • WASD move • Esc stop"
		PlacementMode.LAKE:
			return "LAKE POLYGON — LMB points • RMB look • WASD move • Esc stop"
		PlacementMode.RIVER:
			return "RIVER BÉZIER — LMB knots • RMB look • WASD move • Esc stop"
		PlacementMode.SCULPT_RAISE:
			return "RAISE — LMB sculpt • RMB look • WASD move • Esc stop"
		PlacementMode.SCULPT_LOWER:
			return "LOWER — LMB sculpt • RMB look • WASD move • Esc stop"
		_:
			return "LIVE VIEW — WASD move • Space/Ctrl vertical • Shift fast • hold RMB to look"


func _rewrite_movement_hints() -> void:
	_rewrite_movement_hints_recursive(self)


func _rewrite_movement_hints_recursive(node: Node) -> void:
	for child: Node in node.get_children():
		if child is Label:
			var label := child as Label
			if "TAB" in label.text or "navigate" in label.text.to_lower():
				label.text = label.text.replace("TAB toggles viewport navigation", "WASD moves while RMB looks")
				label.text = label.text.replace("TAB navigate", "WASD move")
				label.text = label.text.replace("TAB: navigate", "WASD move • RMB look")
		_rewrite_movement_hints_recursive(child)
