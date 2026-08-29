extends "res://scripts/ui/start_menu.gd"
## Thin launcher extension that keeps the existing development menu intact while
## exposing Planet Studio as a live-world authoring workflow.

const MAIN_SCENE := "res://scenes/Main.tscn"

func _build_menu() -> void:
	super._build_menu()
	if _main_menu_center == null or _main_menu_center.get_child_count() == 0:
		return
	var column := _main_menu_center.get_child(0) as VBoxContainer
	if column == null:
		return
	var appended_index := column.get_child_count()
	_add_mode_button(
		column,
		"PLANET STUDIO",
		"Author the live planet, terrain, water, atmosphere and celestial system with staged Apply/Undo/Presets.",
		_on_planet_studio_pressed
	)
	if appended_index < column.get_child_count():
		var planet_studio_panel := column.get_child(appended_index)
		# Place it immediately after Map Editor and before Character Editor.
		column.move_child(planet_studio_panel, mini(5, column.get_child_count() - 1))
	for child: Node in column.get_children():
		if child is Label and (child as Label).text.begins_with("Asterra 0.0."):
			(child as Label).text = "Asterra 0.0.5 development"

func _on_planet_studio_pressed() -> void:
	get_tree().set_meta("launch_mode", "planet_studio")
	get_tree().change_scene_to_file(MAIN_SCENE)
