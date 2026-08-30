extends "res://scripts/world_authoring/world_authoring_editor_live_phase23.gd"
## Phase 25: live Godot shader-source editing for the actual production materials.
##
## Phase 18 exposed the resident ShaderMaterial, shader path and uniforms. This
## layer completes that path with editable Godot shading-language source:
## - source is edited in a CodeEdit with line numbers and lightweight highlighting;
## - edits are preflighted and assigned to a transient Shader after a short debounce;
## - the last submitted source is persisted per body in PlanetAuthoringProfile;
## - production .gdshader files are never overwritten by ordinary live editing;
## - explicit uniform overrides survive source recompiles and remain final writers;
## - a selected lightweight moon can never rebind the resident root planet shader.

const LIVE_SHADER_DEBOUNCE_S: float = 0.35
const LIVE_SHADER_MAX_SOURCE_BYTES: int = 2 * 1024 * 1024
const LIVE_SHADER_EDITOR_HEIGHT_PX: float = 460.0

var _live_shader_source_expanded: Dictionary = {}
var _live_shader_auto_compile: Dictionary = {}
var _live_shader_pending: Dictionary = {}
var _live_shader_cache: Dictionary = {}
var _live_shader_status_text: Dictionary = {}
var _live_shader_status_labels: Dictionary = {}
var _live_shader_editor_controls: Dictionary = {}


func _process(delta: float) -> void:
	super._process(delta)
	_tick_live_shader_compilation(maxf(delta, 0.0))


func _build_runtime_shader_inspector(target_id: String, title: String,
		material: ShaderMaterial) -> void:
	# The singleton production terrain/ocean/sky materials belong to the resident
	# detailed root body. Never expose those as if they were the selected moon's
	# shader while the moon is using the lightweight orbital representation.
	if not _interest_uses_detailed_surface:
		_section(title)
		_add_note("No detailed ShaderMaterial is attached to the selected orbital preview body. Select the resident detailed root planet to edit its live production shader source.")
		return
	super._build_runtime_shader_inspector(target_id, title, material)
	if material != null and material.shader != null:
		_build_live_shader_source_editor(target_id, material)


func _build_live_shader_source_editor(target_id: String,
		material: ShaderMaterial) -> void:
	var profile: Resource = _session.active_planet_profile() as Resource
	if profile == null:
		return
	var source_override: String = String(profile.call("runtime_shader_source", target_id)) \
		if profile.has_method("runtime_shader_source") else ""
	var source_active: bool = not source_override.is_empty()
	var expanded: bool = bool(_live_shader_source_expanded.get(target_id, false))
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 7)
	_workspace.add_child(row)

	var toggle := Button.new()
	toggle.text = "Hide Live Source" if expanded else "Edit Live Source"
	toggle.pressed.connect(func() -> void:
		_live_shader_source_expanded[target_id] = not expanded
		_refresh_current_category()
	)
	row.add_child(toggle)

	var state := Label.new()
	state.text = "LIVE SOURCE OVERRIDE" if source_active else "production source"
	state.modulate = Color(0.96, 0.72, 0.34) if source_active \
		else Color(0.58, 0.70, 0.79)
	row.add_child(state)
	var base_path: String = _runtime_shader_base_path(target_id, material)
	var base_label := Label.new()
	base_label.text = "base: %s" % (base_path if not base_path.is_empty() else "<runtime shader>")
	base_label.tooltip_text = base_label.text
	base_label.text_overrun_behavior = TextServer.OVERRUN_TRIM_ELLIPSIS
	base_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	base_label.modulate = Color(0.52, 0.64, 0.73)
	row.add_child(base_label)
	if not expanded:
		return

	var toolbar := HBoxContainer.new()
	toolbar.add_theme_constant_override("separation", 6)
	_workspace.add_child(toolbar)
	var auto_compile := CheckButton.new()
	auto_compile.text = "Auto compile"
	auto_compile.tooltip_text = "Submit source after %.0f ms without typing" % (LIVE_SHADER_DEBOUNCE_S * 1000.0)
	auto_compile.button_pressed = bool(_live_shader_auto_compile.get(target_id, true))
	auto_compile.toggled.connect(func(enabled: bool) -> void:
		_live_shader_auto_compile[target_id] = enabled
		if enabled and _live_shader_pending.has(target_id):
			var pending: Dictionary = _live_shader_pending[target_id] as Dictionary
			pending["remaining"] = minf(float(pending.get("remaining", LIVE_SHADER_DEBOUNCE_S)), 0.05)
			_live_shader_pending[target_id] = pending
	)
	toolbar.add_child(auto_compile)

	var compile_now := Button.new()
	compile_now.text = "Compile Now"
	compile_now.tooltip_text = "Preflight and assign this source to the live ShaderMaterial immediately"
	toolbar.add_child(compile_now)
	var reset_source := Button.new()
	reset_source.text = "Revert Source"
	reset_source.tooltip_text = "Discard the transient source override and return to the selected/production .gdshader resource"
	reset_source.disabled = not source_active
	reset_source.pressed.connect(_clear_live_shader_source.bind(target_id, material))
	toolbar.add_child(reset_source)
	var source_info := Label.new()
	source_info.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	source_info.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	source_info.modulate = Color(0.52, 0.64, 0.73)
	toolbar.add_child(source_info)

	var editor := CodeEdit.new()
	editor.custom_minimum_size = Vector2(820.0, LIVE_SHADER_EDITOR_HEIGHT_PX)
	editor.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	editor.gutters_draw_line_numbers = true
	editor.gutters_draw_fold_gutter = true
	editor.line_folding = true
	editor.auto_brace_completion_enabled = true
	editor.auto_brace_completion_highlight_matching = true
	editor.wrap_mode = TextEdit.LINE_WRAPPING_NONE
	editor.syntax_highlighter = _make_shader_highlighter()
	var initial_source: String = source_override if source_active else material.shader.code
	editor.text = initial_source
	editor.tooltip_text = "Live Godot shading-language source. Ctrl+Enter or Compile Now submits immediately."
	_workspace.add_child(editor)
	_live_shader_editor_controls[target_id] = editor
	source_info.text = "%d lines • %.1f KiB" % [
		maxi(1, editor.get_line_count()),
		float(editor.text.to_utf8_buffer().size()) / 1024.0,
	]

	var status := Label.new()
	status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	status.modulate = Color(0.62, 0.74, 0.83)
	status.text = String(_live_shader_status_text.get(target_id,
		"Editing is non-destructive: source is stored in the body preset, not written to the project file. Godot compiler diagnostics still appear in the engine Output."))
	_workspace.add_child(status)
	_live_shader_status_labels[target_id] = status

	editor.text_changed.connect(func() -> void:
		source_info.text = "%d lines • %.1f KiB" % [
			maxi(1, editor.get_line_count()),
			float(editor.text.to_utf8_buffer().size()) / 1024.0,
		]
		_queue_live_shader_source(target_id, material, editor.text)
	)
	editor.gui_input.connect(func(event: InputEvent) -> void:
		if event is InputEventKey:
			var key := event as InputEventKey
			if key.pressed and not key.echo and key.keycode == KEY_ENTER \
					and (key.ctrl_pressed or key.meta_pressed):
				_submit_live_shader_source(target_id, material, editor.text)
				editor.accept_event()
	)
	compile_now.pressed.connect(func() -> void:
		_submit_live_shader_source(target_id, material, editor.text)
	)

	_add_note("Live source overrides the shader resource path for this target until Revert Source/Reset is used. Uniform controls remain layered on top of the compiled source. Obvious type/delimiter errors are blocked before assignment; deeper Godot shader compiler diagnostics are emitted to Output.")


func _make_shader_highlighter() -> CodeHighlighter:
	var highlighter := CodeHighlighter.new()
	highlighter.number_color = Color(0.78, 0.68, 0.44)
	highlighter.symbol_color = Color(0.76, 0.80, 0.86)
	highlighter.function_color = Color(0.58, 0.78, 0.95)
	highlighter.member_variable_color = Color(0.82, 0.72, 0.94)
	for keyword: String in [
		"shader_type", "render_mode", "uniform", "varying", "const", "struct",
		"void", "bool", "int", "uint", "float", "vec2", "vec3", "vec4",
		"ivec2", "ivec3", "ivec4", "uvec2", "uvec3", "uvec4", "mat2", "mat3", "mat4",
		"if", "else", "for", "while", "do", "switch", "case", "default", "break",
		"continue", "return", "discard", "true", "false", "in", "out", "inout",
	]:
		highlighter.add_keyword_color(keyword, Color(0.83, 0.60, 0.92))
	highlighter.add_color_region("//", "", Color(0.42, 0.58, 0.48), true)
	highlighter.add_color_region("/*", "*/", Color(0.42, 0.58, 0.48), false)
	highlighter.add_color_region("\"", "\"", Color(0.76, 0.66, 0.48), false)
	return highlighter


func _queue_live_shader_source(target_id: String, material: ShaderMaterial,
		source: String) -> void:
	var profile: Resource = _session.active_planet_profile() as Resource
	if profile == null:
		return
	_live_shader_pending[target_id] = {
		"source": source,
		"material": material,
		"remaining": LIVE_SHADER_DEBOUNCE_S,
		"profile_id": profile.get_instance_id(),
	}
	_set_live_shader_status(target_id,
		"Source changed — %s." % ("auto compile pending" \
		if bool(_live_shader_auto_compile.get(target_id, true)) else "press Compile Now"))


func _tick_live_shader_compilation(delta: float) -> void:
	if _live_shader_pending.is_empty():
		return
	var ready: Array[String] = []
	for target_value: Variant in _live_shader_pending.keys():
		var target_id := String(target_value)
		if not bool(_live_shader_auto_compile.get(target_id, true)):
			continue
		var pending: Dictionary = _live_shader_pending[target_value] as Dictionary
		var remaining: float = float(pending.get("remaining", LIVE_SHADER_DEBOUNCE_S)) - delta
		pending["remaining"] = remaining
		_live_shader_pending[target_value] = pending
		if remaining <= 0.0:
			ready.append(target_id)
	for target_id: String in ready:
		var pending: Dictionary = _live_shader_pending.get(target_id, {}) as Dictionary
		var material: ShaderMaterial = pending.get("material") as ShaderMaterial
		_submit_live_shader_source(target_id, material,
			String(pending.get("source", "")), int(pending.get("profile_id", 0)))


func _submit_live_shader_source(target_id: String, material: ShaderMaterial,
		source: String, expected_profile_id: int = 0) -> void:
	_live_shader_pending.erase(target_id)
	if material == null:
		_set_live_shader_status(target_id, "Live compile cancelled: ShaderMaterial is no longer resident.")
		return
	if not _interest_uses_detailed_surface:
		_set_live_shader_status(target_id, "Live compile cancelled: selected body does not own the detailed runtime material.")
		return
	var profile: Resource = _session.active_planet_profile() as Resource
	if profile == null or not profile.has_method("set_runtime_shader_source"):
		return
	if expected_profile_id != 0 and profile.get_instance_id() != expected_profile_id:
		_set_live_shader_status(target_id, "Live compile cancelled because the active body changed while editing.")
		return
	var preflight: String = _live_shader_preflight(target_id, source)
	if not preflight.is_empty():
		_set_live_shader_status(target_id, "PRE-FLIGHT ERROR — %s" % preflight)
		return

	var candidate := Shader.new()
	candidate.resource_name = "PlanetStudioLive_%s" % target_id.replace(":", "_")
	candidate.code = source
	# Requesting reflection forces Godot to process the new Shader immediately. The
	# engine still owns the definitive compiler diagnostics and reports them to Output.
	candidate.get_shader_uniform_list()
	_live_shader_cache[target_id] = {
		"source": source,
		"shader": candidate,
		"profile_id": profile.get_instance_id(),
	}
	material.shader = candidate
	_uniform_name_cache.clear()
	_session.stage_action("Edit live shader source: %s" % target_id, func() -> void:
		profile.call("set_runtime_shader_source", target_id, source)
	, WorldAuthoringSession.ApplyScope.HOT)
	_apply_runtime_shader_uniforms(profile, target_id, material)
	_set_live_shader_status(target_id,
		"LIVE — source submitted to Godot (%d lines, %.1f KiB). Ctrl+Z/Revert Source restores the previous authoring state." % [
			maxi(1, source.count("\n") + 1),
			float(source.to_utf8_buffer().size()) / 1024.0,
		])


func _clear_live_shader_source(target_id: String, material: ShaderMaterial) -> void:
	var profile: Resource = _session.active_planet_profile() as Resource
	if profile == null or not profile.has_method("clear_runtime_shader_source"):
		return
	_live_shader_pending.erase(target_id)
	_live_shader_cache.erase(target_id)
	_session.stage_action("Revert live shader source: %s" % target_id, func() -> void:
		profile.call("clear_runtime_shader_source", target_id)
	, WorldAuthoringSession.ApplyScope.HOT)
	_uniform_name_cache.clear()
	_apply_runtime_shader_state(target_id, material)
	_set_live_shader_status(target_id, "Source override released; production/path shader restored.")
	_refresh_current_category()


func _on_runtime_shader_path_submitted(path: String, target_id: String,
		material: ShaderMaterial, path_edit: LineEdit) -> void:
	# Choosing a file-backed shader is an explicit source replacement. Clear a live
	# source override first so the requested path becomes visible immediately.
	var profile: Resource = _session.active_planet_profile() as Resource
	if profile != null and profile.has_method("has_runtime_shader_source") \
			and bool(profile.call("has_runtime_shader_source", target_id)):
		profile.call("clear_runtime_shader_source", target_id)
		_live_shader_pending.erase(target_id)
		_live_shader_cache.erase(target_id)
	super._on_runtime_shader_path_submitted(path, target_id, material, path_edit)


func _reset_runtime_shader_target(target_id: String, material: ShaderMaterial) -> void:
	_live_shader_pending.erase(target_id)
	_live_shader_cache.erase(target_id)
	super._reset_runtime_shader_target(target_id, material)


func _apply_all_runtime_shader_state() -> void:
	# Phase 24 keeps the detailed root body resident while an orbital child is
	# selected. Shader ownership must follow that same root, otherwise selecting a
	# moon would silently apply the moon's profile to Asterra's materials.
	var profile: Resource = _runtime_shader_owner_profile()
	if profile == null:
		return
	var materials: Dictionary = _runtime_material_map()
	for target_value: Variant in materials.keys():
		var target_id := String(target_value)
		var material: ShaderMaterial = materials[target_value] as ShaderMaterial
		if material == null:
			continue
		_remember_default_shader(target_id, material)
		_apply_runtime_shader_state_for_profile(profile, target_id, material)


func _apply_runtime_shader_state(target_id: String,
		material: ShaderMaterial) -> void:
	var profile: Resource = _runtime_shader_owner_profile()
	if profile != null:
		_apply_runtime_shader_state_for_profile(profile, target_id, material)


func _apply_runtime_shader_state_for_profile(profile: Resource, target_id: String,
		material: ShaderMaterial) -> void:
	if profile == null or material == null:
		return
	var source: String = String(profile.call("runtime_shader_source", target_id)) \
		if profile.has_method("runtime_shader_source") else ""
	if not source.is_empty():
		var preflight: String = _live_shader_preflight(target_id, source)
		if preflight.is_empty():
			var cached: Dictionary = _live_shader_cache.get(target_id, {}) as Dictionary
			var candidate: Shader = cached.get("shader") as Shader
			if candidate == null or String(cached.get("source", "")) != source \
					or int(cached.get("profile_id", 0)) != profile.get_instance_id():
				candidate = Shader.new()
				candidate.resource_name = "PlanetStudioLive_%s" % target_id.replace(":", "_")
				candidate.code = source
				candidate.get_shader_uniform_list()
				_live_shader_cache[target_id] = {
					"source": source,
					"shader": candidate,
					"profile_id": profile.get_instance_id(),
				}
			if material.shader != candidate:
				material.shader = candidate
		else:
			_assign_runtime_base_shader(profile, target_id, material)
	else:
		_assign_runtime_base_shader(profile, target_id, material)
	_apply_runtime_shader_uniforms(profile, target_id, material)


func _assign_runtime_base_shader(profile: Resource, target_id: String,
		material: ShaderMaterial) -> void:
	var explicit_path: String = String(profile.call("runtime_shader_path", target_id)) \
		if profile.has_method("runtime_shader_path") else ""
	var desired_path: String = explicit_path
	if desired_path.is_empty():
		desired_path = String(_default_shader_paths.get(target_id, ""))
	var current_path: String = material.shader.resource_path if material.shader != null else ""
	if desired_path.is_empty() or desired_path == current_path:
		return
	if not ResourceLoader.exists(desired_path, "Shader"):
		return
	var loaded: Resource = load(desired_path)
	if loaded is Shader:
		material.shader = loaded as Shader


func _apply_runtime_shader_uniforms(profile: Resource, target_id: String,
		material: ShaderMaterial) -> void:
	if material == null or material.shader == null:
		return
	var valid_names: Dictionary = _shader_uniform_name_set(material.shader)
	var overrides: Dictionary = profile.call("runtime_shader_uniform_overrides", target_id) \
		if profile.has_method("runtime_shader_uniform_overrides") else {}
	for uniform_value: Variant in overrides.keys():
		var uniform_name := String(uniform_value)
		if valid_names.has(uniform_name):
			material.set_shader_parameter(StringName(uniform_name), overrides[uniform_value])


func _runtime_shader_owner_profile() -> Resource:
	if _session == null or _session.staged_system == null:
		return null
	var detailed_id: String = ""
	var layer: Node = get_parent()
	var host: Node = layer.get_parent() if layer != null else null
	if host != null:
		detailed_id = String(host.get("_detailed_runtime_body_id"))
	if not detailed_id.is_empty():
		var detailed_body: Resource = _session.staged_system.call("find_body", detailed_id) as Resource
		if detailed_body != null:
			var detailed_profile: Resource = detailed_body.get(&"planet_profile") as Resource
			if detailed_profile != null:
				return detailed_profile
	return _session.active_planet_profile() as Resource


func _runtime_shader_base_path(target_id: String, material: ShaderMaterial) -> String:
	var profile: Resource = _session.active_planet_profile() as Resource
	var explicit_path: String = String(profile.call("runtime_shader_path", target_id)) \
		if profile != null and profile.has_method("runtime_shader_path") else ""
	if not explicit_path.is_empty():
		return explicit_path
	var default_path: String = String(_default_shader_paths.get(target_id, ""))
	if not default_path.is_empty():
		return default_path
	return material.shader.resource_path if material != null and material.shader != null else ""


func _live_shader_preflight(target_id: String, source: String) -> String:
	if source.is_empty():
		return "shader source is empty"
	var byte_count: int = source.to_utf8_buffer().size()
	if byte_count > LIVE_SHADER_MAX_SOURCE_BYTES:
		return "source is %.2f MiB; live limit is %.2f MiB" % [
			float(byte_count) / (1024.0 * 1024.0),
			float(LIVE_SHADER_MAX_SOURCE_BYTES) / (1024.0 * 1024.0),
		]
	var type_regex := RegEx.new()
	var regex_error: Error = type_regex.compile("shader_type\\s+([A-Za-z0-9_]+)\\s*;")
	if regex_error != OK:
		return "internal shader_type validator failed"
	var type_match: RegExMatch = type_regex.search(source)
	if type_match == null:
		return "missing `shader_type ...;` declaration"
	var actual_type: String = type_match.get_string(1)
	var expected_type: String = "sky" if target_id == TARGET_ATMOSPHERE else "spatial"
	if actual_type != expected_type:
		return "%s expects `shader_type %s;`, got `%s`" % [
			target_id, expected_type, actual_type]
	var delimiter_error: String = _balanced_shader_delimiters(source)
	if not delimiter_error.is_empty():
		return delimiter_error
	return ""


func _balanced_shader_delimiters(source: String) -> String:
	var stack := PackedInt32Array()
	var in_line_comment := false
	var in_block_comment := false
	var in_string := false
	var escaped := false
	var index := 0
	while index < source.length():
		var code: int = source.unicode_at(index)
		var next_code: int = source.unicode_at(index + 1) if index + 1 < source.length() else -1
		if in_line_comment:
			if code == 10:
				in_line_comment = false
			index += 1
			continue
		if in_block_comment:
			if code == 42 and next_code == 47:
				in_block_comment = false
				index += 2
				continue
			index += 1
			continue
		if in_string:
			if escaped:
				escaped = false
			elif code == 92:
				escaped = true
			elif code == 34:
				in_string = false
			index += 1
			continue
		if code == 47 and next_code == 47:
			in_line_comment = true
			index += 2
			continue
		if code == 47 and next_code == 42:
			in_block_comment = true
			index += 2
			continue
		if code == 34:
			in_string = true
			index += 1
			continue
		if code == 123 or code == 40 or code == 91:
			stack.append(code)
		elif code == 125 or code == 41 or code == 93:
			if stack.is_empty():
				return "unexpected closing delimiter at character %d" % index
			var opening: int = stack[stack.size() - 1]
			var matches: bool = (opening == 123 and code == 125) \
				or (opening == 40 and code == 41) \
				or (opening == 91 and code == 93)
			if not matches:
				return "mismatched delimiter at character %d" % index
			stack.resize(stack.size() - 1)
		index += 1
	if in_block_comment:
		return "unterminated block comment"
	if in_string:
		return "unterminated string literal"
	if not stack.is_empty():
		return "unclosed delimiter in shader source"
	return ""


func _set_live_shader_status(target_id: String, text: String) -> void:
	_live_shader_status_text[target_id] = text
	var label: Label = _live_shader_status_labels.get(target_id) as Label
	if label != null and is_instance_valid(label):
		label.text = text
