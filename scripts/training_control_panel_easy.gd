extends "res://scripts/training_control_panel.gd"
## Beginner-facing shell for the humanoid training tools.
##
## The existing training_control_panel.gd remains the technical backend. This
## derived UI keeps every manual control available under Advanced, while the
## default screen presents the workflow as three simple steps and translates
## RSL-RL console output into live progress/learning metrics.

const EASY_PANEL_WIDTH: float = 560.0
const STAND_EPISODE_SECONDS: float = 8.0
const POLICY_HZ: float = 60.0

var _progress_bar: ProgressBar
var _progress_text: Label
var _phase_label: Label
var _next_action_label: Label

var _tools_state: Label
var _verify_state: Label
var _train_state: Label
var _preset: OptionButton
var _preset_help: Label

var _iteration_label: Label
var _reward_label: Label
var _episode_label: Label
var _speed_label: Label
var _eta_label: Label
var _trend_label: Label

var _advanced_box: VBoxContainer
var _advanced_toggle: CheckButton
var _log_toggle: CheckButton

var _last_iteration_seen: int = -1
var _reward_samples: Array[float] = []


func _build_ui() -> void:
	_panel = PanelContainer.new()
	_panel.name = "HumanoidTrainer"
	_panel.anchor_left = 1.0
	_panel.anchor_right = 1.0
	_panel.anchor_bottom = 1.0
	_panel.offset_left = -EASY_PANEL_WIDTH - 12.0
	_panel.offset_right = -12.0
	_panel.offset_top = 12.0
	_panel.offset_bottom = -12.0
	_panel.mouse_filter = Control.MOUSE_FILTER_STOP
	add_child(_panel)

	var margin := MarginContainer.new()
	for side: String in ["left", "right", "top", "bottom"]:
		margin.add_theme_constant_override("margin_%s" % side, 14)
	_panel.add_child(margin)

	var scroll := ScrollContainer.new()
	scroll.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	margin.add_child(scroll)

	var root := VBoxContainer.new()
	root.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	root.add_theme_constant_override("separation", 8)
	scroll.add_child(root)

	_build_header(root)
	_build_status_card(root)
	_build_guided_steps(root)
	_build_live_training_card(root)
	_build_advanced(root)


func _build_header(root: VBoxContainer) -> void:
	var row := HBoxContainer.new()
	root.add_child(row)
	var title := Label.new()
	title.text = "HUMANOID TRAINER"
	title.add_theme_font_size_override("font_size", 21)
	title.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(title)
	var hide := Button.new()
	hide.text = "Hide [F3]"
	hide.tooltip_text = "Press F3 at any time to show/hide this panel."
	hide.pressed.connect(_on_hide_pressed)
	row.add_child(hide)

	var intro := Label.new()
	intro.text = "Teach the physics humanoid to stand. For normal use, follow steps 1 → 2 → 3 below."
	intro.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	root.add_child(intro)


func _build_status_card(root: VBoxContainer) -> void:
	root.add_child(HSeparator.new())
	_status_label = Label.new()
	_status_label.text = "Checking setup..."
	_status_label.add_theme_font_size_override("font_size", 18)
	root.add_child(_status_label)

	_detail_label = Label.new()
	_detail_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	root.add_child(_detail_label)

	_phase_label = Label.new()
	_phase_label.text = "What is happening: idle"
	_phase_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	root.add_child(_phase_label)

	_progress_bar = ProgressBar.new()
	_progress_bar.min_value = 0.0
	_progress_bar.max_value = 100.0
	_progress_bar.value = 0.0
	_progress_bar.show_percentage = false
	_progress_bar.custom_minimum_size.y = 18.0
	root.add_child(_progress_bar)

	_progress_text = Label.new()
	_progress_text.text = "No task running"
	root.add_child(_progress_text)

	_next_action_label = Label.new()
	_next_action_label.text = "Next: check the training tools."
	_next_action_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	root.add_child(_next_action_label)


func _build_guided_steps(root: VBoxContainer) -> void:
	_section(root, "Simple workflow")

	_tools_state = _step_title(root, "1", "Training tools", "Installs Python, Isaac Lab, CUDA training libraries and RSL-RL.")
	var setup_row := HBoxContainer.new()
	root.add_child(setup_row)
	var setup_button := _task_button(setup_row, "Install / repair training tools", _on_setup_pressed)
	setup_button.tooltip_text = "Safe to use if the environment is missing or broken. This can take several minutes."

	_verify_state = _step_title(root, "2", "Prepare & verify", "Builds the articulation, checks neural I/O, converts the robot and runs both physics smoke tests.")
	var verify_row := HBoxContainer.new()
	root.add_child(verify_row)
	var verify_button := _task_button(verify_row, "PREPARE & VERIFY", _on_preflight_pressed)
	verify_button.tooltip_text = "Recommended before a new training run. It automatically runs the whole preflight sequence."

	_train_state = _step_title(root, "3", "Teach standing", "Runs many copies of the humanoid in parallel and improves the neural controller with PPO.")
	var preset_row := _labeled_row(root, "Training plan")
	_preset = OptionButton.new()
	_preset.add_item("Quick check — 512 worlds / 50 updates", 0)
	_preset.add_item("Full stand training — 2048 worlds / 1500 updates", 1)
	_preset.add_item("Custom — use Advanced settings", 2)
	_preset.selected = 0
	_preset.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_preset.item_selected.connect(_on_preset_selected)
	preset_row.add_child(_preset)

	_preset_help = Label.new()
	_preset_help.text = "Recommended first: Quick check. It proves the complete PPO loop works before committing to a long run."
	_preset_help.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	root.add_child(_preset_help)

	var train_row := HBoxContainer.new()
	root.add_child(train_row)
	var train_button := _task_button(train_row, "START TRAINING", _on_train_stand_pressed)
	train_button.custom_minimum_size.y = 42.0
	train_button.tooltip_text = "Training runs in a separate headless Isaac process. Godot stays usable while it learns."

	var background_note := Label.new()
	background_note.text = "Training runs in the background. You can keep using the game; this panel updates automatically."
	background_note.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	root.add_child(background_note)


func _build_live_training_card(root: VBoxContainer) -> void:
	_section(root, "Live learning — plain language")
	_iteration_label = _metric_row(root, "Progress", "Waiting for training")
	_reward_label = _metric_row(root, "Learning score", "—")
	_episode_label = _metric_row(root, "Standing time", "— / %.1f s" % STAND_EPISODE_SECONDS)
	_speed_label = _metric_row(root, "GPU speed", "—")
	_eta_label = _metric_row(root, "Time remaining", "—")
	_trend_label = Label.new()
	_trend_label.text = "How to read this: reward should generally rise; standing time is the easiest metric — the lesson lasts 8 seconds."
	_trend_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	root.add_child(_trend_label)

	var goal := Label.new()
	goal.text = "The AI is rewarded for staying upright, keeping its center of mass over the feet, moving smoothly and avoiding unnecessary torque. It is not playing an animation."
	goal.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	root.add_child(goal)


func _build_advanced(root: VBoxContainer) -> void:
	root.add_child(HSeparator.new())
	_advanced_toggle = CheckButton.new()
	_advanced_toggle.text = "Show Advanced / troubleshooting controls"
	_advanced_toggle.button_pressed = false
	_advanced_toggle.toggled.connect(_on_advanced_toggled)
	root.add_child(_advanced_toggle)

	_advanced_box = VBoxContainer.new()
	_advanced_box.visible = false
	_advanced_box.add_theme_constant_override("separation", 5)
	root.add_child(_advanced_box)

	_section(_advanced_box, "Runtime")
	_python_path = _line_row(_advanced_box, "Python", _default_training_python())
	_device_edit = _line_row(_advanced_box, "Device", "cuda:0")
	_seed = _spin_row(_advanced_box, "Seed", 0.0, 2147483647.0, 1467.0, 1.0)

	_section(_advanced_box, "Training details")
	_train_env_count = _spin_row(_advanced_box, "Training worlds", 1.0, 8192.0, 512.0, 1.0)
	_train_iterations = _spin_row(_advanced_box, "Learning updates", 1.0, 100000.0, 50.0, 1.0)
	var stand_note := Label.new()
	stand_note.text = "Foundation ABI: 197 observations • 54 actions • PhysX 240 Hz • neural policy 60 Hz"
	stand_note.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_advanced_box.add_child(stand_note)

	_section(_advanced_box, "Physics test details")
	_smoke_env_count = _spin_row(_advanced_box, "Smoke worlds", 1.0, 4096.0, 256.0, 1.0)
	_smoke_seconds = _spin_row(_advanced_box, "Smoke seconds", 0.1, 30.0, 2.0, 0.1)
	_noise_deg = _spin_row(_advanced_box, "Joint noise °", 0.0, 45.0, 0.0, 0.25)
	var mode_row := _labeled_row(_advanced_box, "Smoke mode")
	_mode = OptionButton.new()
	_mode.add_item("Driven / neutral PD", 0)
	_mode.add_item("Passive fall", 1)
	_mode.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	mode_row.add_child(_mode)

	var checks := HBoxContainer.new()
	_advanced_box.add_child(checks)
	_headless = CheckButton.new()
	_headless.text = "Headless tools"
	_headless.button_pressed = true
	checks.add_child(_headless)
	_strict_contact = CheckButton.new()
	_strict_contact.text = "Require foot contact"
	_strict_contact.button_pressed = true
	checks.add_child(_strict_contact)

	_section(_advanced_box, "Manual pipeline")
	var row_a := HBoxContainer.new()
	_advanced_box.add_child(row_a)
	_task_button(row_a, "Check libraries", _on_check_stack_pressed)
	_task_button(row_a, "Build robot", _on_build_pressed)
	var row_b := HBoxContainer.new()
	_advanced_box.add_child(row_b)
	_task_button(row_b, "Run code tests", _on_tests_pressed)
	_task_button(row_b, "Convert USD", _on_convert_pressed)
	var row_c := HBoxContainer.new()
	_advanced_box.add_child(row_c)
	_task_button(row_c, "Run selected smoke", _on_smoke_pressed)

	var process_row := HBoxContainer.new()
	_advanced_box.add_child(process_row)
	_cancel_button = Button.new()
	_cancel_button.text = "STOP active task"
	_cancel_button.disabled = true
	_cancel_button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_cancel_button.pressed.connect(_on_cancel_pressed)
	process_row.add_child(_cancel_button)
	var folder_button := Button.new()
	folder_button.text = "Open training results"
	folder_button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	folder_button.pressed.connect(_on_open_folder_pressed)
	process_row.add_child(folder_button)

	_log_toggle = CheckButton.new()
	_log_toggle.text = "Show technical log"
	_log_toggle.button_pressed = false
	_log_toggle.toggled.connect(_on_log_toggled)
	_advanced_box.add_child(_log_toggle)

	_log_view = TextEdit.new()
	_log_view.editable = false
	_log_view.visible = false
	_log_view.custom_minimum_size.y = 250.0
	_log_view.wrap_mode = TextEdit.LINE_WRAPPING_BOUNDARY
	_advanced_box.add_child(_log_view)


func _step_title(parent: VBoxContainer, number: String, title: String, explanation: String) -> Label:
	var row := HBoxContainer.new()
	parent.add_child(row)
	var label := Label.new()
	label.text = "%s. %s" % [number, title]
	label.add_theme_font_size_override("font_size", 16)
	label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(label)
	var state := Label.new()
	state.text = "CHECKING"
	state.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	state.custom_minimum_size.x = 120.0
	row.add_child(state)
	var help := Label.new()
	help.text = explanation
	help.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	parent.add_child(help)
	return state


func _metric_row(parent: VBoxContainer, name: String, value: String) -> Label:
	var row := HBoxContainer.new()
	parent.add_child(row)
	var name_label := Label.new()
	name_label.text = name
	name_label.custom_minimum_size.x = 150.0
	row.add_child(name_label)
	var value_label := Label.new()
	value_label.text = value
	value_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(value_label)
	return value_label


func _on_advanced_toggled(enabled: bool) -> void:
	_advanced_box.visible = enabled


func _on_log_toggled(enabled: bool) -> void:
	_log_view.visible = enabled


func _on_preset_selected(index: int) -> void:
	if _train_env_count == null or _train_iterations == null:
		return
	match index:
		0:
			_train_env_count.value = 512.0
			_train_iterations.value = 50.0
			_preset_help.text = "Recommended first: Quick check. It proves the complete PPO loop works before committing to a long run."
		1:
			_train_env_count.value = 2048.0
			_train_iterations.value = 1500.0
			_preset_help.text = "Full lesson: use this after the Quick check runs cleanly. This is intended to learn useful standing/balance."
		2:
			_preset_help.text = "Custom mode uses Training worlds and Learning updates under Advanced."


func _refresh_environment_status() -> void:
	if _python_path == null:
		return
	var python_ok := FileAccess.file_exists(_python_path.text.strip_edges())
	var bridge_ok := FileAccess.file_exists(_bridge_path)
	var manifest_ok := FileAccess.file_exists(_experiment_root.path_join("generated").path_join("articulation_manifest.json"))
	var usd_ok := DirAccess.dir_exists_absolute(_experiment_root.path_join("assets").path_join("usd"))

	_tools_state.text = "READY" if python_ok and bridge_ok else "NEEDS SETUP"
	_verify_state.text = "FILES PRESENT" if manifest_ok and usd_ok else "NOT PREPARED"
	_train_state.text = "READY AFTER CHECK" if python_ok else "WAITING"

	_progress_bar.value = 0.0
	_progress_text.text = "No task running"
	_phase_label.text = "What is happening: waiting for you"
	if python_ok and bridge_ok:
		_status_label.text = "Ready"
		_detail_label.text = "Training tools are available. The guided workflow will handle the technical steps for you."
		_next_action_label.text = "Next: click PREPARE & VERIFY."
	else:
		_status_label.text = "Training tools need setup"
		_detail_label.text = "The Isaac training Python environment is missing or incomplete."
		_next_action_label.text = "Next: click Install / repair training tools."


func _refresh_bridge_status() -> void:
	if not FileAccess.file_exists(_status_path):
		return
	var text := FileAccess.get_file_as_string(_status_path)
	if text.is_empty() or text == _last_status_text:
		return
	_last_status_text = text
	var parsed: Variant = JSON.parse_string(text)
	if not (parsed is Dictionary):
		return
	var data := parsed as Dictionary
	var state := String(data.get("state", "unknown"))
	var current_step := String(data.get("current_step", ""))
	var message := String(data.get("message", ""))
	var step_index := int(data.get("step_index", 0))
	var step_count := int(data.get("step_count", 0))

	if _active_task == "train_stand":
		_status_label.text = "Learning to stand"
		_detail_label.text = "Isaac is running many humanoids in parallel and PPO is improving one shared controller."
		_train_state.text = "LEARNING"
		_next_action_label.text = "No action needed. Training is automatic; you can keep playing while it runs."
		if not current_step.is_empty() and _last_iteration_seen < 0:
			_phase_label.text = "What is happening: %s" % _friendly_step(current_step)
		return

	if state == "running":
		_status_label.text = "Preparing training"
		_detail_label.text = _friendly_step(current_step) if not current_step.is_empty() else message
		_phase_label.text = "What is happening: %s" % (_friendly_step(current_step) if not current_step.is_empty() else "starting")
		_next_action_label.text = "No action needed. The checks run automatically."
		if _active_task == "preflight":
			_verify_state.text = "CHECKING"
		if step_count > 0:
			var completed_before := max(step_index - 1, 0)
			_progress_bar.value = 100.0 * float(completed_before) / float(step_count)
			_progress_text.text = "Automatic check %d of %d" % [step_index, step_count]
	elif state == "failed":
		_status_label.text = "Something needs attention"
		_detail_label.text = message
		_phase_label.text = "What is happening: task stopped on an error"
		_next_action_label.text = "Open Advanced → Show technical log for the exact error."
	elif state == "canceled":
		_status_label.text = "Stopped"
		_detail_label.text = "The active task was canceled."
		_phase_label.text = "What is happening: idle"
		_next_action_label.text = "You can restart the step whenever you are ready."


func _refresh_log() -> void:
	super._refresh_log()
	if _last_log_text.is_empty():
		return
	if _active_task == "train_stand" or _last_log_text.contains("Asterra foundation training"):
		_update_training_progress_from_log(_last_log_text)


func _update_training_progress_from_log(text: String) -> void:
	if text.contains("[Asterra training] entering PPO learn loop") and _last_iteration_seen < 0:
		_progress_bar.value = max(_progress_bar.value, 18.0)
		_progress_text.text = "Neural network ready — beginning learning"
		_phase_label.text = "What is happening: collecting experience and updating the neural network"
	elif text.contains("[Asterra training] PPO runner ready") and _last_iteration_seen < 0:
		_progress_bar.value = max(_progress_bar.value, 15.0)
		_progress_text.text = "Neural network initialized"
		_phase_label.text = "What is happening: preparing PPO"
	elif text.contains("[Asterra training] RSL-RL environment wrapper ready") and _last_iteration_seen < 0:
		_progress_bar.value = max(_progress_bar.value, 12.0)
		_progress_text.text = "Training worlds reset and ready"
		_phase_label.text = "What is happening: placing humanoids into their starting poses"
	elif text.contains("[Asterra training] DirectRLEnv constructed") and _last_iteration_seen < 0:
		_progress_bar.value = max(_progress_bar.value, 9.0)
		_progress_text.text = "Physics worlds created"
		_phase_label.text = "What is happening: initializing sensors and physics tensors"
	elif text.contains("[Asterra training] constructing DirectRLEnv") and _last_iteration_seen < 0:
		_progress_bar.value = max(_progress_bar.value, 5.0)
		_progress_text.text = "Creating parallel training worlds"
		_phase_label.text = "What is happening: building the Isaac simulation"

	var iteration_match := _last_match(text, "Learning iteration\\s+(\\d+)/(\\d+)")
	if iteration_match != null:
		var raw_iteration := int(iteration_match.get_string(1))
		var total := max(int(iteration_match.get_string(2)), 1)
		var displayed_iteration := clampi(raw_iteration + 1, 1, total)
		_last_iteration_seen = raw_iteration
		_progress_bar.value = 18.0 + 82.0 * float(displayed_iteration) / float(total)
		_progress_text.text = "Learning update %d of %d" % [displayed_iteration, total]
		_iteration_label.text = "%d / %d  (%.0f%%)" % [displayed_iteration, total, 100.0 * float(displayed_iteration) / float(total)]
		_phase_label.text = "What is happening: simulating → scoring behavior → updating the neural network"

	var reward_match := _last_match(text, "Mean reward:\\s+(-?[0-9.]+)")
	if reward_match != null:
		var reward := float(reward_match.get_string(1))
		_reward_label.text = "%.2f  (higher over time is better)" % reward
		if _last_iteration_seen >= 0 and (_reward_samples.is_empty() or _reward_samples.back() != reward):
			_reward_samples.append(reward)
			if _reward_samples.size() > 20:
				_reward_samples.pop_front()
		_update_reward_trend()

	var length_match := _last_match(text, "Mean episode length:\\s+([0-9.]+)")
	if length_match != null:
		var steps := float(length_match.get_string(1))
		var seconds := steps / POLICY_HZ
		_episode_label.text = "%.2f / %.1f s" % [seconds, STAND_EPISODE_SECONDS]

	var speed_match := _last_match(text, "Computation:\\s+([0-9]+) steps/s")
	if speed_match != null:
		_speed_label.text = "%s simulation steps/s" % speed_match.get_string(1)

	var eta_match := _last_match(text, "ETA:\\s+([0-9:]+)")
	if eta_match != null:
		_eta_label.text = eta_match.get_string(1)


func _last_match(text: String, pattern: String) -> RegExMatch:
	var regex := RegEx.new()
	if regex.compile(pattern) != OK:
		return null
	var matches := regex.search_all(text)
	if matches.is_empty():
		return null
	return matches[matches.size() - 1]


func _update_reward_trend() -> void:
	if _reward_samples.size() < 4:
		_trend_label.text = "Reward trend: collecting enough updates to show a trend. Standing time is still the easiest metric to interpret."
		return
	var first := _reward_samples[0]
	var last := _reward_samples[_reward_samples.size() - 1]
	var threshold := max(0.10, abs(first) * 0.05)
	if last > first + threshold:
		_trend_label.text = "Reward trend: improving. That is a good sign; also watch whether standing time moves toward 8 seconds."
	elif last < first - threshold:
		_trend_label.text = "Reward trend: currently falling. Early PPO can be noisy; judge the direction over many updates, not one update."
	else:
		_trend_label.text = "Reward trend: roughly flat so far. Early training can take time before a useful balance strategy appears."


func _friendly_step(step: String) -> String:
	match step:
		"check training stack":
			return "checking Python, CUDA and training libraries"
		"build articulation":
			return "building the humanoid's physics description"
		"articulation builder tests":
			return "checking the 19-body / 54-joint structure"
		"articulation helper tests":
			return "checking the mapping between the neural controller and PhysX"
		"foundation tensor tests":
			return "checking the 197 inputs and 54 outputs used by the neural network"
		"convert URDF to USD":
			return "preparing the humanoid asset for Isaac PhysX"
		"PhysX smoke hold":
			return "testing driven joints, contacts and hard limits"
		"PhysX smoke passive":
			return "dropping a passive ragdoll to verify impact stability"
		"train foundation: stand":
			return "teaching the humanoid to stand"
		_:
			return step.replace("_", " ")


func _finish_setup_process() -> void:
	super._finish_setup_process()
	var ready := FileAccess.file_exists(_python_path.text.strip_edges())
	_tools_state.text = "READY" if ready else "INCOMPLETE"
	if ready:
		_progress_bar.value = 100.0
		_progress_text.text = "Training tools installed"
		_phase_label.text = "What is happening: setup complete"
		_next_action_label.text = "Next: click PREPARE & VERIFY."


func _finish_bridge_process() -> void:
	var finished_task := _active_task
	super._finish_bridge_process()
	var final_state := _read_final_bridge_state()
	if final_state == "passed":
		_progress_bar.value = 100.0
		if finished_task == "preflight":
			_verify_state.text = "READY"
			_train_state.text = "READY"
			_status_label.text = "Physics check passed"
			_detail_label.text = "The articulation, neural I/O contract, USD conversion and both PhysX smoke tests passed."
			_progress_text.text = "Preparation complete"
			_phase_label.text = "What is happening: ready to train"
			_next_action_label.text = "Next: choose Quick check or Full stand training, then click START TRAINING."
		elif finished_task == "train_stand":
			_train_state.text = "COMPLETE"
			_status_label.text = "Training finished"
			_detail_label.text = "The requested PPO run completed and its checkpoint/logs were saved."
			_progress_text.text = "Training complete"
			_phase_label.text = "What is happening: finished"
			_next_action_label.text = "Result saved. Use Advanced → Open training results to inspect checkpoints and logs."
	elif final_state == "failed":
		if finished_task == "preflight":
			_verify_state.text = "FAILED"
		elif finished_task == "train_stand":
			_train_state.text = "STOPPED"
		_status_label.text = "Something needs attention"
		_next_action_label.text = "Open Advanced → Show technical log for the exact error."


func _read_final_bridge_state() -> String:
	if not FileAccess.file_exists(_status_path):
		return "unknown"
	var parsed: Variant = JSON.parse_string(FileAccess.get_file_as_string(_status_path))
	if parsed is Dictionary:
		return String((parsed as Dictionary).get("state", "unknown"))
	return "unknown"


func _clear_runtime_files() -> void:
	_last_iteration_seen = -1
	_reward_samples.clear()
	_iteration_label.text = "Starting..."
	_reward_label.text = "—"
	_episode_label.text = "— / %.1f s" % STAND_EPISODE_SECONDS
	_speed_label.text = "—"
	_eta_label.text = "—"
	_trend_label.text = "How to read this: reward should generally rise; standing time is the easiest metric — the lesson lasts 8 seconds."
	_progress_bar.value = 0.0
	_progress_text.text = "Starting..."
	super._clear_runtime_files()


func _set_status(status: String, detail: String) -> void:
	# Base/backend methods use compact developer status strings. Translate the most
	# common ones here so setup/cancel/manual tools still read naturally.
	var upper := status.to_upper()
	if upper.begins_with("RUNNING"):
		_status_label.text = "Working..."
	elif upper.begins_with("STARTING"):
		_status_label.text = "Starting..."
	elif upper.begins_with("CANCEL"):
		_status_label.text = "Stopping..."
	elif upper.contains("FAILED") or upper.contains("ERROR") or upper.contains("MISSING"):
		_status_label.text = "Something needs attention"
	elif upper.contains("SETUP FINISHED"):
		_status_label.text = "Training tools ready"
	elif upper == "READY":
		_status_label.text = "Ready"
	else:
		_status_label.text = status.capitalize()
	_detail_label.text = detail
