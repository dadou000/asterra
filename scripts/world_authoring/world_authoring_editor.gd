extends Control
## Planet Studio Phase 0 shell: persistent staging, presets, history and the five
## top-level authoring categories. It does not mutate production terrain yet.

const START_MENU_SCENE := "res://scenes/StartMenu.tscn"
const CATEGORY_NAMES: PackedStringArray = ["PLANET", "TERRAIN", "WATER", "ATMOSPHERIC", "CELESTIALS"]

var _session: WorldAuthoringSession
var _category: String = "PLANET"
var _body_selector: OptionButton
var _dirty_label: Label
var _status_label: Label
var _workspace: VBoxContainer
var _undo_button: Button
var _redo_button: Button
var _apply_button: Button
var _revert_button: Button
var _save_dialog: FileDialog
var _load_dialog: FileDialog

func _ready() -> void:
	Input.set_mouse_mode(Input.MOUSE_MODE_VISIBLE)
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_session = WorldAuthoringSession.new()
	_session.changed.connect(_on_session_changed)
	_session.error_reported.connect(_set_status)
	_session.preset_saved.connect(func(path: String) -> void: _set_status("Preset saved: %s" % path))
	_session.preset_loaded.connect(func(path: String) -> void: _set_status("Preset staged: %s" % path))
	_session.applied.connect(func(_system: CelestialSystemDefinition) -> void:
		_set_status("Staged system applied inside Planet Studio. Runtime binding begins in Phase 1.")
	)
	_session.bootstrap_from_current_world()
	_build_shell()
	_build_file_dialogs()
	_refresh_all()

func _build_shell() -> void:
	var background := ColorRect.new()
	background.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	background.color = Color("080d13")
	background.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(background)

	var margin := MarginContainer.new()
	margin.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	margin.add_theme_constant_override("margin_left", 12)
	margin.add_theme_constant_override("margin_right", 12)
	margin.add_theme_constant_override("margin_top", 12)
	margin.add_theme_constant_override("margin_bottom", 12)
	add_child(margin)

	var root := VBoxContainer.new()
	root.add_theme_constant_override("separation", 8)
	margin.add_child(root)

	root.add_child(_build_toolbar())
	root.add_child(HSeparator.new())

	var content := HBoxContainer.new()
	content.size_flags_vertical = Control.SIZE_EXPAND_FILL
	content.add_theme_constant_override("separation", 8)
	root.add_child(content)

	var navigation := VBoxContainer.new()
	navigation.custom_minimum_size.x = 180.0
	navigation.add_theme_constant_override("separation", 5)
	content.add_child(navigation)

	var nav_title := Label.new()
	nav_title.text = "AUTHORING"
	nav_title.modulate = Color(0.55, 0.66, 0.76)
	nav_title.add_theme_font_size_override("font_size", 12)
	navigation.add_child(nav_title)

	for category_name: String in CATEGORY_NAMES:
		var button := Button.new()
		button.text = category_name
		button.custom_minimum_size.y = 46.0
		button.alignment = HORIZONTAL_ALIGNMENT_LEFT
		button.pressed.connect(_show_category.bind(category_name))
		navigation.add_child(button)

	var divider := VSeparator.new()
	content.add_child(divider)

	var scroll := ScrollContainer.new()
	scroll.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	content.add_child(scroll)

	_workspace = VBoxContainer.new()
	_workspace.custom_minimum_size = Vector2(760.0, 0.0)
	_workspace.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_workspace.add_theme_constant_override("separation", 10)
	scroll.add_child(_workspace)

	_status_label = Label.new()
	_status_label.text = "Phase 0: staging only — production terrain is intentionally unchanged."
	_status_label.modulate = Color(0.58, 0.68, 0.77)
	_status_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	root.add_child(_status_label)

func _build_toolbar() -> HBoxContainer:
	var toolbar := HBoxContainer.new()
	toolbar.custom_minimum_size.y = 46.0
	toolbar.add_theme_constant_override("separation", 6)

	var back := _toolbar_button("Back")
	back.pressed.connect(func() -> void: get_tree().change_scene_to_file(START_MENU_SCENE))
	toolbar.add_child(back)

	var title := Label.new()
	title.text = "PLANET STUDIO"
	title.custom_minimum_size.x = 150.0
	title.add_theme_font_size_override("font_size", 20)
	toolbar.add_child(title)

	_body_selector = OptionButton.new()
	_body_selector.custom_minimum_size.x = 190.0
	_body_selector.item_selected.connect(_on_body_selected)
	toolbar.add_child(_body_selector)

	_dirty_label = Label.new()
	_dirty_label.custom_minimum_size.x = 185.0
	toolbar.add_child(_dirty_label)

	var spacer := Control.new()
	spacer.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	toolbar.add_child(spacer)

	var save := _toolbar_button("Save Preset")
	save.pressed.connect(func() -> void: _save_dialog.popup_centered_ratio(0.72))
	toolbar.add_child(save)
	var load_button := _toolbar_button("Load Preset")
	load_button.pressed.connect(func() -> void: _load_dialog.popup_centered_ratio(0.72))
	toolbar.add_child(load_button)

	_undo_button = _toolbar_button("Undo")
	_undo_button.pressed.connect(_on_undo_pressed)
	toolbar.add_child(_undo_button)
	_redo_button = _toolbar_button("Redo")
	_redo_button.pressed.connect(_on_redo_pressed)
	toolbar.add_child(_redo_button)

	_apply_button = _toolbar_button("Apply")
	_apply_button.pressed.connect(_on_apply_pressed)
	toolbar.add_child(_apply_button)
	_revert_button = _toolbar_button("Revert")
	_revert_button.pressed.connect(_on_revert_pressed)
	toolbar.add_child(_revert_button)
	return toolbar

func _build_file_dialogs() -> void:
	_save_dialog = FileDialog.new()
	_save_dialog.title = "Save Planet Studio Preset"
	_save_dialog.file_mode = FileDialog.FILE_MODE_SAVE_FILE
	_save_dialog.access = FileDialog.ACCESS_USERDATA
	_save_dialog.filters = PackedStringArray(["*.tres ; Asterra Planet Studio Preset"])
	_save_dialog.current_file = "planet_preset.tres"
	_save_dialog.file_selected.connect(func(path: String) -> void: _session.save_preset(path))
	add_child(_save_dialog)

	_load_dialog = FileDialog.new()
	_load_dialog.title = "Load Planet Studio Preset"
	_load_dialog.file_mode = FileDialog.FILE_MODE_OPEN_FILE
	_load_dialog.access = FileDialog.ACCESS_USERDATA
	_load_dialog.filters = PackedStringArray(["*.tres ; Asterra Planet Studio Preset"])
	_load_dialog.file_selected.connect(_on_preset_selected)
	add_child(_load_dialog)

func _toolbar_button(text: String) -> Button:
	var button := Button.new()
	button.text = text
	button.custom_minimum_size = Vector2(86.0, 36.0)
	return button

func _refresh_all() -> void:
	_refresh_toolbar()
	_show_category(_category)

func _refresh_toolbar() -> void:
	if _body_selector == null:
		return
	_body_selector.clear()
	var active_id := _session.staged_system.active_body_id
	var selected_index := 0
	for index: int in _session.staged_system.bodies.size():
		var body: CelestialBodyDefinition = _session.staged_system.bodies[index]
		_body_selector.add_item(body.display_name)
		_body_selector.set_item_metadata(index, body.body_id)
		if body.body_id == active_id:
			selected_index = index
	_body_selector.select(selected_index)

	_dirty_label.text = ("● Modified — %s" % _scope_name(_session.apply_scope)) if _session.dirty else "Saved / applied"
	_dirty_label.modulate = Color(0.96, 0.70, 0.30) if _session.dirty else Color(0.50, 0.70, 0.58)
	_undo_button.disabled = not _session.can_undo()
	_redo_button.disabled = not _session.can_redo()
	_apply_button.disabled = not _session.dirty
	_revert_button.disabled = not _session.dirty

func _show_category(category_name: String) -> void:
	_category = category_name
	_clear_workspace()
	match _category:
		"PLANET":
			_build_planet_page()
		"TERRAIN":
			_build_terrain_page()
		"WATER":
			_build_water_page()
		"ATMOSPHERIC":
			_build_atmosphere_page()
		"CELESTIALS":
			_build_celestials_page()

func _clear_workspace() -> void:
	for child: Node in _workspace.get_children():
		_workspace.remove_child(child)
		child.queue_free()

func _page_title(title_text: String, description: String) -> void:
	var title := Label.new()
	title.text = title_text
	title.add_theme_font_size_override("font_size", 30)
	_workspace.add_child(title)
	var subtitle := Label.new()
	subtitle.text = description
	subtitle.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	subtitle.modulate = Color(0.62, 0.70, 0.78)
	_workspace.add_child(subtitle)
	_workspace.add_child(HSeparator.new())

func _section(title_text: String) -> void:
	var label := Label.new()
	label.text = title_text
	label.add_theme_font_size_override("font_size", 18)
	label.modulate = Color(0.72, 0.82, 0.91)
	_workspace.add_child(label)

func _build_planet_page() -> void:
	var body := _session.active_body()
	_page_title("Planet", "Physical body, rotation and orbital authoring. Phase 0 changes are staged safely and round-trip through presets.")
	if body == null:
		_add_note("No active celestial body.")
		return

	_section("Identity")
	_add_text_field("Display name", body.display_name, func(value: String) -> void:
		_session.stage_set(body, &"display_name", value, WorldAuthoringSession.ApplyScope.HOT, "Rename body")
		_refresh_toolbar()
	)

	_section("Physical")
	_add_number_field("Radius", body.radius_m / 1000.0, 1.0, 1000000.0, 1.0, " km", func(value: float) -> void:
		_session.stage_set(body, &"radius_m", value * 1000.0, WorldAuthoringSession.ApplyScope.FULL_REBUILD, "Change radius")
	)
	_add_number_field("Surface gravity", body.surface_gravity_m_s2, 0.0, 1000.0, 0.01, " m/s²", func(value: float) -> void:
		_session.stage_set(body, &"surface_gravity_m_s2", value, WorldAuthoringSession.ApplyScope.FULL_REBUILD, "Change gravity")
	)

	_section("Rotation")
	_add_number_field("Sidereal day", body.hours_per_day(), 0.001, 100000.0, 0.01, " h", func(value: float) -> void:
		_session.stage_action("Change day length", func() -> void: body.set_hours_per_day(value), WorldAuthoringSession.ApplyScope.FULL_REBUILD)
	)
	_add_number_field("Axial tilt", body.axial_tilt_deg, -180.0, 180.0, 0.1, "°", func(value: float) -> void:
		_session.stage_set(body, &"axial_tilt_deg", value, WorldAuthoringSession.ApplyScope.FULL_REBUILD, "Change axial tilt")
	)

	_section("Orbit")
	_add_note("Keplerian orientation is stored already. The interactive celestial map, parent picker and apsis/orbit gizmos are the next Phase 1 implementation pass.")
	_add_number_field("Periapsis", body.orbit.periapsis_m() / 1000.0, 0.0, 1.0e12, 1.0, " km", func(value: float) -> void:
		var apo := maxf(body.orbit.apoapsis_m(), value * 1000.0)
		_session.stage_action("Change periapsis", func() -> void: body.orbit.set_apsides(value * 1000.0, apo), WorldAuthoringSession.ApplyScope.FULL_REBUILD)
	)
	_add_number_field("Apoapsis", body.orbit.apoapsis_m() / 1000.0, 0.0, 1.0e12, 1.0, " km", func(value: float) -> void:
		var peri := minf(body.orbit.periapsis_m(), value * 1000.0)
		_session.stage_action("Change apoapsis", func() -> void: body.orbit.set_apsides(peri, value * 1000.0), WorldAuthoringSession.ApplyScope.FULL_REBUILD)
	)
	_add_number_field("Inclination", body.orbit.inclination_deg, -180.0, 180.0, 0.1, "°", func(value: float) -> void:
		_session.stage_set(body.orbit, &"inclination_deg", value, WorldAuthoringSession.ApplyScope.FULL_REBUILD, "Change orbit inclination")
	)

func _build_terrain_page() -> void:
	var body := _session.active_body()
	_page_title("Terrain", "Generation parameters now live inside the staged planet profile. Biome painting and graph slots will bind to this profile without altering the pristine generator.")
	if body == null or body.planet_profile == null:
		_add_note("The selected body has no terrestrial terrain profile.")
		return
	var cfg := body.planet_profile.terrain.ensure_generation_config()
	_section("Generation")
	_add_text_field("World seed (int64)", str(cfg.world_seed), func(value: String) -> void:
		if value.is_valid_int():
			_session.stage_set(cfg, &"world_seed", value.to_int(), WorldAuthoringSession.ApplyScope.FULL_REBUILD, "Change world seed")
		else:
			_set_status("World seed must be a signed 64-bit integer.")
	)
	_add_number_field("Ocean fraction", cfg.ocean_fraction, 0.0, 1.0, 0.001, "", func(value: float) -> void:
		_session.stage_set(cfg, &"ocean_fraction", value, WorldAuthoringSession.ApplyScope.FULL_REBUILD, "Change ocean fraction")
	)
	_add_number_field("Maximum uplift", cfg.max_uplift, 0.0, 30000.0, 10.0, " m", func(value: float) -> void:
		_session.stage_set(cfg, &"max_uplift", value, WorldAuthoringSession.ApplyScope.FULL_REBUILD, "Change uplift")
	)
	_add_number_field("Detail amplitude", cfg.detail_amplitude, 0.0, 5000.0, 1.0, " m", func(value: float) -> void:
		_session.stage_set(cfg, &"detail_amplitude", value, WorldAuthoringSession.ApplyScope.CLIPMAP, "Change detail amplitude")
	)

	_section("Post-generation biome overrides")
	_add_note("Sparse spherical biome override layers are reserved in the data model. Brush painting, opacity/hardness and terrain/scatter resolution are Phase 2.")
	_section("Displacement shader slots")
	_add_note("Slot list is persisted separately from material slots. Phase 4 adds clipmap masks, ALL / ONLY / EXCEPT biome targeting, signed composition and the typed node compiler.")
	_section("Material shader slots")
	_add_note("Material slots share the same clipmap and biome selectors but compile into one terrain material path rather than one draw pass per slot.")

func _build_water_page() -> void:
	var body := _session.active_body()
	_page_title("Water", "Ocean, lake and river authoring share one persistent water profile. Vector features will rasterize into clipmap water fields in Phase 3.")
	if body == null or body.planet_profile == null:
		_add_note("The selected body has no water profile.")
		return
	var water := body.planet_profile.water
	_add_toggle("Ocean enabled", water.ocean_enabled, func(value: bool) -> void:
		_session.stage_set(water, &"ocean_enabled", value, WorldAuthoringSession.ApplyScope.CLIPMAP, "Toggle ocean")
	)
	_add_number_field("Sea level", water.sea_level_m, -20000.0, 20000.0, 0.1, " m", func(value: float) -> void:
		_session.stage_set(water, &"sea_level_m", value, WorldAuthoringSession.ApplyScope.CLIPMAP, "Change sea level")
	)
	_add_number_field("Wave amplitude scale", water.wave_amplitude_scale, 0.0, 20.0, 0.01, "×", func(value: float) -> void:
		_session.stage_set(water, &"wave_amplitude_scale", value, WorldAuthoringSession.ApplyScope.HOT, "Change wave amplitude")
	)
	_add_number_field("Wave frequency scale", water.wave_frequency_scale, 0.01, 20.0, 0.01, "×", func(value: float) -> void:
		_session.stage_set(water, &"wave_frequency_scale", value, WorldAuthoringSession.ApplyScope.HOT, "Change wave frequency")
	)
	_section("Authored features")
	_add_note("Next: 3D cubic Bézier rivers with editable width/depth/current and freeform spherical lake polygons, both feeding the same water clipmap simulation/render field.")

func _build_atmosphere_page() -> void:
	var body := _session.active_body()
	_page_title("Atmospheric", "Atmosphere values are now body-owned instead of editor-global. Runtime live binding is deliberately deferred until the profile contract is stable.")
	if body == null or body.planet_profile == null:
		_add_note("The selected body has no atmosphere profile.")
		return
	var atmosphere := body.planet_profile.atmosphere
	_add_toggle("Atmosphere enabled", atmosphere.enabled, func(value: bool) -> void:
		_session.stage_set(atmosphere, &"enabled", value, WorldAuthoringSession.ApplyScope.HOT, "Toggle atmosphere")
	)
	_add_number_field("Atmosphere height", atmosphere.atmosphere_height_m / 1000.0, 0.1, 10000.0, 0.1, " km", func(value: float) -> void:
		_session.stage_set(atmosphere, &"atmosphere_height_m", value * 1000.0, WorldAuthoringSession.ApplyScope.HOT, "Change atmosphere height")
	)
	_add_number_field("Rayleigh strength", atmosphere.rayleigh_strength, 0.0, 20.0, 0.01, "×", func(value: float) -> void:
		_session.stage_set(atmosphere, &"rayleigh_strength", value, WorldAuthoringSession.ApplyScope.HOT, "Change Rayleigh scattering")
	)
	_add_number_field("Mie strength", atmosphere.mie_strength, 0.0, 20.0, 0.01, "×", func(value: float) -> void:
		_session.stage_set(atmosphere, &"mie_strength", value, WorldAuthoringSession.ApplyScope.HOT, "Change Mie scattering")
	)
	_add_number_field("Cloud coverage", atmosphere.cloud_coverage, 0.0, 1.0, 0.01, "", func(value: float) -> void:
		_session.stage_set(atmosphere, &"cloud_coverage", value, WorldAuthoringSession.ApplyScope.HOT, "Change cloud coverage")
	)

func _build_celestials_page() -> void:
	_page_title("Celestials", "Persistent multi-body hierarchy foundation. Runtime body switching and the orbital map are Phase 1, but create/duplicate/delete already round-trip safely.")
	var list := ItemList.new()
	list.custom_minimum_size = Vector2(0.0, 220.0)
	for index: int in _session.staged_system.bodies.size():
		var body: CelestialBodyDefinition = _session.staged_system.bodies[index]
		var parent_text := "" if body.parent_body_id.is_empty() else "  → %s" % body.parent_body_id
		list.add_item("%s  [%s]%s" % [body.display_name, _body_type_name(body.body_type), parent_text])
		list.set_item_metadata(index, body.body_id)
		if body.body_id == _session.staged_system.active_body_id:
			list.select(index)
	list.item_selected.connect(func(index: int) -> void:
		var body_id := String(list.get_item_metadata(index))
		_session.select_body(body_id)
		_refresh_all()
	)
	_workspace.add_child(list)

	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 6)
	_workspace.add_child(row)
	var add_planet := _toolbar_button("Add Planet")
	add_planet.pressed.connect(func() -> void:
		_session.create_body("New Planet", CelestialBodyDefinition.BodyType.PLANET)
		_refresh_all()
	)
	row.add_child(add_planet)
	var add_moon := _toolbar_button("Add Moon")
	add_moon.pressed.connect(func() -> void:
		var parent := _session.active_body()
		var parent_id := parent.body_id if parent != null else ""
		_session.create_body("New Moon", CelestialBodyDefinition.BodyType.MOON, parent_id)
		_refresh_all()
	)
	row.add_child(add_moon)
	var duplicate := _toolbar_button("Duplicate")
	duplicate.pressed.connect(func() -> void:
		_session.duplicate_active_body()
		_refresh_all()
	)
	row.add_child(duplicate)
	var delete_button := _toolbar_button("Delete")
	delete_button.disabled = _session.staged_system.bodies.size() <= 1
	delete_button.pressed.connect(func() -> void:
		if not _session.delete_active_body():
			_set_status("Planet Studio keeps at least one body in the system.")
		_refresh_all()
	)
	row.add_child(delete_button)

func _add_text_field(label_text: String, value: String, callback: Callable) -> void:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 12)
	_workspace.add_child(row)
	var label := Label.new()
	label.text = label_text
	label.custom_minimum_size.x = 260.0
	row.add_child(label)
	var edit := LineEdit.new()
	edit.text = value
	edit.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	edit.text_submitted.connect(func(text: String) -> void: callback.call(text))
	row.add_child(edit)

func _add_number_field(label_text: String, value: float, min_value: float, max_value: float, step: float, suffix: String, callback: Callable) -> void:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 12)
	_workspace.add_child(row)
	var label := Label.new()
	label.text = label_text
	label.custom_minimum_size.x = 260.0
	row.add_child(label)
	var spin := SpinBox.new()
	spin.min_value = min_value
	spin.max_value = max_value
	spin.step = step
	spin.suffix = suffix
	spin.value = clampf(value, min_value, max_value)
	spin.custom_minimum_size.x = 240.0
	spin.value_changed.connect(func(next_value: float) -> void: callback.call(next_value))
	row.add_child(spin)

func _add_toggle(label_text: String, value: bool, callback: Callable) -> void:
	var toggle := CheckButton.new()
	toggle.text = label_text
	toggle.button_pressed = value
	toggle.toggled.connect(func(next_value: bool) -> void: callback.call(next_value))
	_workspace.add_child(toggle)

func _add_note(text: String) -> void:
	var note := Label.new()
	note.text = text
	note.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	note.modulate = Color(0.57, 0.65, 0.72)
	_workspace.add_child(note)

func _on_body_selected(index: int) -> void:
	if index < 0 or index >= _body_selector.item_count:
		return
	var body_id := String(_body_selector.get_item_metadata(index))
	_session.select_body(body_id)
	_refresh_all()

func _on_undo_pressed() -> void:
	_session.undo()
	_refresh_all()

func _on_redo_pressed() -> void:
	_session.redo()
	_refresh_all()

func _on_apply_pressed() -> void:
	_session.apply()
	_refresh_all()

func _on_revert_pressed() -> void:
	_session.revert()
	_refresh_all()
	_set_status("Staged changes reverted to the last Planet Studio Apply state.")

func _on_preset_selected(path: String) -> void:
	if _session.load_preset(path) == OK:
		_refresh_all()

func _on_session_changed(_dirty: bool, _scope: int) -> void:
	_refresh_toolbar()

func _set_status(message: String) -> void:
	if _status_label != null:
		_status_label.text = message

func _scope_name(scope: int) -> String:
	match scope:
		WorldAuthoringSession.ApplyScope.HOT:
			return "HOT"
		WorldAuthoringSession.ApplyScope.GRAPH:
			return "GRAPH"
		WorldAuthoringSession.ApplyScope.TILES:
			return "TILES"
		WorldAuthoringSession.ApplyScope.CLIPMAP:
			return "CLIPMAP"
		WorldAuthoringSession.ApplyScope.FULL_REBUILD:
			return "FULL REBUILD"
		_:
			return "NONE"

func _body_type_name(body_type: int) -> String:
	match body_type:
		CelestialBodyDefinition.BodyType.STAR:
			return "STAR"
		CelestialBodyDefinition.BodyType.MOON:
			return "MOON"
		CelestialBodyDefinition.BodyType.DWARF:
			return "DWARF"
		CelestialBodyDefinition.BodyType.OTHER:
			return "OTHER"
		_:
			return "PLANET"
