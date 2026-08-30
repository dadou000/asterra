extends CanvasLayer
## Small beginner-facing model control for the viewport ragdoll.

const PANEL_WIDTH: float = 430.0
const REFRESH_INTERVAL: float = 0.20
const TRAINING_TOPOLOGY_SCENE: String = "res://scenes/ragdoll_test.tscn"
const COMPACT_TOPOLOGY_SCENE: String = "res://scenes/ragdoll_compact.tscn"

var _ragdoll: Node
var _panel: PanelContainer
var _model_label: Label
var _status_label: Label
var _detail_label: Label
var _latency_label: Label
var _topology_label: Label
var _topology_button: Button
var _activate_button: Button
var _latest_button: Button
var _browse_button: Button
var _unload_button: Button
var _file_dialog: FileDialog
var _refresh_accumulator: float = 0.0


func _ready() -> void:
	layer = 35
	_ragdoll = get_parent()
	_build_ui()
	if _ragdoll != null and _ragdoll.has_signal("policy_state_changed"):
		_ragdoll.connect("policy_state_changed", Callable(self, "_refresh"))
	_refresh()


func _process(delta: float) -> void:
	_refresh_accumulator += delta
	if _refresh_accumulator < REFRESH_INTERVAL:
		return
	_refresh_accumulator = 0.0
	_refresh()


func _build_ui() -> void:
	_panel = PanelContainer.new()
	_panel.name = "ViewportPolicyPanel"
	_panel.anchor_top = 1.0
	_panel.anchor_bottom = 1.0
	_panel.offset_left = 14.0
	_panel.offset_right = 14.0 + PANEL_WIDTH
	_panel.offset_top = -282.0
	_panel.offset_bottom = -14.0
	_panel.mouse_filter = Control.MOUSE_FILTER_STOP
	add_child(_panel)

	var margin := MarginContainer.new()
	for side: String in ["left", "right", "top", "bottom"]:
		margin.add_theme_constant_override("margin_%s" % side, 12)
	_panel.add_child(margin)

	var root := VBoxContainer.new()
	root.add_theme_constant_override("separation", 6)
	margin.add_child(root)

	var title := Label.new()
	title.text = "VIEWPORT MODEL"
	title.add_theme_font_size_override("font_size", 17)
	root.add_child(title)

	var explanation := Label.new()
	explanation.text = "Load a trained checkpoint, then activate it on this physical ragdoll. The model requests joint targets; Jolt still enforces physics, ROM and torque."
	explanation.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	root.add_child(explanation)

	var topology_row := HBoxContainer.new()
	root.add_child(topology_row)
	_topology_label = Label.new()
	_topology_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_topology_label.text = "Skeleton: checking..."
	topology_row.add_child(_topology_label)
	_topology_button = Button.new()
	_topology_button.text = "Switch skeleton"
	_topology_button.pressed.connect(_on_topology_pressed)
	topology_row.add_child(_topology_button)

	_model_label = Label.new()
	_model_label.text = "Model: none"
	_model_label.text_overrun_behavior = TextServer.OVERRUN_TRIM_ELLIPSIS
	root.add_child(_model_label)

	var load_row := HBoxContainer.new()
	root.add_child(load_row)
	_latest_button = Button.new()
	_latest_button.text = "Use latest trained"
	_latest_button.tooltip_text = "Find the newest model_*.pt checkpoint under runs/training/stand."
	_latest_button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_latest_button.pressed.connect(_on_latest_pressed)
	load_row.add_child(_latest_button)
	_browse_button = Button.new()
	_browse_button.text = "Browse .pt..."
	_browse_button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_browse_button.pressed.connect(_on_browse_pressed)
	load_row.add_child(_browse_button)

	var action_row := HBoxContainer.new()
	root.add_child(action_row)
	_activate_button = Button.new()
	_activate_button.text = "ACTIVATE"
	_activate_button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_activate_button.disabled = true
	_activate_button.pressed.connect(_on_activate_pressed)
	action_row.add_child(_activate_button)
	_unload_button = Button.new()
	_unload_button.text = "Unload"
	_unload_button.disabled = true
	_unload_button.pressed.connect(_on_unload_pressed)
	action_row.add_child(_unload_button)

	_status_label = Label.new()
	_status_label.text = "PASSIVE"
	_status_label.add_theme_font_size_override("font_size", 15)
	root.add_child(_status_label)

	_detail_label = Label.new()
	_detail_label.text = "No model loaded."
	_detail_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	root.add_child(_detail_label)

	_latency_label = Label.new()
	_latency_label.text = "Inference: --"
	root.add_child(_latency_label)

	_file_dialog = FileDialog.new()
	_file_dialog.title = "Choose trained humanoid checkpoint"
	_file_dialog.access = FileDialog.ACCESS_FILESYSTEM
	_file_dialog.file_mode = FileDialog.FILE_MODE_OPEN_FILE
	_file_dialog.filters = PackedStringArray(["*.pt ; RSL-RL checkpoint"])
	_file_dialog.file_selected.connect(_on_file_selected)
	add_child(_file_dialog)


func _on_latest_pressed() -> void:
	var checkpoint: String = _find_latest_checkpoint()
	if checkpoint.is_empty():
		_status_label.text = "NO CHECKPOINT FOUND"
		_detail_label.text = "Train a model first, or use Browse .pt to select one manually."
		return
	_load_checkpoint(checkpoint)


func _on_browse_pressed() -> void:
	var stand_runs: String = _stand_runs_root()
	if DirAccess.dir_exists_absolute(stand_runs):
		_file_dialog.current_dir = stand_runs
	_file_dialog.popup_centered_ratio(0.72)


func _on_file_selected(path: String) -> void:
	_load_checkpoint(path)


func _load_checkpoint(path: String) -> void:
	if _ragdoll == null or not _ragdoll.has_method("load_policy_checkpoint"):
		_status_label.text = "RUNTIME MISSING"
		_detail_label.text = "This scene is not using the policy-enabled ragdoll script."
		return
	_ragdoll.call("load_policy_checkpoint", path)
	_refresh()


func _on_activate_pressed() -> void:
	if _ragdoll == null:
		return
	var data: Dictionary = _policy_state()
	if bool(data.get("active", false)):
		_ragdoll.call("deactivate_policy")
	else:
		_ragdoll.call("activate_loaded_policy")
	_refresh()


func _on_unload_pressed() -> void:
	if _ragdoll != null and _ragdoll.has_method("unload_policy"):
		_ragdoll.call("unload_policy")
	_refresh()


func _on_topology_pressed() -> void:
	if _ragdoll != null and _ragdoll.has_method("unload_policy"):
		_ragdoll.call("unload_policy")
	var target_scene: String = COMPACT_TOPOLOGY_SCENE if _skeleton_mode() == "training" else TRAINING_TOPOLOGY_SCENE
	var error: Error = get_tree().change_scene_to_file(target_scene)
	if error != OK:
		_status_label.text = "SCENE SWITCH FAILED"
		_detail_label.text = "Could not open %s (error %d)." % [target_scene, error]


func _refresh() -> void:
	if _ragdoll == null or not _ragdoll.has_method("get_policy_ui_state"):
		return
	var data: Dictionary = _policy_state()
	var state: String = String(data.get("state", "passive"))
	var detail: String = String(data.get("detail", ""))
	var loaded: bool = bool(data.get("loaded", false))
	var active: bool = bool(data.get("active", false))
	var checkpoint: String = String(data.get("checkpoint", ""))
	var iteration: int = int(data.get("model_iteration", -1))
	var latency: float = float(data.get("latency_ms", 0.0))
	var topology: String = _skeleton_mode()

	if topology == "training":
		_topology_label.text = "Skeleton: TRAINING 55 links / 54 DOF"
		_topology_button.text = "Compare compact"
		_topology_button.tooltip_text = "Switch to the older 19-body / 18x6DOF approximation."
	else:
		_topology_label.text = "Skeleton: COMPACT 19 / 18x6DOF"
		_topology_button.text = "Use training skeleton"
		_topology_button.tooltip_text = "Switch to the 19 physical + 36 virtual link / 54 serial-DOF training topology."

	if checkpoint.is_empty():
		_model_label.text = "Model: none"
	elif iteration >= 0:
		_model_label.text = "Model: %s  •  update %d" % [checkpoint.get_file(), iteration + 1]
	else:
		_model_label.text = "Model: %s" % checkpoint.get_file()
	_model_label.tooltip_text = checkpoint

	_status_label.text = "NEURAL CONTROL ACTIVE" if active else state.to_upper()
	_detail_label.text = detail
	_latency_label.text = "Inference: %.2f ms  •  policy 60 Hz" % latency if loaded else "Inference: --"

	_activate_button.disabled = not loaded
	_activate_button.text = "DEACTIVATE → PASSIVE" if active else "ACTIVATE ON RAGDOLL"
	_unload_button.disabled = checkpoint.is_empty()
	_latest_button.disabled = state == "loading"
	_browse_button.disabled = state == "loading"
	_topology_button.disabled = state == "loading"


func _policy_state() -> Dictionary:
	var value: Variant = _ragdoll.call("get_policy_ui_state")
	if value is Dictionary:
		return value as Dictionary
	return {}


func _skeleton_mode() -> String:
	if _ragdoll != null and _ragdoll.has_method("get_skeleton_mode"):
		return String(_ragdoll.call("get_skeleton_mode"))
	return "compact"


func _stand_runs_root() -> String:
	var repo_root: String = ProjectSettings.globalize_path("res://").trim_suffix("/").trim_suffix("\\")
	return repo_root.path_join("experiments").path_join("locomotion_19body").path_join("runs").path_join("training").path_join("stand")


func _find_latest_checkpoint() -> String:
	var root: String = _stand_runs_root()
	if not DirAccess.dir_exists_absolute(root):
		return ""
	var run_names: PackedStringArray = DirAccess.get_directories_at(root)
	run_names.sort()
	for reverse_index: int in range(run_names.size() - 1, -1, -1):
		var run_path: String = root.path_join(run_names[reverse_index])
		var files: PackedStringArray = DirAccess.get_files_at(run_path)
		var best_path: String = ""
		var best_iteration: int = -1
		for file_name: String in files:
			if not file_name.begins_with("model_") or not file_name.ends_with(".pt"):
				continue
			var number_text: String = file_name.trim_prefix("model_").trim_suffix(".pt")
			if not number_text.is_valid_int():
				continue
			var iteration: int = number_text.to_int()
			if iteration > best_iteration:
				best_iteration = iteration
				best_path = run_path.path_join(file_name)
		if not best_path.is_empty():
			return best_path
	return ""
