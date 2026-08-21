extends Control

const WORLD_SCENE := "res://scenes/Main.tscn"
const CHARACTER_EDITOR_SCENE := "res://scenes/CharacterEditor.tscn"

func _ready() -> void:
	Input.set_mouse_mode(Input.MOUSE_MODE_VISIBLE)
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_build_background()
	_build_menu()

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
	var center := CenterContainer.new()
	center.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	add_child(center)

	var column := VBoxContainer.new()
	column.custom_minimum_size = Vector2(470.0, 0.0)
	column.add_theme_constant_override("separation", 12)
	center.add_child(column)

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

	_add_mode_button(
		column,
		"PLAY",
		"Enter the current Asterra world prototype.",
		_on_play_pressed
	)
	_add_mode_button(
		column,
		"MAP EDITOR",
		"Open the world with the terrain-editing workflow selected.",
		_on_map_editor_pressed
	)
	_add_mode_button(
		column,
		"CHARACTER EDITOR",
		"Open the studio used to inspect the human rig and facial morphs.",
		_on_character_editor_pressed
	)

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
