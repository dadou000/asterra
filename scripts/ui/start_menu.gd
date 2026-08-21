extends Control

const WORLD_SCENE := "res://scenes/Main.tscn"
const CHARACTER_EDITOR_SCENE := "res://scenes/CharacterEditor.tscn"

var _main_menu_center: CenterContainer
var _settings_center: CenterContainer

func _ready() -> void:
	Input.set_mouse_mode(Input.MOUSE_MODE_VISIBLE)
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_build_background()
	_build_menu()
	_build_settings_menu()
	_show_main_menu()

func _build_background() -> void:
	var background := ColorRect.new()
	background.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	background.color = Color("0b1118")
	background.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(background)

	var glow := ColorRect.new()
	glow.set_anchors_preset(Control.PRESET_CENTER)
	glow.position = Vector2(-360.0, -260.0)
	glow.size = Vector2(720.0, 520.0)
	glow.color = Color(0.08, 0.14, 0.19, 0.42)
	glow.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(glow)

func _build_menu() -> void:
	_main_menu_center = CenterContainer.new()
	_main_menu_center.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	add_child(_main_menu_center)

	var column := VBoxContainer.new()
	column.custom_minimum_size = Vector2(470.0, 0.0)
	column.add_theme_constant_override("separation", 12)
	_main_menu_center.add_child(column)

	var title := Label.new()
	title.text = "ASTERRA"
	title.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	title.add_theme_font_size_override("font_size", 52)
	column.add_child(title)

	var subtitle := Label.new()
	subtitle.text = "Development launcher"
	subtitle.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	subtitle.modulate = Color(0.68, 0.74, 0.80)
	subtitle.add_theme_font_size_override("font_size", 17)
	column.add_child(subtitle)

	var spacer := Control.new()
	spacer.custom_minimum_size.y = 22.0
	column.add_child(spacer)

	_add_mode_button(column, "PLAY", "Enter the current Asterra world prototype.", _on_play_pressed)
	_add_mode_button(column, "MAP EDITOR", "Open the world with the terrain-editing workflow selected.", _on_map_editor_pressed)
	_add_mode_button(column, "CHARACTER EDITOR", "Open the studio used to inspect the human rig and facial morphs.", _on_character_editor_pressed)
	_add_mode_button(column, "SETTINGS", "Display, debug and development options.", _show_settings_menu)

	var spacer_bottom := Control.new()
	spacer_bottom.custom_minimum_size.y = 10.0
	column.add_child(spacer_bottom)

	var quit := Button.new()
	quit.text = "Quit"
	quit.custom_minimum_size.y = 42.0
	quit.pressed.connect(func() -> void: get_tree().quit())
	column.add_child(quit)

	var branch_label := Label.new()
	branch_label.text = "character branch"
	branch_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	branch_label.modulate = Color(0.46, 0.53, 0.60)
	branch_label.add_theme_font_size_override("font_size", 13)
	column.add_child(branch_label)

func _build_settings_menu() -> void:
	_settings_center = CenterContainer.new()
	_settings_center.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	add_child(_settings_center)

	var panel := PanelContainer.new()
	panel.custom_minimum_size = Vector2(560.0, 0.0)
	panel.add_theme_stylebox_override("panel", _panel_style())
	_settings_center.add_child(panel)

	var margin := MarginContainer.new()
	margin.add_theme_constant_override("margin_left", 22)
	margin.add_theme_constant_override("margin_right", 22)
	margin.add_theme_constant_override("margin_top", 20)
	margin.add_theme_constant_override("margin_bottom", 20)
	panel.add_child(margin)

	var column := VBoxContainer.new()
	column.add_theme_constant_override("separation", 12)
	margin.add_child(column)

	var title := Label.new()
	title.text = "SETTINGS"
	title.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	title.add_theme_font_size_override("font_size", 32)
	column.add_child(title)

	var subtitle := Label.new()
	subtitle.text = "Development settings are saved in user://settings.cfg"
	subtitle.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	subtitle.modulate = Color(0.62, 0.69, 0.75)
	subtitle.add_theme_font_size_override("font_size", 13)
	column.add_child(subtitle)

	column.add_child(HSeparator.new())

	var debug_title := Label.new()
	debug_title.text = "DEBUG"
	debug_title.modulate = Color(0.58, 0.68, 0.77)
	debug_title.add_theme_font_size_override("font_size", 13)
	column.add_child(debug_title)

	var distance_row := HBoxContainer.new()
	distance_row.add_theme_constant_override("separation", 16)
	column.add_child(distance_row)

	var distance_text := VBoxContainer.new()
	distance_text.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	distance_text.add_theme_constant_override("separation", 3)
	distance_row.add_child(distance_text)

	var distance_label := Label.new()
	distance_label.text = "Distance from closest character"
	distance_label.add_theme_font_size_override("font_size", 17)
	distance_text.add_child(distance_label)

	var distance_hint := Label.new()
	distance_hint.text = "Shows the live camera distance to the nearest registered character. Useful for character scale, LOD and interaction debugging."
	distance_hint.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	distance_hint.modulate = Color(0.62, 0.69, 0.75)
	distance_hint.add_theme_font_size_override("font_size", 13)
	distance_text.add_child(distance_hint)

	var distance_toggle := CheckButton.new()
	distance_toggle.button_pressed = AppSettings.debug_closest_character_distance
	distance_toggle.toggled.connect(func(enabled: bool) -> void:
		AppSettings.set_debug_closest_character_distance(enabled)
	)
	distance_row.add_child(distance_toggle)

	column.add_child(HSeparator.new())

	var lod_title_row := HBoxContainer.new()
	column.add_child(lod_title_row)
	var lod_title := Label.new()
	lod_title.text = "Force brow LOD"
	lod_title.add_theme_font_size_override("font_size", 17)
	lod_title.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	lod_title_row.add_child(lod_title)
	var lod_value := Label.new()
	lod_value.custom_minimum_size.x = 90.0
	lod_value.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	lod_value.text = _brow_lod_label(AppSettings.debug_forced_brow_lod)
	lod_title_row.add_child(lod_value)

	var lod_slider := HSlider.new()
	lod_slider.min_value = -1.0
	lod_slider.max_value = 1.0
	lod_slider.step = 1.0
	lod_slider.value = float(AppSettings.debug_forced_brow_lod)
	lod_slider.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	lod_slider.value_changed.connect(func(value: float) -> void:
		var lod := int(round(value))
		lod_value.text = _brow_lod_label(lod)
		AppSettings.set_debug_forced_brow_lod(lod)
	)
	column.add_child(lod_slider)

	var lod_scale := HBoxContainer.new()
	column.add_child(lod_scale)
	var auto_label := Label.new()
	auto_label.text = "AUTO"
	auto_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	auto_label.modulate = Color(0.53, 0.60, 0.67)
	lod_scale.add_child(auto_label)
	var lod0_label := Label.new()
	lod0_label.text = "LOD0"
	lod0_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	lod0_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	lod0_label.modulate = Color(0.53, 0.60, 0.67)
	lod_scale.add_child(lod0_label)
	var lod1_label := Label.new()
	lod1_label.text = "LOD1"
	lod1_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	lod1_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	lod1_label.modulate = Color(0.53, 0.60, 0.67)
	lod_scale.add_child(lod1_label)

	var lod_hint := Label.new()
	lod_hint.text = "AUTO uses the normal 6 m switch. LOD0 and LOD1 force that brow level regardless of camera distance for visual comparison."
	lod_hint.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	lod_hint.modulate = Color(0.62, 0.69, 0.75)
	lod_hint.add_theme_font_size_override("font_size", 13)
	column.add_child(lod_hint)

	column.add_child(HSeparator.new())

	var registration_note := Label.new()
	registration_note.text = "Character detection: nodes in the 'characters' group, nodes tagged with metadata 'asterra_character', and the current AsterraHuman studio model."
	registration_note.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	registration_note.modulate = Color(0.48, 0.56, 0.63)
	registration_note.add_theme_font_size_override("font_size", 12)
	column.add_child(registration_note)

	var back := Button.new()
	back.text = "Back"
	back.custom_minimum_size.y = 44.0
	back.pressed.connect(_show_main_menu)
	column.add_child(back)

func _brow_lod_label(lod: int) -> String:
	match lod:
		0:
			return "LOD0"
		1:
			return "LOD1"
		_:
			return "AUTO"

func _panel_style() -> StyleBoxFlat:
	var style := StyleBoxFlat.new()
	style.bg_color = Color(0.055, 0.078, 0.105, 0.98)
	style.border_color = Color(0.18, 0.26, 0.33)
	style.set_border_width_all(1)
	style.corner_radius_top_left = 10
	style.corner_radius_top_right = 10
	style.corner_radius_bottom_left = 10
	style.corner_radius_bottom_right = 10
	return style

func _show_main_menu() -> void:
	if _main_menu_center != null:
		_main_menu_center.visible = true
	if _settings_center != null:
		_settings_center.visible = false

func _show_settings_menu() -> void:
	if _main_menu_center != null:
		_main_menu_center.visible = false
	if _settings_center != null:
		_settings_center.visible = true

func _add_mode_button(parent: VBoxContainer, title: String, description: String, callback: Callable) -> void:
	var panel := PanelContainer.new()
	var style := StyleBoxFlat.new()
	style.bg_color = Color(0.07, 0.10, 0.14, 0.96)
	style.border_color = Color(0.18, 0.26, 0.33)
	style.set_border_width_all(1)
	style.corner_radius_top_left = 8
	style.corner_radius_top_right = 8
	style.corner_radius_bottom_left = 8
	style.corner_radius_bottom_right = 8
	style.content_margin_left = 12.0
	style.content_margin_right = 12.0
	style.content_margin_top = 10.0
	style.content_margin_bottom = 10.0
	panel.add_theme_stylebox_override("panel", style)
	parent.add_child(panel)

	var row := VBoxContainer.new()
	row.add_theme_constant_override("separation", 3)
	panel.add_child(row)

	var button := Button.new()
	button.text = title
	button.custom_minimum_size.y = 48.0
	button.add_theme_font_size_override("font_size", 19)
	button.pressed.connect(callback)
	row.add_child(button)

	var hint := Label.new()
	hint.text = description
	hint.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	hint.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	hint.modulate = Color(0.63, 0.69, 0.75)
	hint.add_theme_font_size_override("font_size", 13)
	row.add_child(hint)

func _on_play_pressed() -> void:
	get_tree().set_meta("launch_mode", "play")
	get_tree().change_scene_to_file(WORLD_SCENE)

func _on_map_editor_pressed() -> void:
	get_tree().set_meta("launch_mode", "map_editor")
	get_tree().change_scene_to_file(WORLD_SCENE)

func _on_character_editor_pressed() -> void:
	get_tree().set_meta("launch_mode", "character_editor")
	get_tree().change_scene_to_file(CHARACTER_EDITOR_SCENE)
