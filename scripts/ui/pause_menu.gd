class_name PauseMenu
extends CanvasLayer
## Normal in-game pause flow. Escape owns this menu; the developer debug menu
## remains on '&'. Settings mirror the launcher and write through AppSettings.

signal opened
signal closed

const START_MENU_SCENE := "res://scenes/StartMenu.tscn"

var enabled: bool = true
var _root: Control
var _pause_center: CenterContainer
var _settings_center: CenterContainer


func _ready() -> void:
	layer = 30
	process_mode = Node.PROCESS_MODE_ALWAYS
	visible = false
	_build_ui()


func set_enabled(value: bool) -> void:
	enabled = value
	if not enabled and visible:
		resume()


func is_open() -> bool:
	return visible


func _input(event: InputEvent) -> void:
	if not (event is InputEventKey):
		return
	var key := event as InputEventKey
	if not key.pressed or key.echo:
		return

	# Do not let the developer '&' overlay open behind the pause layer.
	if visible and key.unicode == 38:
		get_viewport().set_input_as_handled()
		return
	if key.keycode != KEY_ESCAPE:
		return

	if visible:
		if _settings_center != null and _settings_center.visible:
			_show_pause_menu()
		else:
			resume()
	elif _can_pause():
		open()
	else:
		return
	get_viewport().set_input_as_handled()


func _can_pause() -> bool:
	return enabled and Planet.ready_state and _find_player() != null


func open() -> void:
	if visible or not _can_pause():
		return
	# The debug menu is a separate developer flow. Close it before pausing so there
	# is only one owner of mouse capture and player input.
	var debug_menu := _find_debug_menu()
	if debug_menu != null and debug_menu.visible:
		debug_menu.toggle()

	var player := _find_player()
	if player != null:
		player.set_mouse_captured(false)
		player.input_enabled = false
	visible = true
	_show_pause_menu()
	Input.set_mouse_mode(Input.MOUSE_MODE_VISIBLE)
	opened.emit()
	get_tree().paused = true


func resume() -> void:
	if not visible:
		return
	get_tree().paused = false
	visible = false
	var player := _find_player()
	var debug_menu := _find_debug_menu()
	if player != null:
		var debug_open: bool = debug_menu != null and debug_menu.visible
		player.input_enabled = not debug_open
		if not debug_open:
			player.set_mouse_captured(true)
	closed.emit()


func _build_ui() -> void:
	_root = Control.new()
	_root.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_root.mouse_filter = Control.MOUSE_FILTER_STOP
	add_child(_root)

	var dim := ColorRect.new()
	dim.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	dim.color = Color(0.018, 0.026, 0.038, 0.78)
	dim.mouse_filter = Control.MOUSE_FILTER_STOP
	_root.add_child(dim)

	var shade := ColorRect.new()
	shade.set_anchors_preset(Control.PRESET_CENTER)
	shade.position = Vector2(-380.0, -300.0)
	shade.size = Vector2(760.0, 600.0)
	shade.color = Color(0.055, 0.085, 0.115, 0.44)
	shade.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_root.add_child(shade)

	_build_pause_menu()
	_build_settings_menu()


func _build_pause_menu() -> void:
	_pause_center = CenterContainer.new()
	_pause_center.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_root.add_child(_pause_center)

	var panel := PanelContainer.new()
	panel.custom_minimum_size = Vector2(470.0, 0.0)
	panel.add_theme_stylebox_override("panel", _panel_style())
	_pause_center.add_child(panel)

	var margin := MarginContainer.new()
	margin.add_theme_constant_override("margin_left", 24)
	margin.add_theme_constant_override("margin_right", 24)
	margin.add_theme_constant_override("margin_top", 24)
	margin.add_theme_constant_override("margin_bottom", 24)
	panel.add_child(margin)

	var column := VBoxContainer.new()
	column.add_theme_constant_override("separation", 12)
	margin.add_child(column)

	var title := Label.new()
	title.text = "ASTERRA"
	title.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	title.add_theme_font_size_override("font_size", 42)
	column.add_child(title)

	var subtitle := Label.new()
	subtitle.text = "PAUSED"
	subtitle.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	subtitle.modulate = Color(0.62, 0.69, 0.75)
	subtitle.add_theme_font_size_override("font_size", 14)
	column.add_child(subtitle)

	var spacer := Control.new()
	spacer.custom_minimum_size.y = 12.0
	column.add_child(spacer)

	_add_button(column, "RESUME", resume)
	_add_button(column, "SETTINGS", _show_settings_menu)

	column.add_child(HSeparator.new())

	var main_menu := Button.new()
	main_menu.text = "Return to main menu"
	main_menu.custom_minimum_size.y = 42.0
	main_menu.pressed.connect(_return_to_main_menu)
	column.add_child(main_menu)

	var quit := Button.new()
	quit.text = "Quit"
	quit.custom_minimum_size.y = 42.0
	quit.pressed.connect(_quit)
	column.add_child(quit)

	var hint := Label.new()
	hint.text = "Esc resumes  •  & opens developer debug while playing"
	hint.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	hint.modulate = Color(0.46, 0.53, 0.60)
	hint.add_theme_font_size_override("font_size", 12)
	column.add_child(hint)


func _build_settings_menu() -> void:
	_settings_center = CenterContainer.new()
	_settings_center.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_root.add_child(_settings_center)

	var scroll := ScrollContainer.new()
	scroll.custom_minimum_size = Vector2(620.0, 0.0)
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_settings_center.add_child(scroll)

	var panel := PanelContainer.new()
	panel.custom_minimum_size = Vector2(590.0, 0.0)
	panel.add_theme_stylebox_override("panel", _panel_style())
	scroll.add_child(panel)

	var margin := MarginContainer.new()
	margin.add_theme_constant_override("margin_left", 24)
	margin.add_theme_constant_override("margin_right", 24)
	margin.add_theme_constant_override("margin_top", 20)
	margin.add_theme_constant_override("margin_bottom", 20)
	panel.add_child(margin)

	var column := VBoxContainer.new()
	column.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	column.add_theme_constant_override("separation", 12)
	margin.add_child(column)

	var title := Label.new()
	title.text = "SETTINGS"
	title.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	title.add_theme_font_size_override("font_size", 32)
	column.add_child(title)

	var subtitle := Label.new()
	subtitle.text = "Changes are saved to user://settings.cfg"
	subtitle.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	subtitle.modulate = Color(0.62, 0.69, 0.75)
	subtitle.add_theme_font_size_override("font_size", 13)
	column.add_child(subtitle)

	column.add_child(HSeparator.new())
	column.add_child(_section_title("GRAPHICS"))

	var graphics_row := HBoxContainer.new()
	graphics_row.add_theme_constant_override("separation", 16)
	column.add_child(graphics_row)

	var graphics_text := VBoxContainer.new()
	graphics_text.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	graphics_text.add_theme_constant_override("separation", 3)
	graphics_row.add_child(graphics_text)

	var graphics_label := Label.new()
	graphics_label.text = "Rendering preset"
	graphics_label.add_theme_font_size_override("font_size", 17)
	graphics_text.add_child(graphics_label)

	var graphics_hint := Label.new()
	graphics_hint.text = GraphicsQuality.preset_description(AppSettings.graphics_quality)
	graphics_hint.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	graphics_hint.modulate = Color(0.62, 0.69, 0.75)
	graphics_hint.add_theme_font_size_override("font_size", 13)
	graphics_text.add_child(graphics_hint)

	var graphics_preset := OptionButton.new()
	graphics_preset.custom_minimum_size = Vector2(150.0, 42.0)
	for preset: int in range(GraphicsQuality.Preset.PERFORMANCE, GraphicsQuality.Preset.ULTRA + 1):
		graphics_preset.add_item(GraphicsQuality.preset_name(preset), preset)
	graphics_preset.select(AppSettings.graphics_quality)
	graphics_preset.item_selected.connect(func(index: int) -> void:
		var preset: int = graphics_preset.get_item_id(index)
		AppSettings.set_graphics_quality(preset)
		graphics_hint.text = GraphicsQuality.preset_description(preset)
		_apply_graphics_quality(preset)
	)
	graphics_row.add_child(graphics_preset)

	var live_note := Label.new()
	live_note.text = "Applied live to render scale, GI, screen-space effects, shadows and cloud quality."
	live_note.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	live_note.modulate = Color(0.48, 0.56, 0.63)
	live_note.add_theme_font_size_override("font_size", 12)
	column.add_child(live_note)

	column.add_child(HSeparator.new())
	column.add_child(_section_title("DEBUG"))

	var distance_row := HBoxContainer.new()
	distance_row.add_theme_constant_override("separation", 16)
	column.add_child(distance_row)
	var distance_text := VBoxContainer.new()
	distance_text.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	distance_row.add_child(distance_text)
	var distance_label := Label.new()
	distance_label.text = "Distance from closest character"
	distance_label.add_theme_font_size_override("font_size", 17)
	distance_text.add_child(distance_label)
	var distance_hint := Label.new()
	distance_hint.text = "Shows the live camera distance to the nearest registered character."
	distance_hint.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	distance_hint.modulate = Color(0.62, 0.69, 0.75)
	distance_hint.add_theme_font_size_override("font_size", 13)
	distance_text.add_child(distance_hint)
	var distance_toggle := CheckButton.new()
	distance_toggle.button_pressed = AppSettings.debug_closest_character_distance
	distance_toggle.toggled.connect(func(value: bool) -> void:
		AppSettings.set_debug_closest_character_distance(value)
	)
	distance_row.add_child(distance_toggle)

	column.add_child(HSeparator.new())

	var brow_row := HBoxContainer.new()
	brow_row.add_theme_constant_override("separation", 16)
	column.add_child(brow_row)
	var brow_text := VBoxContainer.new()
	brow_text.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	brow_row.add_child(brow_text)
	var brow_label := Label.new()
	brow_label.text = "Brow LOD debug"
	brow_label.add_theme_font_size_override("font_size", 17)
	brow_text.add_child(brow_label)
	var brow_hint := Label.new()
	brow_hint.text = "Enable manual brow LOD testing and force the active level."
	brow_hint.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	brow_hint.modulate = Color(0.62, 0.69, 0.75)
	brow_hint.add_theme_font_size_override("font_size", 13)
	brow_text.add_child(brow_hint)
	var brow_toggle := CheckButton.new()
	brow_toggle.button_pressed = AppSettings.debug_brow_lod_controls
	brow_row.add_child(brow_toggle)

	var lod_controls := VBoxContainer.new()
	lod_controls.visible = AppSettings.debug_brow_lod_controls
	lod_controls.add_theme_constant_override("separation", 7)
	column.add_child(lod_controls)

	var lod_header := HBoxContainer.new()
	lod_controls.add_child(lod_header)
	var lod_label := Label.new()
	lod_label.text = "Force brow LOD"
	lod_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	lod_label.add_theme_font_size_override("font_size", 16)
	lod_header.add_child(lod_label)
	var lod_value := Label.new()
	lod_value.custom_minimum_size.x = 90.0
	lod_value.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	lod_value.text = _brow_lod_label(AppSettings.debug_forced_brow_lod)
	lod_header.add_child(lod_value)

	var lod_slider := HSlider.new()
	lod_slider.min_value = -1.0
	lod_slider.max_value = 2.0
	lod_slider.step = 1.0
	lod_slider.value = float(AppSettings.debug_forced_brow_lod)
	lod_slider.value_changed.connect(func(value: float) -> void:
		var lod: int = int(round(value))
		lod_value.text = _brow_lod_label(lod)
		AppSettings.set_debug_forced_brow_lod(lod)
	)
	lod_controls.add_child(lod_slider)

	brow_toggle.toggled.connect(func(value: bool) -> void:
		AppSettings.set_debug_brow_lod_controls(value)
		lod_controls.visible = value
		if not value:
			lod_slider.set_value_no_signal(-1.0)
			lod_value.text = "AUTO"
	)

	column.add_child(HSeparator.new())
	var back := Button.new()
	back.text = "Back"
	back.custom_minimum_size.y = 44.0
	back.pressed.connect(_show_pause_menu)
	column.add_child(back)


func _apply_graphics_quality(preset: int) -> void:
	var quality: int = GraphicsQuality.sanitize(preset)
	GraphicsQuality.configure_viewport(get_viewport(), quality)
	var world_environment: WorldEnvironment = null
	var parent := get_parent()
	if parent != null:
		for child: Node in parent.get_children():
			if child is WorldEnvironment:
				world_environment = child as WorldEnvironment
				if world_environment.environment != null:
					GraphicsQuality.configure_world_environment(
						world_environment.environment, quality)
			elif child is DirectionalLight3D:
				var light := child as DirectionalLight3D
				GraphicsQuality.configure_sun(light, quality)
				# Preserve Helion's physical apparent disc after the quality helper
				# changes the generic soft-shadow preset.
				light.light_angular_distance = Frames.helion_angular_diameter_deg()

	# Cloud ray steps and cloud-shadow sampling are quality-dependent too. Reusing
	# the controller's existing configure path changes only parameters/resources;
	# it does not regenerate the resident terrain.
	if world_environment != null and world_environment.environment != null:
		var sky := world_environment.environment.sky
		if sky != null and sky.sky_material is ShaderMaterial and Planet.cfg != null:
			VolumetricClouds.configure(
				sky.sky_material as ShaderMaterial,
				Planet.cfg.world_seed,
				quality)


func _find_player() -> AsterraPlayer:
	var parent := get_parent()
	if parent == null:
		return null
	for child: Node in parent.get_children():
		if child is AsterraPlayer:
			return child as AsterraPlayer
	return null


func _find_debug_menu() -> DebugMenu:
	var parent := get_parent()
	if parent == null:
		return null
	for child: Node in parent.get_children():
		if child is DebugMenu:
			return child as DebugMenu
	return null


func _show_pause_menu() -> void:
	if _pause_center != null:
		_pause_center.visible = true
	if _settings_center != null:
		_settings_center.visible = false


func _show_settings_menu() -> void:
	if _pause_center != null:
		_pause_center.visible = false
	if _settings_center != null:
		_settings_center.visible = true


func _return_to_main_menu() -> void:
	get_tree().paused = false
	visible = false
	get_tree().change_scene_to_file(START_MENU_SCENE)


func _quit() -> void:
	get_tree().paused = false
	get_tree().quit()


func _add_button(parent: VBoxContainer, text: String, callback: Callable) -> void:
	var button := Button.new()
	button.text = text
	button.custom_minimum_size.y = 48.0
	button.add_theme_font_size_override("font_size", 18)
	button.pressed.connect(callback)
	parent.add_child(button)


func _section_title(text: String) -> Label:
	var label := Label.new()
	label.text = text
	label.modulate = Color(0.58, 0.68, 0.77)
	label.add_theme_font_size_override("font_size", 13)
	return label


func _panel_style() -> StyleBoxFlat:
	var style := StyleBoxFlat.new()
	style.bg_color = Color(0.055, 0.078, 0.105, 0.985)
	style.border_color = Color(0.18, 0.26, 0.33)
	style.set_border_width_all(1)
	style.set_corner_radius_all(10)
	return style


func _brow_lod_label(lod: int) -> String:
	match lod:
		0:
			return "LOD0"
		1:
			return "LOD1"
		2:
			return "LOD2"
		_:
			return "AUTO"
