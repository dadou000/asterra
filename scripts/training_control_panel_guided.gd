extends "res://scripts/training_control_panel_easy.gd"
## Final guided shell: keeps emergency stop and results access visible to beginners.

var _easy_stop_button: Button


func _build_guided_steps(root: VBoxContainer) -> void:
	super._build_guided_steps(root)

	var control_row := HBoxContainer.new()
	root.add_child(control_row)
	_easy_stop_button = Button.new()
	_easy_stop_button.text = "STOP active task"
	_easy_stop_button.disabled = true
	_easy_stop_button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_easy_stop_button.tooltip_text = "Stops the current setup/check/training process. Saved checkpoints already written to disk are kept."
	_easy_stop_button.pressed.connect(_on_cancel_pressed)
	control_row.add_child(_easy_stop_button)

	var results_button := Button.new()
	results_button.text = "Open results folder"
	results_button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	results_button.tooltip_text = "Open runs/training and smoke reports in your file browser."
	results_button.pressed.connect(_on_open_folder_pressed)
	control_row.add_child(results_button)


func _set_busy(busy: bool) -> void:
	super._set_busy(busy)
	if _easy_stop_button != null:
		_easy_stop_button.disabled = not busy
