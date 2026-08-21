extends "res://scripts/ui/groom_lab.gd"

const SurfaceGroomGenerator = preload("res://scripts/character/advanced_surface_groom.gd")

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
	if not _brow_settings.has("middle_spacing"):
		_brow_settings["middle_spacing"] = 0.030
	if not _brow_settings.has("inner_fade_ratio"):
		_brow_settings["inner_fade_ratio"] = 0.10
	if not _brow_settings.has("outer_fade_ratio"):
		_brow_settings["outer_fade_ratio"] = 0.18
	if not _brow_settings.has("thickness"):
		_brow_settings["thickness"] = 1.0
	if not _brow_settings.has("messiness"):
		_brow_settings["messiness"] = 0.18

func _setup_ui() -> void:
	super._setup_ui()

	var layer: Node = get_node_or_null("GroomLabUI")
	var column: VBoxContainer = _find_groom_column(layer)
	if column == null:
		push_warning("Could not find groom UI column for advanced brow controls")
		return

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
