extends "res://scripts/ui/groom_lab_facial_hair.gd"

const GPUHairCardGenerator = preload("res://scripts/character/gpu_hair_card_groom.gd")
const PresetLibrary = preload("res://scripts/character/character_preset_library.gd")
const SKIN_SHADER := preload("res://shaders/character_skin.gdshader")
const EYE_SHADER := preload("res://shaders/character_eye.gdshader")

const PRESET_CATEGORIES := [
	{"key": "hair", "label": "Hair"},
	{"key": "beard", "label": "Beard"},
	{"key": "mustache", "label": "Mustache"},
	{"key": "brows", "label": "Brows"},
	{"key": "eyes", "label": "Eyes"},
	{"key": "skin", "label": "Skin"}
]

var _simple_layer: CanvasLayer
var _simple_status: Label
var _skin_material: ShaderMaterial
var _eye_material: ShaderMaterial
var _selected_lash_variant := 1
var _simple_selectors: Dictionary = {}
var _simple_selector_configs: Dictionary = {}
var _simple_tabs: TabContainer
var _advanced_tabs: TabContainer
var _active_part_index := 0
var _syncing_part_tabs := false
var _preset_category_selector: OptionButton
var _preset_category_badge: Label
var _preset_name: LineEdit
var _preset_status: Label

var _eye_settings := {
	"color": Color("416d86"),
	"pupil_size": 0.34,
	"limbal_strength": 0.72,
	"sclera_roughness": 0.28
}

var _skin_settings := {
	"color": Color("e8a77f"),
	"tone_strength": 0.62,
	"roughness": 0.52,
	"sss_strength": 0.42,
	"pore_strength": 0.22
}

func _late_setup() -> void:
	await get_tree().process_frame
	var root: Node = get_parent()
	_character = root.find_child("AsterraHuman", true, false) as Node3D
	if _character == null:
		await get_tree().process_frame
		_character = root.find_child("AsterraHuman", true, false) as Node3D
	if _character == null:
		push_warning("Groom lab could not find AsterraHuman")
		return

	_collect_meshes(_character)
	_discover_lashes()
	_setup_timers()
	var bounds: AABB = _world_bounds()
	var height: float = maxf(bounds.size.y, 0.5)
	var bottom: float = bounds.position.y
	_ensure_advanced_brow_defaults()
	_apply_full_beard_defaults()
	_setup_character_materials()

	_groom = GPUHairCardGenerator.new()
	_groom.name = "ProceduralGroomGenerator"
	add_child(_groom)
	if not _groom.configure(_character, _meshes, bottom, height, _front_sign, false):
		push_warning("Could not configure procedural character groom")
		return
	_push_settings_to_groom()
	_groom.rebuild_all()

	_setup_ui()
	_apply_lash_variant(_selected_lash_variant if _available_lash_count() > 0 else 0)
	_apply_character_material_settings()
	_sync_advanced_controls_from_settings()
	_refresh_status()

func _apply_full_beard_defaults() -> void:
	_mustache_settings["density"] = 1.0
	_mustache_settings["width"] = 0.86
	_mustache_settings["thickness"] = 1.15
	_mustache_settings["length"] = 0.0055
	_mustache_settings["strand_width"] = 0.00042
	_mustache_settings["middle_gap"] = 0.007
	_mustache_settings["droop"] = 0.18
	_mustache_settings["height_offset"] = 0.0
	_mustache_settings["forward_offset"] = 0.00055
	_mustache_settings["messiness"] = 0.12

	_beard_settings["density"] = 1.0
	_beard_settings["density_multiplier"] = 3.0
	_beard_settings["coverage"] = 1.0
	_beard_settings["fullness"] = 1.30
	_beard_settings["length"] = 0.016
	_beard_settings["chin_length"] = 0.022
	_beard_settings["strand_width"] = 0.00065
	_beard_settings["height_offset"] = 0.0
	_beard_settings["forward_offset"] = 0.00055
	_beard_settings["messiness"] = 0.18

	var default_facial_color: Color = Color(_hair_settings.get("root_color", _hair_settings["color"])).darkened(0.10)
	_mustache_settings["color"] = default_facial_color
	_beard_settings["color"] = default_facial_color

func _push_settings_to_groom() -> void:
	_groom.set("hair_settings", _hair_settings.duplicate(true))
	_groom.set("brow_settings", _brow_settings.duplicate(true))
	_groom.set("mustache_settings", _mustache_settings.duplicate(true))
	_groom.set("beard_settings", _beard_settings.duplicate(true))
	_groom.call("_apply_material_settings")

func _setup_ui() -> void:
	super._setup_ui()
	var advanced_layer: CanvasLayer = get_node_or_null("GroomLabUI") as CanvasLayer
	var column: VBoxContainer = _find_groom_column(advanced_layer)
	if column == null:
		push_warning("Could not find advanced groom UI column")
		return

	_rename_label_recursive(advanced_layer, "Side length", "Beard hair length")
	_add_section(column, "BEARD DENSITY")
	_add_slider(column, "Beard density multiplier", 1.0, 6.0, 0.25, float(_beard_settings.get("density_multiplier", 3.0)), "×", func(v: float) -> void:
		_beard_settings["density_multiplier"] = v
		_schedule_facial_hair()
	)

	_add_section(column, "EYES / SKIN MATERIALS")
	var eye_picker := _add_color_control(column, "Eye color", Color(_eye_settings["color"]), func(color: Color) -> void:
		_eye_settings["color"] = color
		_apply_character_material_settings()
	)
	_advanced_controls[_advanced_control_key("Eye color")] = eye_picker
	_add_slider(column, "Pupil size", 0.15, 0.70, 0.01, float(_eye_settings["pupil_size"]), "", func(v: float) -> void:
		_eye_settings["pupil_size"] = v
		_apply_character_material_settings()
	)
	_add_slider(column, "Limbal ring", 0.0, 1.0, 0.01, float(_eye_settings["limbal_strength"]), "", func(v: float) -> void:
		_eye_settings["limbal_strength"] = v
		_apply_character_material_settings()
	)
	var skin_picker := _add_color_control(column, "Skin color", Color(_skin_settings["color"]), func(color: Color) -> void:
		_skin_settings["color"] = color
		_apply_character_material_settings()
	)
	_advanced_controls[_advanced_control_key("Skin color")] = skin_picker
	_add_slider(column, "Skin tone mix", 0.0, 1.0, 0.01, float(_skin_settings["tone_strength"]), "", func(v: float) -> void:
		_skin_settings["tone_strength"] = v
		_apply_character_material_settings()
	)
	_add_slider(column, "Skin roughness", 0.15, 0.95, 0.01, float(_skin_settings["roughness"]), "", func(v: float) -> void:
		_skin_settings["roughness"] = v
		_apply_character_material_settings()
	)
	_add_slider(column, "Subsurface scattering", 0.0, 1.0, 0.01, float(_skin_settings["sss_strength"]), "", func(v: float) -> void:
		_skin_settings["sss_strength"] = v
		_apply_character_material_settings()
	)

	var preset_authoring := _build_preset_authoring_ui(column)
	var note := Label.new()
	note.text = "Advanced mode exposes every groom and material parameter. Save individual looks as reusable presets for Simple mode."
	note.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	note.modulate = Color(0.63, 0.70, 0.76)
	note.add_theme_font_size_override("font_size", 11)
	column.add_child(note)

	var mode_row := _make_mode_row(false)
	column.add_child(mode_row)
	column.move_child(mode_row, 0)
	column.move_child(preset_authoring, 1)
	_organize_advanced_tabs(column, mode_row, preset_authoring)
	_build_simple_ui()
	_set_editor_mode(false)

func _build_preset_authoring_ui(parent: VBoxContainer) -> VBoxContainer:
	var box := VBoxContainer.new()
	box.name = "PresetAuthoring"
	box.add_theme_constant_override("separation", 8)
	parent.add_child(box)
	box.add_child(HSeparator.new())
	var title := Label.new()
	title.text = "CREATE SIMPLE PRESET"
	title.modulate = Color(0.58, 0.68, 0.77)
	title.add_theme_font_size_override("font_size", 13)
	box.add_child(title)
	var hint := Label.new()
	hint.text = "Tune the current look below, choose which part to capture, and save it into Simple mode. Saving the same category and name updates that preset."
	hint.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	hint.modulate = Color(0.63, 0.70, 0.76)
	hint.add_theme_font_size_override("font_size", 11)
	box.add_child(hint)
	_preset_category_badge = Label.new()
	_preset_category_badge.text = "Saving: Hair preset"
	_preset_category_badge.modulate = Color(0.78, 0.84, 0.90)
	box.add_child(_preset_category_badge)
	_preset_category_selector = OptionButton.new()
	for category in PRESET_CATEGORIES:
		_preset_category_selector.add_item(str(category["label"]))
		_preset_category_selector.set_item_metadata(_preset_category_selector.item_count - 1, category["key"])
	_preset_name = LineEdit.new()
	_preset_name.placeholder_text = "Preset name, e.g. Heavy Arched"
	box.add_child(_preset_name)
	var save_preset_button := Button.new()
	save_preset_button.text = "Save current part as preset"
	save_preset_button.pressed.connect(_save_current_category_preset)
	box.add_child(save_preset_button)
	_preset_status = Label.new()
	_preset_status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_preset_status.modulate = Color(0.70, 0.78, 0.84)
	box.add_child(_preset_status)
	return box

func _make_mode_row(simple_active: bool) -> HBoxContainer:
	var row := HBoxContainer.new()
	row.name = "EditorModeRow"
	row.add_theme_constant_override("separation", 8)
	var simple := Button.new()
	simple.text = "Simple"
	simple.disabled = simple_active
	simple.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	simple.pressed.connect(func() -> void: _set_editor_mode(false))
	row.add_child(simple)
	var advanced := Button.new()
	advanced.text = "Advanced"
	advanced.disabled = not simple_active
	advanced.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	advanced.pressed.connect(func() -> void: _set_editor_mode(true))
	row.add_child(advanced)
	return row

func _create_part_pages(tab_container: TabContainer, add_headers: bool) -> Dictionary:
	var pages := {}
	for entry in PRESET_CATEGORIES:
		var page := VBoxContainer.new()
		page.name = str(entry["label"])
		page.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		page.size_flags_vertical = Control.SIZE_EXPAND_FILL
		page.add_theme_constant_override("separation", 9)
		tab_container.add_child(page)
		pages[entry["key"]] = page
		if add_headers:
			var header := Label.new()
			header.text = "%s CONTROLS" % str(entry["label"]).to_upper()
			header.modulate = Color(0.58, 0.68, 0.77)
			header.add_theme_font_size_override("font_size", 13)
			page.add_child(header)
	return pages

func _organize_advanced_tabs(column: VBoxContainer, mode_row: Control, preset_authoring: Control) -> void:
	var movable := column.get_children()
	_advanced_tabs = TabContainer.new()
	_advanced_tabs.name = "AdvancedPartTabs"
	_advanced_tabs.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_advanced_tabs.size_flags_vertical = Control.SIZE_EXPAND_FILL
	column.add_child(_advanced_tabs)
	var pages := _create_part_pages(_advanced_tabs, true)
	var current_category := "hair"
	var current_section := ""
	var pending_separator: Control = null
	for child_variant in movable:
		var child: Node = child_variant
		if child == mode_row or child == preset_authoring:
			continue
		if child is HSeparator:
			if pending_separator != null:
				_move_control_to_page(pending_separator, pages[current_category])
			pending_separator = child as Control
			continue
		if child is Label:
			var section_text := (child as Label).text
			var section_category := _advanced_category_for_section(section_text)
			if not section_category.is_empty():
				current_section = section_text
				current_category = section_category
		if current_section == "EYES / SKIN MATERIALS" and _node_contains_label(child, "Skin color"):
			current_category = "skin"
		if pending_separator != null:
			_move_control_to_page(pending_separator, pages[current_category])
			pending_separator = null
		_move_control_to_page(child, pages[current_category])
	if pending_separator != null:
		_move_control_to_page(pending_separator, pages[current_category])
	column.move_child(_advanced_tabs, 2)
	_advanced_tabs.tab_changed.connect(_on_part_tab_changed)
	_advanced_tabs.current_tab = _active_part_index

func _move_control_to_page(control: Node, page_variant: Variant) -> void:
	var page := page_variant as VBoxContainer
	if page == null or control.get_parent() == page:
		return
	var old_parent := control.get_parent()
	if old_parent != null:
		old_parent.remove_child(control)
	page.add_child(control)

func _advanced_category_for_section(section: String) -> String:
	match section:
		"GROOM", "PROCEDURAL HAIR", "CALIBRATION": return "hair"
		"EYELASHES", "EYES / SKIN MATERIALS": return "eyes"
		"PROCEDURAL BROWS", "BROW SHAPE / FALLOFF": return "brows"
		"FACIAL HAIR", "MUSTACHE": return "mustache"
		"BEARD", "BEARD DENSITY": return "beard"
	return ""

func _node_contains_label(node: Node, exact_text: String) -> bool:
	if node is Label and (node as Label).text == exact_text:
		return true
	for child in node.get_children():
		if _node_contains_label(child, exact_text):
			return true
	return false

func _on_part_tab_changed(index: int) -> void:
	if _syncing_part_tabs:
		return
	_active_part_index = clampi(index, 0, PRESET_CATEGORIES.size() - 1)
	_syncing_part_tabs = true
	for tabs in [_simple_tabs, _advanced_tabs]:
		var tab_container := tabs as TabContainer
		if tab_container != null and tab_container.current_tab != _active_part_index:
			tab_container.current_tab = _active_part_index
	if _preset_category_selector != null:
		_preset_category_selector.select(_active_part_index)
	_syncing_part_tabs = false
	var part_key := str(PRESET_CATEGORIES[_active_part_index]["key"])
	if _preset_category_badge != null:
		_preset_category_badge.text = "Saving: %s preset" % str(PRESET_CATEGORIES[_active_part_index]["label"])
	var editor := get_parent()
	if editor != null and editor.has_method("focus_customization_part"):
		editor.call("focus_customization_part", part_key)

func _build_simple_ui() -> void:
	_simple_selectors.clear()
	_simple_selector_configs.clear()
	_simple_layer = CanvasLayer.new()
	_simple_layer.name = "SimpleCharacterUI"
	add_child(_simple_layer)
	var panel := PanelContainer.new()
	panel.set_anchors_preset(Control.PRESET_LEFT_WIDE)
	panel.offset_left = 18.0
	panel.offset_top = 94.0
	panel.offset_right = 500.0
	panel.offset_bottom = -18.0
	panel.add_theme_stylebox_override("panel", _panel_style(Color(0.045, 0.065, 0.085, 0.96)))
	_simple_layer.add_child(panel)
	var margin := MarginContainer.new()
	margin.add_theme_constant_override("margin_left", 14)
	margin.add_theme_constant_override("margin_right", 14)
	margin.add_theme_constant_override("margin_top", 14)
	margin.add_theme_constant_override("margin_bottom", 14)
	panel.add_child(margin)
	var outer := VBoxContainer.new()
	outer.add_theme_constant_override("separation", 10)
	margin.add_child(outer)
	outer.add_child(_make_mode_row(true))

	var title := Label.new()
	title.text = "CHARACTER PRESETS"
	title.modulate = Color(0.58, 0.68, 0.77)
	title.add_theme_font_size_override("font_size", 13)
	outer.add_child(title)
	var hint := Label.new()
	hint.text = "Choose a part, then apply a built-in or player-created preset."
	hint.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	hint.modulate = Color(0.63, 0.70, 0.76)
	outer.add_child(hint)

	_simple_tabs = TabContainer.new()
	_simple_tabs.name = "SimplePartTabs"
	_simple_tabs.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_simple_tabs.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_simple_tabs.tab_changed.connect(_on_part_tab_changed)
	outer.add_child(_simple_tabs)
	var pages := _create_part_pages(_simple_tabs, false)
	_add_simple_selector(pages["hair"], "Hair preset", "hair", ["Bald", "Cropped", "Natural", "Swept back", "Side part", "Curly", "Long"], 2, _apply_hair_preset)
	_add_simple_selector(pages["beard"], "Beard preset", "beard", ["Clean shaven", "Stubble", "Short boxed", "Full beard", "Long beard"], 3, _apply_beard_preset)
	_add_simple_selector(pages["mustache"], "Mustache preset", "mustache", ["None", "Pencil", "Natural", "Chevron", "Handlebar"], 2, _apply_mustache_preset)
	_add_simple_selector(pages["brows"], "Brows preset", "brows", ["Soft", "Natural", "Straight", "Arched", "Strong"], 1, _apply_brow_preset)
	_add_simple_selector(pages["eyes"], "Eye preset", "eyes", ["Dark brown", "Hazel", "Green", "Blue", "Gray", "Amber"], 3, _apply_eye_preset)
	_add_simple_color(pages["eyes"], "Custom eye color", Color(_eye_settings["color"]), func(color: Color) -> void:
		_eye_settings["color"] = color
		_apply_character_material_settings()
	)
	_add_simple_selector(pages["skin"], "Skin preset", "skin", ["Porcelain", "Light", "Medium", "Olive", "Brown", "Deep"], 1, _apply_skin_preset)
	_add_simple_color(pages["skin"], "Custom skin color", Color(_skin_settings["color"]), func(color: Color) -> void:
		_skin_settings["color"] = color
		_apply_character_material_settings()
	)
	_simple_status = Label.new()
	_simple_status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_simple_status.modulate = Color(0.70, 0.78, 0.84)
	outer.add_child(_simple_status)
	_simple_tabs.current_tab = _active_part_index

func _add_simple_selector(parent: VBoxContainer, label_text: String, category: String, options: Array, selected: int, callback: Callable) -> OptionButton:
	var label := Label.new()
	label.text = label_text
	label.modulate = Color(0.72, 0.78, 0.84)
	parent.add_child(label)
	var selector := OptionButton.new()
	_simple_selectors[category] = selector
	_simple_selector_configs[category] = {
		"options": options.duplicate(),
		"default": selected,
		"callback": callback
	}
	_populate_simple_selector(category)
	selector.item_selected.connect(func(index: int) -> void: _on_simple_preset_selected(category, index))
	parent.add_child(selector)
	return selector

func _populate_simple_selector(category: String, select_preset_id: String = "") -> void:
	var selector: OptionButton = _simple_selectors.get(category) as OptionButton
	var config: Dictionary = _simple_selector_configs.get(category, {})
	if selector == null or config.is_empty():
		return
	selector.set_block_signals(true)
	selector.clear()
	var options: Array = config.get("options", [])
	for builtin_index in options.size():
		selector.add_item(str(options[builtin_index]))
		selector.set_item_metadata(selector.item_count - 1, {"source": "built_in", "index": builtin_index})
	var selected_index := clampi(int(config.get("default", 0)), 0, maxi(selector.item_count - 1, 0))
	var saved_presets: Array[Dictionary] = PresetLibrary.list_presets(category)
	if not saved_presets.is_empty():
		selector.add_separator("SAVED PRESETS")
		for preset in saved_presets:
			selector.add_item("★ %s" % str(preset.get("name", "Preset")))
			var item_index := selector.item_count - 1
			selector.set_item_metadata(item_index, {"source": "saved", "id": preset.get("id", "")})
			if str(preset.get("id", "")) == select_preset_id:
				selected_index = item_index
	if selector.item_count > 0:
		selector.select(selected_index)
	selector.set_block_signals(false)

func _on_simple_preset_selected(category: String, item_index: int) -> void:
	var selector: OptionButton = _simple_selectors.get(category) as OptionButton
	var config: Dictionary = _simple_selector_configs.get(category, {})
	if selector == null or config.is_empty() or item_index < 0:
		return
	var metadata = selector.get_item_metadata(item_index)
	if not (metadata is Dictionary):
		return
	var selection: Dictionary = metadata
	if str(selection.get("source", "")) == "saved":
		_apply_saved_preset(category, str(selection.get("id", "")))
	else:
		var callback: Callable = config.get("callback", Callable())
		if callback.is_valid():
			callback.call(int(selection.get("index", 0)))

func _apply_saved_preset(category: String, preset_id: String) -> void:
	var preset := PresetLibrary.load_preset(category, preset_id)
	if preset.is_empty():
		_set_simple_status("That saved preset could not be loaded.")
		return
	var settings_variant = preset.get("settings", {})
	if not (settings_variant is Dictionary):
		_set_simple_status("That saved preset has invalid settings.")
		return
	var settings: Dictionary = settings_variant
	match category:
		"hair":
			_restore_dictionary(_hair_settings, settings)
			# Presets saved by the original rigid groom had one color and one
			# hairline. Promote them without invalidating player-created files.
			if settings.has("color") and not settings.has("root_color"):
				var legacy_color := Color(settings["color"])
				_hair_settings["root_color"] = legacy_color
				_hair_settings["tip_color"] = legacy_color.lightened(0.18)
			if settings.has("hairline") and not settings.has("front_hairline"):
				_hair_settings["front_hairline"] = settings["hairline"]
			_apply_hair_now()
		"beard":
			_restore_dictionary(_beard_settings, settings)
			_apply_facial_hair_now()
		"mustache":
			_restore_dictionary(_mustache_settings, settings)
			_apply_facial_hair_now()
		"brows":
			_restore_dictionary(_brow_settings, settings)
			_apply_brows_now()
		"eyes":
			_restore_dictionary(_eye_settings, settings)
			_apply_character_material_settings()
		"skin":
			_restore_dictionary(_skin_settings, settings)
			_apply_character_material_settings()
		_:
			return
	_sync_advanced_controls_from_settings()
	_set_simple_status("Applied saved %s preset ‘%s’." % [_preset_category_label(category).to_lower(), str(preset.get("name", "Preset"))])

func _save_current_category_preset() -> void:
	if _preset_category_selector == null or _preset_category_selector.selected < 0:
		return
	var category := str(_preset_category_selector.get_item_metadata(_preset_category_selector.selected))
	var display_name := _preset_name.text.strip_edges() if _preset_name != null else ""
	var settings := _settings_for_preset_category(category)
	var result := PresetLibrary.save_preset(category, display_name, settings)
	if not bool(result.get("ok", false)):
		_set_simple_status(str(result.get("message", "Could not save preset.")))
		return
	var preset: Dictionary = result.get("preset", {})
	_populate_simple_selector(category, str(preset.get("id", "")))
	_set_simple_status("Saved %s preset ‘%s’. It is now available in Simple mode." % [_preset_category_label(category).to_lower(), str(preset.get("name", display_name))])

func _settings_for_preset_category(category: String) -> Dictionary:
	match category:
		"hair": return _hair_settings.duplicate(true)
		"beard": return _beard_settings.duplicate(true)
		"mustache": return _mustache_settings.duplicate(true)
		"brows": return _brow_settings.duplicate(true)
		"eyes": return _eye_settings.duplicate(true)
		"skin": return _skin_settings.duplicate(true)
	return {}

func _preset_category_label(category: String) -> String:
	for entry in PRESET_CATEGORIES:
		if str(entry.get("key", "")) == category:
			return str(entry.get("label", category.capitalize()))
	return category.capitalize()

func _add_simple_color(parent: VBoxContainer, label_text: String, initial: Color, callback: Callable) -> void:
	var row := HBoxContainer.new()
	parent.add_child(row)
	var label := Label.new()
	label.text = label_text
	label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(label)
	var picker := ColorPickerButton.new()
	picker.color = initial
	picker.custom_minimum_size = Vector2(92.0, 32.0)
	picker.color_changed.connect(func(color: Color) -> void: callback.call(color))
	row.add_child(picker)

func _set_editor_mode(advanced: bool) -> void:
	var advanced_layer: CanvasLayer = get_node_or_null("GroomLabUI") as CanvasLayer
	if advanced_layer != null:
		advanced_layer.visible = advanced
	if _simple_layer != null:
		_simple_layer.visible = not advanced
	var morph_panel: Control = get_parent().get_node_or_null("StudioUI/MorphPanel") as Control
	if morph_panel != null:
		morph_panel.visible = advanced
	if advanced:
		_sync_advanced_controls_from_settings()
	_on_part_tab_changed(_active_part_index)

func _apply_hair_preset(index: int) -> void:
	var presets := [
		{"enabled": false},
		{"enabled": true, "style": 4, "density": 0.78, "length": 0.012, "width": 0.00022, "tip_thickness": 0.10, "curl": 0.02, "gravity": 0.08, "front_hairline": 0.54, "side_hairline": 0.72, "back_hairline": 0.78, "root_lift": 0.0007, "physics_stiffness": 0.92},
		{"enabled": true, "style": 0, "density": 0.72, "length": 0.09, "width": 0.00032, "tip_thickness": 0.08, "curl": 0.10, "gravity": 0.38, "front_hairline": 0.52, "side_hairline": 0.68, "back_hairline": 0.76, "root_lift": 0.0012, "physics_stiffness": 0.68},
		{"enabled": true, "style": 1, "density": 0.76, "length": 0.12, "width": 0.00030, "tip_thickness": 0.07, "curl": 0.05, "gravity": 0.24, "front_hairline": 0.55, "side_hairline": 0.70, "back_hairline": 0.78, "root_lift": 0.0015, "physics_stiffness": 0.78},
		{"enabled": true, "style": 2, "density": 0.74, "length": 0.13, "width": 0.00030, "tip_thickness": 0.07, "curl": 0.08, "gravity": 0.42, "front_hairline": 0.53, "side_hairline": 0.69, "back_hairline": 0.77, "root_lift": 0.0013, "physics_stiffness": 0.70},
		{"enabled": true, "style": 0, "density": 0.82, "length": 0.11, "width": 0.00034, "tip_thickness": 0.12, "curl": 0.72, "gravity": 0.36, "front_hairline": 0.51, "side_hairline": 0.70, "back_hairline": 0.78, "root_lift": 0.0016, "physics_stiffness": 0.56},
		{"enabled": true, "style": 0, "density": 0.88, "length": 0.26, "width": 0.00034, "tip_thickness": 0.06, "curl": 0.18, "gravity": 0.80, "front_hairline": 0.52, "side_hairline": 0.75, "back_hairline": 0.84, "root_lift": 0.0012, "physics_stiffness": 0.42, "physics_damping": 0.94}
	]
	_merge_settings(_hair_settings, presets[clampi(index, 0, presets.size() - 1)])
	_apply_hair_now()
	_sync_advanced_controls_from_settings()

func _apply_beard_preset(index: int) -> void:
	var presets := [
		{"enabled": false},
		{"enabled": true, "density": 0.72, "density_multiplier": 1.25, "coverage": 0.55, "fullness": 0.72, "length": 0.0022, "chin_length": 0.0028, "strand_width": 0.00042, "messiness": 0.10},
		{"enabled": true, "density": 0.92, "density_multiplier": 2.0, "coverage": 0.76, "fullness": 1.0, "length": 0.008, "chin_length": 0.011, "strand_width": 0.00055, "messiness": 0.14},
		{"enabled": true, "density": 1.0, "density_multiplier": 3.0, "coverage": 1.0, "fullness": 1.30, "length": 0.016, "chin_length": 0.022, "strand_width": 0.00065, "messiness": 0.18},
		{"enabled": true, "density": 1.0, "density_multiplier": 3.6, "coverage": 0.96, "fullness": 1.35, "length": 0.032, "chin_length": 0.052, "strand_width": 0.00072, "messiness": 0.24}
	]
	_merge_settings(_beard_settings, presets[clampi(index, 0, presets.size() - 1)])
	_apply_facial_hair_now()
	_sync_advanced_controls_from_settings()

func _apply_mustache_preset(index: int) -> void:
	var presets := [
		{"enabled": false},
		{"enabled": true, "density": 0.62, "width": 0.78, "thickness": 0.44, "length": 0.0035, "strand_width": 0.00028, "middle_gap": 0.010, "droop": 0.05, "messiness": 0.08},
		{"enabled": true, "density": 1.0, "width": 0.86, "thickness": 1.15, "length": 0.0055, "strand_width": 0.00042, "middle_gap": 0.007, "droop": 0.18, "messiness": 0.12},
		{"enabled": true, "density": 1.0, "width": 1.0, "thickness": 1.55, "length": 0.009, "strand_width": 0.00052, "middle_gap": 0.004, "droop": 0.34, "messiness": 0.12},
		{"enabled": true, "density": 0.94, "width": 1.28, "thickness": 1.0, "length": 0.018, "strand_width": 0.00050, "middle_gap": 0.008, "droop": 0.66, "messiness": 0.16}
	]
	_merge_settings(_mustache_settings, presets[clampi(index, 0, presets.size() - 1)])
	_apply_facial_hair_now()
	_sync_advanced_controls_from_settings()

func _apply_brow_preset(index: int) -> void:
	var presets := [
		{"enabled": true, "density": 0.58, "width": 0.70, "thickness": 0.48, "arch": 0.28, "strand_width": 0.00036, "messiness": 0.08},
		{"enabled": true, "density": 1.0, "width": 0.73, "thickness": 0.67, "arch": 0.33, "strand_width": 0.00045, "messiness": 0.16},
		{"enabled": true, "density": 0.90, "width": 0.82, "thickness": 0.62, "arch": 0.08, "strand_width": 0.00044, "messiness": 0.10},
		{"enabled": true, "density": 0.88, "width": 0.78, "thickness": 0.64, "arch": 0.82, "strand_width": 0.00043, "messiness": 0.12},
		{"enabled": true, "density": 1.0, "width": 0.86, "thickness": 1.12, "arch": 0.30, "strand_width": 0.00058, "messiness": 0.20}
	]
	_merge_settings(_brow_settings, presets[clampi(index, 0, presets.size() - 1)])
	_apply_brows_now()
	_sync_advanced_controls_from_settings()

func _apply_eye_preset(index: int) -> void:
	var colors := [Color("321d12"), Color("8a6a2f"), Color("47704d"), Color("416d86"), Color("77838a"), Color("b77928")]
	_eye_settings["color"] = colors[clampi(index, 0, colors.size() - 1)]
	_apply_character_material_settings()
	_sync_advanced_controls_from_settings()

func _apply_skin_preset(index: int) -> void:
	var colors := [Color("f4c7b0"), Color("e8a77f"), Color("c9825c"), Color("a87347"), Color("75452f"), Color("3f241d")]
	_skin_settings["color"] = colors[clampi(index, 0, colors.size() - 1)]
	_skin_settings["tone_strength"] = 0.54 if index <= 1 else 0.72
	_apply_character_material_settings()
	_sync_advanced_controls_from_settings()

func _merge_settings(target: Dictionary, values: Dictionary) -> void:
	for key in values.keys():
		target[key] = values[key]

func _setup_character_materials() -> void:
	for mesh_instance in _meshes:
		if mesh_instance.mesh == null or mesh_instance.mesh.get_surface_count() <= 0:
			continue
		var lower_name := mesh_instance.name.to_lower()
		if lower_name.contains("high-poly"):
			_eye_material = ShaderMaterial.new()
			_eye_material.shader = EYE_SHADER
			_configure_eye_geometry(mesh_instance)
			mesh_instance.set_surface_override_material(0, _eye_material)
		elif lower_name == "human_export_copy" or lower_name.contains("human.body"):
			var original := mesh_instance.get_active_material(0) as BaseMaterial3D
			if original == null or original.albedo_texture == null:
				continue
			_skin_material = ShaderMaterial.new()
			_skin_material.shader = SKIN_SHADER
			_skin_material.set_shader_parameter("albedo_texture", original.albedo_texture)
			mesh_instance.set_surface_override_material(0, _skin_material)
	_apply_character_material_settings()

func _configure_eye_geometry(mesh_instance: MeshInstance3D) -> void:
	var arrays: Array = mesh_instance.mesh.surface_get_arrays(0)
	if arrays.size() <= Mesh.ARRAY_VERTEX or arrays[Mesh.ARRAY_VERTEX] == null:
		return
	var vertices: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
	var left := Vector3.ZERO
	var right := Vector3.ZERO
	var left_depth := -INF
	var right_depth := -INF
	for vertex in vertices:
		var depth := vertex.z * _front_sign
		if vertex.x < 0.0 and depth > left_depth:
			left_depth = depth
			left = vertex
		elif vertex.x >= 0.0 and depth > right_depth:
			right_depth = depth
			right = vertex
	_eye_material.set_shader_parameter("left_iris_center", left)
	_eye_material.set_shader_parameter("right_iris_center", right)
	_eye_material.set_shader_parameter("iris_radius", mesh_instance.get_aabb().size.y * 0.203)

func _apply_character_material_settings() -> void:
	if _eye_material != null:
		_eye_material.set_shader_parameter("iris_color", Color(_eye_settings["color"]))
		_eye_material.set_shader_parameter("pupil_size", float(_eye_settings["pupil_size"]))
		_eye_material.set_shader_parameter("limbal_strength", float(_eye_settings["limbal_strength"]))
		_eye_material.set_shader_parameter("sclera_roughness", float(_eye_settings["sclera_roughness"]))
	if _skin_material != null:
		_skin_material.set_shader_parameter("skin_tone", Color(_skin_settings["color"]))
		_skin_material.set_shader_parameter("tone_strength", float(_skin_settings["tone_strength"]))
		_skin_material.set_shader_parameter("skin_roughness", float(_skin_settings["roughness"]))
		_skin_material.set_shader_parameter("sss_strength", float(_skin_settings["sss_strength"]))
		_skin_material.set_shader_parameter("pore_strength", float(_skin_settings["pore_strength"]))

func _restore_dictionary(target: Dictionary, source_variant: Variant) -> void:
	if not (source_variant is Dictionary):
		return
	var source: Dictionary = source_variant
	for key in source.keys():
		target[key] = source[key]

func _set_simple_status(text: String) -> void:
	if _simple_status != null:
		_simple_status.text = text
	if _preset_status != null:
		_preset_status.text = text

func _apply_lash_variant(variant: int) -> void:
	_selected_lash_variant = variant
	super._apply_lash_variant(variant)

func _sync_advanced_controls_from_settings() -> void:
	_set_advanced_control("EYELASHES", "Lash style", _selected_lash_variant)
	_set_advanced_control("EYELASHES", "Lash tint", _lash_tint)
	_set_advanced_control("PROCEDURAL HAIR", "Enabled", _hair_settings["enabled"])
	_set_advanced_control("PROCEDURAL HAIR", "Style", _hair_settings["style"])
	_set_advanced_control("PROCEDURAL HAIR", "Root color", _hair_settings["root_color"])
	_set_advanced_control("PROCEDURAL HAIR", "Tip color", _hair_settings["tip_color"])
	_set_advanced_control("PROCEDURAL HAIR", "Color gradient bias", _hair_settings["gradient_bias"])
	_set_advanced_control("PROCEDURAL HAIR", "Density", _hair_settings["density"])
	_set_advanced_control("PROCEDURAL HAIR", "Length", float(_hair_settings["length"]) * 100.0)
	_set_advanced_control("PROCEDURAL HAIR", "Root thickness", float(_hair_settings["width"]) * 1000.0)
	_set_advanced_control("PROCEDURAL HAIR", "Tip thickness", float(_hair_settings["tip_thickness"]) * 100.0)
	_set_advanced_control("PROCEDURAL HAIR", "Curl", _hair_settings["curl"])
	_set_advanced_control("PROCEDURAL HAIR", "Gravity / fall", _hair_settings["gravity"])
	_set_advanced_control("PROCEDURAL HAIR", "Forehead start", _hair_settings["front_hairline"])
	_set_advanced_control("PROCEDURAL HAIR", "Side start", _hair_settings["side_hairline"])
	_set_advanced_control("PROCEDURAL HAIR", "Back start", _hair_settings["back_hairline"])
	_set_advanced_control("PROCEDURAL HAIR", "Root lift", float(_hair_settings["root_lift"]) * 1000.0)
	_set_advanced_control("PROCEDURAL HAIR", "Secondary-motion physics", _hair_settings["physics_enabled"])
	_set_advanced_control("PROCEDURAL HAIR", "Physics stiffness", _hair_settings["physics_stiffness"])
	_set_advanced_control("PROCEDURAL HAIR", "Physics damping", _hair_settings["physics_damping"])
	_set_advanced_control("PROCEDURAL HAIR", "Physics gravity", _hair_settings["physics_gravity"])
	_set_advanced_control("PROCEDURAL HAIR", "Wind response", _hair_settings["wind_response"])
	_set_advanced_control("PROCEDURAL BROWS", "Enabled", _brow_settings["enabled"])
	_set_advanced_control("PROCEDURAL BROWS", "Brow color", _brow_settings["color"])
	_set_advanced_control("PROCEDURAL BROWS", "Density", _brow_settings["density"])
	_set_advanced_control("PROCEDURAL BROWS", "Brow width", _brow_settings["width"])
	_set_advanced_control("PROCEDURAL BROWS", "Render strand width", float(_brow_settings["strand_width"]) * 1000.0)
	_set_advanced_control("PROCEDURAL BROWS", "Arch", _brow_settings["arch"])
	_set_advanced_control("PROCEDURAL BROWS", "Vertical offset", float(_brow_settings["height_offset"]) * 1000.0)
	_set_advanced_control("PROCEDURAL BROWS", "Forward offset", float(_brow_settings["forward_offset"]) * 1000.0)
	_set_advanced_control("BROW SHAPE / FALLOFF", "Middle spacing", float(_brow_settings.get("middle_spacing", 0.008)) * 1000.0)
	_set_advanced_control("BROW SHAPE / FALLOFF", "Brow thickness", _brow_settings.get("thickness", 0.67))
	_set_advanced_control("BROW SHAPE / FALLOFF", "Inner fading ratio", _brow_settings.get("inner_fade_ratio", 0.14))
	_set_advanced_control("BROW SHAPE / FALLOFF", "Outside fading ratio", _brow_settings.get("outer_fade_ratio", 0.18))
	_set_advanced_control("BROW SHAPE / FALLOFF", "Messiness", _brow_settings.get("messiness", 0.16))
	_set_advanced_control("MUSTACHE", "Enabled", _mustache_settings["enabled"])
	_set_advanced_control("MUSTACHE", "Density", _mustache_settings["density"])
	_set_advanced_control("MUSTACHE", "Width", _mustache_settings["width"])
	_set_advanced_control("MUSTACHE", "Thickness", _mustache_settings["thickness"])
	_set_advanced_control("MUSTACHE", "Hair length", float(_mustache_settings["length"]) * 1000.0)
	_set_advanced_control("MUSTACHE", "Render strand width", float(_mustache_settings["strand_width"]) * 1000.0)
	_set_advanced_control("MUSTACHE", "Middle gap", float(_mustache_settings["middle_gap"]) * 1000.0)
	_set_advanced_control("MUSTACHE", "Outer droop", _mustache_settings["droop"])
	_set_advanced_control("MUSTACHE", "Vertical offset", float(_mustache_settings["height_offset"]) * 1000.0)
	_set_advanced_control("MUSTACHE", "Surface offset", float(_mustache_settings["forward_offset"]) * 1000.0)
	_set_advanced_control("MUSTACHE", "Messiness", _mustache_settings["messiness"])
	_set_advanced_control("BEARD", "Enabled", _beard_settings["enabled"])
	_set_advanced_control("BEARD", "Density", _beard_settings["density"])
	_set_advanced_control("BEARD", "Cheek coverage", _beard_settings["coverage"])
	_set_advanced_control("BEARD", "Jaw fullness", _beard_settings["fullness"])
	_set_advanced_control("BEARD", "Side length", float(_beard_settings["length"]) * 1000.0)
	_set_advanced_control("BEARD", "Chin length", float(_beard_settings["chin_length"]) * 1000.0)
	_set_advanced_control("BEARD", "Render strand width", float(_beard_settings["strand_width"]) * 1000.0)
	_set_advanced_control("BEARD", "Vertical offset", float(_beard_settings["height_offset"]) * 1000.0)
	_set_advanced_control("BEARD", "Surface offset", float(_beard_settings["forward_offset"]) * 1000.0)
	_set_advanced_control("BEARD", "Messiness", _beard_settings["messiness"])
	_set_advanced_control("BEARD DENSITY", "Beard density multiplier", _beard_settings["density_multiplier"])
	_set_advanced_control("EYES / SKIN MATERIALS", "Eye color", _eye_settings["color"])
	_set_advanced_control("EYES / SKIN MATERIALS", "Pupil size", _eye_settings["pupil_size"])
	_set_advanced_control("EYES / SKIN MATERIALS", "Limbal ring", _eye_settings["limbal_strength"])
	_set_advanced_control("EYES / SKIN MATERIALS", "Skin color", _skin_settings["color"])
	_set_advanced_control("EYES / SKIN MATERIALS", "Skin tone mix", _skin_settings["tone_strength"])
	_set_advanced_control("EYES / SKIN MATERIALS", "Skin roughness", _skin_settings["roughness"])
	_set_advanced_control("EYES / SKIN MATERIALS", "Subsurface scattering", _skin_settings["sss_strength"])

func _rename_label_recursive(node: Node, old_text: String, new_text: String) -> bool:
	if node is Label:
		var label: Label = node as Label
		if label.text == old_text:
			label.text = new_text
			return true
	for child in node.get_children():
		if _rename_label_recursive(child, old_text, new_text):
			return true
	return false
