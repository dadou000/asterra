extends CanvasLayer
## In-game controller for the repo-local humanoid training pipeline.
##
## The panel never runs Isaac/Python work on Godot's main thread. It launches a
## small standard-library bridge process, then polls UTF-8 status/log files while
## the Jolt ragdoll remains fully interactive.

const PANEL_WIDTH: float = 452.0
const POLL_INTERVAL: float = 0.25
const LOG_CHAR_LIMIT: int = 18000

var _repo_root: String = ""
var _experiment_root: String = ""
var _control_dir: String = ""
var _status_path: String = ""
var _log_path: String = ""
var _cancel_path: String = ""
var _bridge_path: String = ""
var _setup_script_path: String = ""

var _panel: PanelContainer
var _status_label: Label
var _detail_label: Label
var _python_path: LineEdit
var _device_edit: LineEdit
var _env_count: SpinBox
var _seconds: SpinBox
var _seed: SpinBox
var _noise_deg: SpinBox
var _mode: OptionButton
var _headless: CheckButton
var _strict_contact: CheckButton
var _log_view: TextEdit
var _task_buttons: Array[Button] = []
var _cancel_button: Button

var _active_pid: int = 0
var _active_task: String = ""
var _setup_process: bool = false
var _poll_accumulator: float = 0.0
var _last_status_text: String = ""
var _last_log_text: String = ""


func _ready() -> void:
	layer = 40
	_repo_root = ProjectSettings.globalize_path("res://").trim_suffix("/").trim_suffix("\\")
	_experiment_root = _repo_root.path_join("experiments").path_join("locomotion_19body")
	_control_dir = _experiment_root.path_join("runs").path_join("control")
	_status_path = _control_dir.path_join("game_bridge_status.json")
	_log_path = _control_dir.path_join("game_bridge.log")
	_cancel_path = _control_dir.path_join("cancel.request")
	_bridge_path = _experiment_root.path_join("training").path_join("scripts").path_join("game_training_bridge.py")
	_setup_script_path = _experiment_root.path_join("training").path_join("setup_isaac_windows.ps1")
	DirAccess.make_dir_recursive_absolute(_control_dir)
	_build_ui()
	_refresh_environment_status()
	set_process(true)


func _process(delta: float) -> void:
	_poll_accumulator += delta
	if _poll_accumulator < POLL_INTERVAL:
		return
	_poll_accumulator = 0.0

	_refresh_log()
	if _active_pid <= 0:
		return

	if _setup_process:
		if not OS.is_process_running(_active_pid):
			_finish_setup_process()
		return

	_refresh_bridge_status()
	if not OS.is_process_running(_active_pid):
		_finish_bridge_process()


func _unhandled_key_input(event: InputEvent) -> void:
	if not (event is InputEventKey):
		return
	var key_event: InputEventKey = event as InputEventKey
	if key_event.pressed and not key_event.echo and key_event.keycode == KEY_F3:
		_panel.visible = not _panel.visible
		get_viewport().set_input_as_handled()


func _build_ui() -> void:
	_panel = PanelContainer.new()
	_panel.name = "HumanoidTrainingConsole"
	_panel.anchor_left = 1.0
	_panel.anchor_right = 1.0
	_panel.anchor_top = 0.0
	_panel.anchor_bottom = 1.0
	_panel.offset_left = -PANEL_WIDTH - 12.0
	_panel.offset_right = -12.0
	_panel.offset_top = 12.0
	_panel.offset_bottom = -12.0
	_panel.mouse_filter = Control.MOUSE_FILTER_STOP
	add_child(_panel)

	var margin: MarginContainer = MarginContainer.new()
	margin.add_theme_constant_override("margin_left", 12)
	margin.add_theme_constant_override("margin_right", 12)
	margin.add_theme_constant_override("margin_top", 10)
	margin.add_theme_constant_override("margin_bottom", 10)
	_panel.add_child(margin)

	var root: VBoxContainer = VBoxContainer.new()
	root.add_theme_constant_override("separation", 6)
	margin.add_child(root)

	var title_row: HBoxContainer = HBoxContainer.new()
	root.add_child(title_row)
	var title: Label = Label.new()
	title.text = "HUMANOID TRAINING CONTROL"
	title.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	title.add_theme_font_size_override("font_size", 18)
	title_row.add_child(title)
	var hide_button: Button = Button.new()
	hide_button.text = "Hide [F3]"
	hide_button.pressed.connect(_on_hide_pressed)
	title_row.add_child(hide_button)

	_status_label = Label.new()
	_status_label.text = "IDLE"
	_status_label.add_theme_font_size_override("font_size", 14)
	root.add_child(_status_label)

	_detail_label = Label.new()
	_detail_label.text = "Checking training environment..."
	_detail_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	root.add_child(_detail_label)

	root.add_child(HSeparator.new())
	_add_section_label(root, "Runtime")

	_python_path = _add_line_row(root, "Python", _default_training_python())
	_device_edit = _add_line_row(root, "Device", "cuda:0")

	root.add_child(HSeparator.new())
	_add_section_label(root, "PhysX smoke parameters")

	_env_count = _add_spin_row(root, "Environments", 1.0, 4096.0, 256.0, 1.0, false)
	_seconds = _add_spin_row(root, "Seconds", 0.1, 30.0, 2.0, 0.1, true)
	_seed = _add_spin_row(root, "Seed", 0.0, 2147483647.0, 1467.0, 1.0, false)
	_noise_deg = _add_spin_row(root, "Joint noise °", 0.0, 45.0, 0.0, 0.25, true)

	var mode_row: HBoxContainer = HBoxContainer.new()
	root.add_child(mode_row)
	var mode_label: Label = Label.new()
	mode_label.text = "Mode"
	mode_label.custom_minimum_size.x = 128.0
	mode_row.add_child(mode_label)
	_mode = OptionButton.new()
	_mode.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_mode.add_item("Hold / neutral PD", 0)
	_mode.add_item("Passive fall", 1)
	mode_row.add_child(_mode)

	var check_row: HBoxContainer = HBoxContainer.new()
	root.add_child(check_row)
	_headless = CheckButton.new()
	_headless.text = "Headless"
	_headless.button_pressed = true
	check_row.add_child(_headless)
	_strict_contact = CheckButton.new()
	_strict_contact.text = "Strict foot contact"
	_strict_contact.button_pressed = true
	check_row.add_child(_strict_contact)

	root.add_child(HSeparator.new())
	_add_section_label(root, "Pipeline")

	var setup_row: HBoxContainer = HBoxContainer.new()
	root.add_child(setup_row)
	_add_task_button(setup_row, "Setup Isaac", _on_setup_pressed)
	_add_task_button(setup_row, "Check stack", _on_check_stack_pressed)

	var build_row: HBoxContainer = HBoxContainer.new()
	root.add_child(build_row)
	_add_task_button(build_row, "Build articulation", _on_build_pressed)
	_add_task_button(build_row, "Run tests", _on_tests_pressed)

	var sim_row: HBoxContainer = HBoxContainer.new()
	root.add_child(sim_row)
	_add_task_button(sim_row, "Convert USD", _on_convert_pressed)
	_add_task_button(sim_row, "Smoke selected", _on_smoke_pressed)

	var action_row: HBoxContainer = HBoxContainer.new()
	root.add_child(action_row)
	var preflight: Button = _add_task_button(action_row, "FULL PREFLIGHT", _on_preflight_pressed)
	preflight.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_cancel_button = Button.new()
	_cancel_button.text = "Cancel"
	_cancel_button.disabled = true
	_cancel_button.pressed.connect(_on_cancel_pressed)
	action_row.add_child(_cancel_button)

	var folder_row: HBoxContainer = HBoxContainer.new()
	root.add_child(folder_row)
	var open_folder: Button = Button.new()
	open_folder.text = "Open run folder"
	open_folder.pressed.connect(_on_open_folder_pressed)
	folder_row.add_child(open_folder)
	var refresh_button: Button = Button.new()
	refresh_button.text = "Refresh status"
	refresh_button.pressed.connect(_on_refresh_pressed)
	folder_row.add_child(refresh_button)

	root.add_child(HSeparator.new())
	_add_section_label(root, "Process log")
	_log_view = TextEdit.new()
	_log_view.editable = false
	_log_view.custom_minimum_size.y = 180.0
	_log_view.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_log_view.wrap_mode = TextEdit.LINE_WRAPPING_BOUNDARY
	root.add_child(_log_view)


func _add_section_label(parent: VBoxContainer, text: String) -> void:
	var label: Label = Label.new()
	label.text = text
	label.add_theme_font_size_override("font_size", 14)
	parent.add_child(label)


func _add_line_row(parent: VBoxContainer, label_text: String, initial: String) -> LineEdit:
	var row: HBoxContainer = HBoxContainer.new()
	parent.add_child(row)
	var label: Label = Label.new()
	label.text = label_text
	label.custom_minimum_size.x = 128.0
	row.add_child(label)
	var edit: LineEdit = LineEdit.new()
	edit.text = initial
	edit.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(edit)
	return edit


func _add_spin_row(
	parent: VBoxContainer,
	label_text: String,
	minimum: float,
	maximum: float,
	initial: float,
	step: float,
	allow_decimals: bool
) -> SpinBox:
	var row: HBoxContainer = HBoxContainer.new()
	parent.add_child(row)
	var label: Label = Label.new()
	label.text = label_text
	label.custom_minimum_size.x = 128.0
	row.add_child(label)
	var spin: SpinBox = SpinBox.new()
	spin.min_value = minimum
	spin.max_value = maximum
	spin.step = step
	spin.value = initial
	spin.allow_lesser = false
	spin.allow_greater = false
	spin.update_on_text_changed = true
	spin.custom_arrow_step = step if allow_decimals else 1.0
	spin.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(spin)
	return spin


func _add_task_button(parent: HBoxContainer, text: String, callback: Callable) -> Button:
	var button: Button = Button.new()
	button.text = text
	button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	button.pressed.connect(callback)
	parent.add_child(button)
	_task_buttons.append(button)
	return button


func _default_training_python() -> String:
	var windows_python: String = _repo_root.path_join(".venv-isaac").path_join("Scripts").path_join("python.exe")
	var unix_python: String = _repo_root.path_join(".venv-isaac").path_join("bin").path_join("python")
	if FileAccess.file_exists(windows_python):
		return windows_python
	if FileAccess.file_exists(unix_python):
		return unix_python
	return windows_python if OS.get_name() == "Windows" else unix_python


func _selected_mode() -> String:
	return "passive" if _mode.selected == 1 else "hold"


func _on_hide_pressed() -> void:
	_panel.visible = false


func _on_setup_pressed() -> void:
	if _active_pid > 0:
		return
	if OS.get_name() != "Windows":
		_set_status("SETUP NOT STARTED", "The repo setup script is currently PowerShell/Windows-specific.")
		return
	if not FileAccess.file_exists(_setup_script_path):
		_set_status("SETUP ERROR", "Missing setup script: %s" % _setup_script_path)
		return

	var venv_path: String = _repo_root.path_join(".venv-isaac")
	var arguments: PackedStringArray = PackedStringArray()
	arguments.append("-NoProfile")
	arguments.append("-ExecutionPolicy")
	arguments.append("Bypass")
	arguments.append("-File")
	arguments.append(_setup_script_path)
	arguments.append("-VenvPath")
	arguments.append(venv_path)
	var pid: int = OS.create_process("powershell.exe", arguments, false)
	if pid < 0:
		_set_status("SETUP ERROR", "Unable to launch powershell.exe")
		return
	_active_pid = pid
	_active_task = "setup"
	_setup_process = true
	_set_busy(true)
	_set_status("RUNNING • setup", "Installing/updating .venv-isaac in the background. Godot remains interactive.")


func _on_check_stack_pressed() -> void:
	_launch_bridge("check_stack")


func _on_build_pressed() -> void:
	_launch_bridge("build")


func _on_tests_pressed() -> void:
	_launch_bridge("tests")


func _on_convert_pressed() -> void:
	_launch_bridge("convert")


func _on_smoke_pressed() -> void:
	_launch_bridge("smoke")


func _on_preflight_pressed() -> void:
	_launch_bridge("preflight")


func _on_cancel_pressed() -> void:
	if _active_pid <= 0:
		return
	if _setup_process:
		OS.kill(_active_pid)
		_set_status("CANCELED • setup", "Setup process was terminated. The environment may be incomplete.")
		_active_pid = 0
		_active_task = ""
		_setup_process = false
		_set_busy(false)
		return

	var file: FileAccess = FileAccess.open(_cancel_path, FileAccess.WRITE)
	if file != null:
		file.store_string("cancel\n")
		file.close()
		_set_status("CANCEL REQUESTED", "Waiting for the bridge to terminate the active child process cleanly...")
	else:
		_set_status("CANCEL ERROR", "Could not write %s" % _cancel_path)


func _on_open_folder_pressed() -> void:
	DirAccess.make_dir_recursive_absolute(_control_dir)
	var path: String = _control_dir.replace("\\", "/")
	if not path.begins_with("/"):
		path = "/" + path
	OS.shell_open("file://" + path)


func _on_refresh_pressed() -> void:
	_refresh_environment_status()
	_refresh_bridge_status()
	_refresh_log()


func _launch_bridge(task: String) -> void:
	if _active_pid > 0:
		return
	var python: String = _python_path.text.strip_edges()
	if not FileAccess.file_exists(python):
		_set_status("PYTHON MISSING", "Training Python not found. Run Setup Isaac or correct the Python field.")
		return
	if not FileAccess.file_exists(_bridge_path):
		_set_status("BRIDGE MISSING", "Missing %s" % _bridge_path)
		return

	_clear_runtime_files()
	var arguments: PackedStringArray = PackedStringArray()
	arguments.append(_bridge_path)
	arguments.append("--task")
	arguments.append(task)
	arguments.append("--status")
	arguments.append(_status_path)
	arguments.append("--log")
	arguments.append(_log_path)
	arguments.append("--cancel")
	arguments.append(_cancel_path)
	arguments.append("--mode")
	arguments.append(_selected_mode())
	arguments.append("--num-envs")
	arguments.append(str(int(_env_count.value)))
	arguments.append("--seconds")
	arguments.append(str(_seconds.value))
	arguments.append("--seed")
	arguments.append(str(int(_seed.value)))
	arguments.append("--joint-noise-deg")
	arguments.append(str(_noise_deg.value))
	arguments.append("--device")
	arguments.append(_device_edit.text.strip_edges())
	arguments.append("--force")
	if _headless.button_pressed:
		arguments.append("--headless")
	if _strict_contact.button_pressed:
		arguments.append("--strict-contact")

	var pid: int = OS.create_process(python, arguments, false)
	if pid < 0:
		_set_status("LAUNCH ERROR", "Unable to start training bridge with %s" % python)
		return
	_active_pid = pid
	_active_task = task
	_setup_process = false
	_set_busy(true)
	_set_status("STARTING • %s" % task, "PID %d • waiting for bridge status..." % pid)


func _clear_runtime_files() -> void:
	for path: String in [_status_path, _log_path, _cancel_path]:
		if FileAccess.file_exists(path):
			DirAccess.remove_absolute(path)
	_last_status_text = ""
	_last_log_text = ""
	_log_view.text = ""


func _refresh_bridge_status() -> void:
	if not FileAccess.file_exists(_status_path):
		return
	var text: String = FileAccess.get_file_as_string(_status_path)
	if text.is_empty() or text == _last_status_text:
		return
	_last_status_text = text
	var parsed: Variant = JSON.parse_string(text)
	if not (parsed is Dictionary):
		return
	var data: Dictionary = parsed as Dictionary
	var state: String = String(data.get("state", "unknown"))
	var message: String = String(data.get("message", ""))
	var current_step: String = String(data.get("current_step", ""))
	var step_index: int = int(data.get("step_index", 0))
	var step_count: int = int(data.get("step_count", 0))
	var status_text: String = state.to_upper()
	if not current_step.is_empty():
		status_text += " • %s" % current_step
	var detail: String = message
	if step_count > 0:
		detail += "\nStep %d/%d • PID %d" % [step_index, step_count, _active_pid]
	_set_status(status_text, detail)


func _refresh_log() -> void:
	if not FileAccess.file_exists(_log_path):
		return
	var text: String = FileAccess.get_file_as_string(_log_path)
	if text == _last_log_text:
		return
	_last_log_text = text
	if text.length() > LOG_CHAR_LIMIT:
		text = "… earlier output truncated …\n" + text.right(LOG_CHAR_LIMIT)
	_log_view.text = text
	_log_view.scroll_vertical = _log_view.get_line_count()


func _finish_setup_process() -> void:
	_active_pid = 0
	_active_task = ""
	_setup_process = false
	_set_busy(false)
	_python_path.text = _default_training_python()
	if FileAccess.file_exists(_python_path.text):
		_set_status("SETUP FINISHED", "Training Python exists. Run Check stack next.")
	else:
		_set_status("SETUP FAILED / INCOMPLETE", "The expected .venv-isaac Python was not created.")


func _finish_bridge_process() -> void:
	_refresh_bridge_status()
	_refresh_log()
	var final_state: String = "unknown"
	var final_message: String = "Bridge process exited without a readable final status."
	if FileAccess.file_exists(_status_path):
		var parsed: Variant = JSON.parse_string(FileAccess.get_file_as_string(_status_path))
		if parsed is Dictionary:
			var data: Dictionary = parsed as Dictionary
			final_state = String(data.get("state", "unknown"))
			final_message = String(data.get("message", final_message))

	var finished_task: String = _active_task
	_active_pid = 0
	_active_task = ""
	_set_busy(false)
	var report_summary: String = ""
	if final_state == "passed" and (finished_task == "smoke" or finished_task == "preflight"):
		report_summary = _latest_smoke_summary(_selected_mode() if finished_task == "smoke" else "passive")
	if not report_summary.is_empty():
		final_message += "\n" + report_summary
	_set_status(final_state.to_upper() + " • " + finished_task, final_message)


func _latest_smoke_summary(mode: String) -> String:
	var path: String = _experiment_root.path_join("runs").path_join("smoke").path_join("latest_%s.json" % mode)
	if not FileAccess.file_exists(path):
		return ""
	var parsed: Variant = JSON.parse_string(FileAccess.get_file_as_string(path))
	if not (parsed is Dictionary):
		return ""
	var report: Dictionary = parsed as Dictionary
	var diagnostics_variant: Variant = report.get("diagnostics", {})
	if not (diagnostics_variant is Dictionary):
		return ""
	var diagnostics: Dictionary = diagnostics_variant as Dictionary
	return (
		"%s smoke: root %.3f m • contact %.1f N • limit Δ %.4f rad • %.0f env-steps/s"
		% [
			mode,
			float(diagnostics.get("final_root_height_mean_m", 0.0)),
			float(diagnostics.get("max_foot_contact_force_n", 0.0)),
			float(diagnostics.get("max_joint_limit_violation_rad", 0.0)),
			float(report.get("sim_steps_per_wall_second", 0.0)),
		]
	)


func _refresh_environment_status() -> void:
	if _python_path == null:
		return
	var python_exists: bool = FileAccess.file_exists(_python_path.text.strip_edges())
	var bridge_exists: bool = FileAccess.file_exists(_bridge_path)
	var manifest_exists: bool = FileAccess.file_exists(_experiment_root.path_join("generated").path_join("articulation_manifest.json"))
	var usd_dir: String = _experiment_root.path_join("assets").path_join("usd")
	var status: String = "READY" if python_exists and bridge_exists else "SETUP REQUIRED"
	var detail: String = "Python %s • bridge %s • manifest %s • USD dir %s" % [
		"OK" if python_exists else "missing",
		"OK" if bridge_exists else "missing",
		"present" if manifest_exists else "not built",
		"present" if DirAccess.dir_exists_absolute(usd_dir) else "not converted",
	]
	_set_status(status, detail)


func _set_busy(busy: bool) -> void:
	for button: Button in _task_buttons:
		button.disabled = busy
	_cancel_button.disabled = not busy
	_python_path.editable = not busy
	_device_edit.editable = not busy
	_env_count.editable = not busy
	_seconds.editable = not busy
	_seed.editable = not busy
	_noise_deg.editable = not busy
	_mode.disabled = busy
	_headless.disabled = busy
	_strict_contact.disabled = busy


func _set_status(status: String, detail: String) -> void:
	_status_label.text = status
	_detail_label.text = detail
