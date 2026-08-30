class_name WorldAuthoringLiveEditorPhase19
extends "res://scripts/world_authoring/world_authoring_editor_live_phase18.gd"
## Phase 19: editor-style camera navigation without a separate movement mode.
##
## Planet Studio owns a dedicated free-fly translation path so editor navigation
## does not inherit the gameplay controller's surface-tangent W direction:
## - W/S move along the actual camera view direction, including pitch;
## - A/D strafe along the camera's right axis;
## - Space/Ctrl move along local planet up/down;
## - Shift keeps the existing altitude-scaled fast movement;
## - RMB rotates the camera while the pointer remains visible/unlocked;
## - LMB remains reserved for the selected authoring tool;
## - movement pauses only while typing into a text/number field.

var _movement_suspended_for_text_input: bool = false
var _editor_navigation_enabled: bool = true


func _ready() -> void:
	super._ready()
	_enable_continuous_viewport_movement()
	_rewrite_movement_hints()


func _process(delta: float) -> void:
	super._process(delta)
	_update_text_input_movement_gate()
	_update_editor_free_fly(delta)


func _unhandled_input(event: InputEvent) -> void:
	# Clicking back into the 3D viewport exits text-entry focus immediately so WASD
	# resumes without requiring an extra click or a mode toggle.
	if event is InputEventMouseButton:
		var button := event as InputEventMouseButton
		if button.pressed and _is_live_viewport_point(button.position):
			get_viewport().gui_release_focus()
			_editor_navigation_enabled = true

	# TAB used to switch between authoring and navigation. Movement is now always
	# available, so keep TAB harmless and explain the current interaction model.
	if event is InputEventKey:
		var key := event as InputEventKey
		if key.pressed and not key.echo and key.keycode == KEY_TAB:
			_enable_continuous_viewport_movement()
			_set_status("Viewport movement is always enabled: W/S follow camera aim • A/D strafe • Space/Ctrl vertical • Shift fast • RMB look.")
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
	_editor_navigation_enabled = true
	Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
	# Disable the ordinary gameplay translation path. Planet Studio applies its own
	# view-aligned movement below, otherwise both controllers would move the camera.
	if _player != null:
		_player.set("input_enabled", false)
		if _player.has_method("set_mouse_captured"):
			_player.call("set_mouse_captured", false)
	_update_text_input_movement_gate()
	if _live_status_label != null:
		_live_status_label.text = _placement_status_text()


func _update_text_input_movement_gate() -> void:
	var focus: Control = get_viewport().gui_get_focus_owner()
	var suspend: bool = focus is LineEdit or focus is TextEdit
	_movement_suspended_for_text_input = suspend
	_editor_navigation_enabled = not suspend
	# Gameplay input remains disabled even while editor navigation is active.
	if _player != null and bool(_player.get("input_enabled")):
		_player.set("input_enabled", false)


func _update_editor_free_fly(delta: float) -> void:
	if not _editor_navigation_enabled or _player == null or _camera == null:
		return
	if Planet.cfg == null or not Planet.ready_state:
		return

	var move := Vector3.ZERO
	var view_forward: Vector3 = _player.call("view_dir") as Vector3
	var camera_right: Vector3 = _camera.global_transform.basis.x.normalized()
	var local_up: Vector3 = _player.call("up_dir") as Vector3
	if Input.is_action_pressed("move_forward"):
		move += view_forward
	if Input.is_action_pressed("move_back"):
		move -= view_forward
	if Input.is_action_pressed("move_right"):
		move += camera_right
	if Input.is_action_pressed("move_left"):
		move -= camera_right
	if Input.is_action_pressed("move_up"):
		move += local_up
	if Input.is_action_pressed("move_down"):
		move -= local_up
	if move.length_squared() <= 1e-8:
		return

	var altitude_agl: float = maxf(float(_player.call("height_above_ground")), 1.0)
	var speed: float = clampf(altitude_agl * 0.55, 8.0, 90000.0)
	if Input.is_action_pressed("sprint"):
		speed *= 6.0

	var world_position: Vec3D = _player.get("world_pos") as Vec3D
	if world_position == null:
		return
	var next_position: Vec3D = world_position.add(
		Vec3D.from_v3(move.normalized()).mul(speed * maxf(delta, 0.0)))
	_player.set("world_pos", next_position)
	Frames.maintain_origin(Frames.to_render(next_position))
	if _player.has_method("_sync_transform"):
		_player.call("_sync_transform")
	_player.emit_signal("moved", next_position)


func _placement_status_text() -> String:
	match _placement_mode:
		PlacementMode.BIOME:
			return "BIOME PAINT — LMB paint • RMB look • W/S toward aim • A/D strafe • Esc stop"
		PlacementMode.LAKE:
			return "LAKE POLYGON — LMB points • RMB look • W/S toward aim • A/D strafe • Esc stop"
		PlacementMode.RIVER:
			return "RIVER BÉZIER — LMB knots • RMB look • W/S toward aim • A/D strafe • Esc stop"
		PlacementMode.SCULPT_RAISE:
			return "RAISE — LMB sculpt • RMB look • W/S toward aim • A/D strafe • Esc stop"
		PlacementMode.SCULPT_LOWER:
			return "LOWER — LMB sculpt • RMB look • W/S toward aim • A/D strafe • Esc stop"
		_:
			return "LIVE VIEW — W/S toward camera aim • A/D strafe • Space/Ctrl vertical • Shift fast • RMB look"


func _rewrite_movement_hints() -> void:
	_rewrite_movement_hints_recursive(self)


func _rewrite_movement_hints_recursive(node: Node) -> void:
	for child: Node in node.get_children():
		if child is Label:
			var label := child as Label
			if "TAB" in label.text or "navigate" in label.text.to_lower():
				label.text = label.text.replace("TAB toggles viewport navigation", "WASD free-fly while RMB looks")
				label.text = label.text.replace("TAB navigate", "WASD free-fly")
				label.text = label.text.replace("TAB: navigate", "WASD free-fly • RMB look")
		_rewrite_movement_hints_recursive(child)
