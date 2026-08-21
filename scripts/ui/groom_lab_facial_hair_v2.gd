extends "res://scripts/ui/groom_lab_facial_hair.gd"

const NaturalFacialHairGenerator = preload("res://scripts/character/continuous_surface_beard_groom.gd")

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

	# Dense short facial-hair baseline tuned from the close-up comparison.
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
	_beard_settings["density_multiplier"] = 30.0
	_beard_settings["coverage"] = 0.68
	_beard_settings["fullness"] = 1.10
	_beard_settings["length"] = 0.0055
	_beard_settings["chin_length"] = 0.0065
	_beard_settings["strand_width"] = 0.00042
	_beard_settings["height_offset"] = 0.0
	_beard_settings["forward_offset"] = 0.00055
	_beard_settings["messiness"] = 0.12

	var default_facial_color: Color = Color(_hair_settings["color"]).darkened(0.10)
	_mustache_settings["color"] = default_facial_color
	_beard_settings["color"] = default_facial_color

	_groom = NaturalFacialHairGenerator.new()
	_groom.name = "ProceduralGroomGenerator"
	add_child(_groom)
	_groom.configure(_character, _meshes, bottom, height, _front_sign)
	_groom.apply_brows(_brow_settings)
	_groom.apply_facial_hair(_mustache_settings, _beard_settings)
	_setup_ui()
	_apply_lash_variant(1 if _available_lash_count() > 0 else 0)
	_refresh_status()

func _setup_ui() -> void:
	super._setup_ui()
	var layer: Node = get_node_or_null("GroomLabUI")
	if layer == null:
		return

	_rename_label_recursive(layer, "Side length", "Beard hair length")

	var column: VBoxContainer = _find_groom_column(layer)
	if column == null:
		push_warning("Could not find groom UI column for beard density multiplier")
		return

	_add_section(column, "BEARD DENSITY")
	_add_slider(column, "Beard density multiplier", 1.0, 100.0, 1.0, float(_beard_settings.get("density_multiplier", 30.0)), "×", func(v: float) -> void:
		_beard_settings["density_multiplier"] = v
		_schedule_facial_hair()
	)

	var note := Label.new()
	note.text = "30× is the tuned default. LOD0 uses the full value; distant LODs automatically reduce follicle population. Beard roots are sampled directly on a continuous head-skin surface field and facial morphs are transferred per follicle root."
	note.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	note.modulate = Color(0.63, 0.70, 0.76)
	note.add_theme_font_size_override("font_size", 11)
	column.add_child(note)

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
