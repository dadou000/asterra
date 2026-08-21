extends "res://scripts/ui/groom_lab.gd"

const SurfaceGroomGenerator = preload("res://scripts/character/morph_bound_brow_groom.gd")

func _late_setup() -> void:
	await get_tree().process_frame
	var root: Node = get_parent()
	_character = root.get_node_or_null("AsterraHuman") as Node3D
	if _character == null:
		await get_tree().process_frame
		_character = root.get_node_or_null("AsterraHuman") as Node3D
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

	_groom = SurfaceGroomGenerator.new()
	_groom.name = "ProceduralGroomGenerator"
	add_child(_groom)
	_groom.configure(_character, _meshes, bottom, height, _front_sign)
	# Configure performs an initial rebuild using generator defaults. Apply the
	# creator dictionary once so the visible default matches the UI exactly.
	_groom.apply_brows(_brow_settings)
	_setup_ui()
	_apply_lash_variant(1 if _available_lash_count() > 0 else 0)
	_refresh_status()

func _ensure_advanced_brow_defaults() -> void:
	# Tuned Asterra brow baseline approved in Character Editor. Keep these values
	# together so every new character-creator session starts from the same look.
	_brow_settings["density"] = 1.0
	_brow_settings["width"] = 0.73
	_brow_settings["strand_width"] = 0.00045
	_brow_settings["arch"] = 0.33
	_brow_settings["height_offset"] = -0.013
	_brow_settings["forward_offset"] = 0.0025
	_brow_settings["middle_spacing"] = 0.008
	_brow_settings["thickness"] = 0.67
	_brow_settings["inner_fade_ratio"] = 0.14
	_brow_settings["outer_fade_ratio"] = 0.18
	_brow_settings["messiness"] = 0.16

func _setup_ui() -> void:
	super._setup_ui()

	var layer: Node = get_node_or_null("GroomLabUI")
	var column: VBoxContainer = _find_groom_column(layer)
	if column == null:
		push_warning("Could not find groom UI column for advanced brow controls")
		return

	_build_brow_lod_debug_controls(column)

	_add_section(column, "BROW SHAPE / FALLOFF")

	_add_slider(column, "Middle spacing", -20.0, 60.0, 0.5, float(_brow_settings["middle_spacing"]) * 1000.0, " mm", func(v: float) -> void:
		_brow_settings["middle_spacing"] = v * 0.001
		_schedule_brows()
	)

	_add_slider(column, "Brow thickness", 0.30, 2.50, 0.01, float(_brow_settings["thickness"]), "×", func(v: float) -> void:
		_brow_settings["thickness"] = v
		_schedule_brows()
	)

	_add_slider(column, "Inner fading ratio", 0.0, 0.50, 0.01, float(_brow_settings["inner_fade_ratio"]), "", func(v: float) -> void:
		_brow_settings["inner_fade_ratio"] = v
		_schedule_brows()
	)

	_add_slider(column, "Outside fading ratio", 0.0, 0.50, 0.01, float(_brow_settings["outer_fade_ratio"]), "", func(v: float) -> void:
		_brow_settings["outer_fade_ratio"] = v
		_schedule_brows()
	)

	_add_slider(column, "Messiness", 0.0, 1.0, 0.01, float(_brow_settings["messiness"]), "", func(v: float) -> void:
		_brow_settings["messiness"] = v
		_schedule_brows()
	)

	var hint := Label.new()
	hint.text = "Middle spacing: 0 mm = brows meet; negative values overlap for a stronger unibrow. Fade ratios control how much of each end gradually loses follicle density."
	hint.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	hint.modulate = Color(0.63, 0.70, 0.76)
	hint.add_theme_font_size_override("font_size", 11)
	column.add_child(hint)

func _build_brow_lod_debug_controls(column: VBoxContainer) -> void:
	var block := VBoxContainer.new()
	block.name = "BrowLODDebugControls"
	block.add_theme_constant_override("separation", 6)
	block.visible = AppSettings.debug_brow_lod_controls
	column.add_child(block)

	block.add_child(HSeparator.new())
	var section := Label.new()
	section.text = "BROW LOD DEBUG"
	section.modulate = Color(0.58, 0.68, 0.77)
	section.add_theme_font_size_override("font_size", 13)
	block.add_child(section)

	var header := HBoxContainer.new()
	block.add_child(header)
	var title := Label.new()
	title.text = "Force brow LOD"
	title.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	header.add_child(title)
	var value_label := Label.new()
	value_label.custom_minimum_size.x = 70.0
	value_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	value_label.text = _brow_lod_label(AppSettings.debug_forced_brow_lod)
	header.add_child(value_label)

	var slider := HSlider.new()
	slider.min_value = -1.0
	slider.max_value = 2.0
	slider.step = 1.0
	slider.value = float(AppSettings.debug_forced_brow_lod)
	slider.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	slider.value_changed.connect(func(value: float) -> void:
		var lod: int = int(round(value))
		value_label.text = _brow_lod_label(lod)
		AppSettings.set_debug_forced_brow_lod(lod)
		_refresh_status()
	)
	block.add_child(slider)

	var scale := HBoxContainer.new()
	block.add_child(scale)
	for label_text in ["AUTO", "LOD0", "LOD1", "LOD2"]:
		var label := Label.new()
		label.text = label_text
		label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
		label.modulate = Color(0.53, 0.60, 0.67)
		scale.add_child(label)

	var hint := Label.new()
	hint.text = "AUTO: LOD0 < 6 m, LOD1 from 6-15 m, LOD2 from 15 m. This control only appears while Brow LOD debug is enabled in Settings."
	hint.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	hint.modulate = Color(0.63, 0.70, 0.76)
	hint.add_theme_font_size_override("font_size", 11)
	block.add_child(hint)

	AppSettings.debug_brow_lod_controls_changed.connect(func(enabled: bool) -> void:
		block.visible = enabled
		if not enabled:
			slider.set_value_no_signal(-1.0)
			value_label.text = "AUTO"
	)
	AppSettings.debug_forced_brow_lod_changed.connect(func(lod: int) -> void:
		slider.set_value_no_signal(float(lod))
		value_label.text = _brow_lod_label(lod)
		_refresh_status()
	)

func _brow_lod_label(lod: int) -> String:
	match lod:
		0:
			return "LOD0"
		1:
			return "LOD1"
		2:
			return "LOD2"
		_:
			return "AUTO"

func _find_groom_column(node: Node) -> VBoxContainer:
	if node == null:
		return null
	if node is VBoxContainer:
		return node as VBoxContainer
	for child in node.get_children():
		var found: VBoxContainer = _find_groom_column(child)
		if found != null:
			return found
	return null