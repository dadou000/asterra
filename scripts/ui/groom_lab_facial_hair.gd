extends "res://scripts/ui/groom_lab_surface.gd"

const FacialHairGenerator = preload("res://scripts/character/facial_hair_groom.gd")

var _facial_hair_timer: Timer
var _link_facial_color := true

var _mustache_settings := {
	"enabled": true,
	"density": 0.72,
	"width": 0.82,
	"thickness": 0.78,
	"length": 0.0075,
	"strand_width": 0.00034,
	"middle_gap": 0.006,
	"droop": 0.32,
	"height_offset": 0.0,
	"forward_offset": 0.00065,
	"messiness": 0.18,
	"color": Color("3b2b21")
}

var _beard_settings := {
	"enabled": true,
	"density": 0.68,
	"coverage": 0.58,
	"fullness": 0.72,
	"length": 0.008,
	"chin_length": 0.014,
	"strand_width": 0.00036,
	"height_offset": 0.0,
	"forward_offset": 0.00065,
	"messiness": 0.22,
	"color": Color("3b2b21")
}

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

	var default_facial_color: Color = Color(_hair_settings["color"]).darkened(0.10)
	_mustache_settings["color"] = default_facial_color
	_beard_settings["color"] = default_facial_color

	_groom = FacialHairGenerator.new()
	_groom.name = "ProceduralGroomGenerator"
	add_child(_groom)
	_groom.configure(_character, _meshes, bottom, height, _front_sign)
	_groom.apply_brows(_brow_settings)
	_groom.apply_facial_hair(_mustache_settings, _beard_settings)
	_setup_ui()
	_apply_lash_variant(1 if _available_lash_count() > 0 else 0)
	_refresh_status()

func _setup_timers() -> void:
	super._setup_timers()
	_facial_hair_timer = Timer.new()
	_facial_hair_timer.one_shot = true
	_facial_hair_timer.wait_time = 0.12
	_facial_hair_timer.timeout.connect(_apply_facial_hair_now)
	add_child(_facial_hair_timer)

func _setup_ui() -> void:
	super._setup_ui()
	var layer: Node = get_node_or_null("GroomLabUI")
	var column: VBoxContainer = _find_groom_column(layer)
	if column == null:
		push_warning("Could not find groom UI column for facial hair controls")
		return

	_add_section(column, "FACIAL HAIR")
	var link_color := CheckButton.new()
	link_color.text = "Link facial hair color to hair"
	link_color.button_pressed = _link_facial_color
	link_color.toggled.connect(func(value: bool) -> void:
		_link_facial_color = value
		if value:
			_sync_facial_color_from_hair()
			_schedule_facial_hair()
	)
	column.add_child(link_color)
	_advanced_controls[_advanced_control_key("Link color")] = link_color

	var facial_color := _add_color_control(column, "Facial hair color", Color(_mustache_settings["color"]), func(color: Color) -> void:
		_link_facial_color = false
		link_color.button_pressed = false
		_mustache_settings["color"] = color
		_beard_settings["color"] = color
		_schedule_facial_hair()
	)
	_advanced_controls[_advanced_control_key("Facial hair color")] = facial_color

	_add_section(column, "MUSTACHE")
	var mustache_enabled := CheckButton.new()
	mustache_enabled.text = "Enabled"
	mustache_enabled.button_pressed = bool(_mustache_settings["enabled"])
	mustache_enabled.toggled.connect(func(value: bool) -> void:
		_mustache_settings["enabled"] = value
		_schedule_facial_hair()
	)
	column.add_child(mustache_enabled)
	_advanced_controls[_advanced_control_key("Enabled")] = mustache_enabled

	_add_slider(column, "Density", 0.05, 1.0, 0.01, float(_mustache_settings["density"]), "", func(v: float) -> void:
		_mustache_settings["density"] = v
		_schedule_facial_hair()
	)
	_add_slider(column, "Width", 0.45, 1.35, 0.01, float(_mustache_settings["width"]), "×", func(v: float) -> void:
		_mustache_settings["width"] = v
		_schedule_facial_hair()
	)
	_add_slider(column, "Thickness", 0.30, 1.80, 0.01, float(_mustache_settings["thickness"]), "×", func(v: float) -> void:
		_mustache_settings["thickness"] = v
		_schedule_facial_hair()
	)
	_add_slider(column, "Hair length", 1.5, 30.0, 0.5, float(_mustache_settings["length"]) * 1000.0, " mm", func(v: float) -> void:
		_mustache_settings["length"] = v * 0.001
		_schedule_facial_hair()
	)
	_add_slider(column, "Render strand width", 0.12, 1.20, 0.02, float(_mustache_settings["strand_width"]) * 1000.0, " mm", func(v: float) -> void:
		_mustache_settings["strand_width"] = v * 0.001
		_schedule_facial_hair()
	)
	_add_slider(column, "Middle gap", 0.0, 30.0, 0.5, float(_mustache_settings["middle_gap"]) * 1000.0, " mm", func(v: float) -> void:
		_mustache_settings["middle_gap"] = v * 0.001
		_schedule_facial_hair()
	)
	_add_slider(column, "Outer droop", 0.0, 1.0, 0.01, float(_mustache_settings["droop"]), "", func(v: float) -> void:
		_mustache_settings["droop"] = v
		_schedule_facial_hair()
	)
	_add_slider(column, "Vertical offset", -25.0, 25.0, 0.5, float(_mustache_settings["height_offset"]) * 1000.0, " mm", func(v: float) -> void:
		_mustache_settings["height_offset"] = v * 0.001
		_schedule_facial_hair()
	)
	_add_slider(column, "Surface offset", -2.0, 8.0, 0.1, float(_mustache_settings["forward_offset"]) * 1000.0, " mm", func(v: float) -> void:
		_mustache_settings["forward_offset"] = v * 0.001
		_schedule_facial_hair()
	)
	_add_slider(column, "Messiness", 0.0, 1.0, 0.01, float(_mustache_settings["messiness"]), "", func(v: float) -> void:
		_mustache_settings["messiness"] = v
		_schedule_facial_hair()
	)

	_add_section(column, "BEARD")
	var beard_enabled := CheckButton.new()
	beard_enabled.text = "Enabled"
	beard_enabled.button_pressed = bool(_beard_settings["enabled"])
	beard_enabled.toggled.connect(func(value: bool) -> void:
		_beard_settings["enabled"] = value
		_schedule_facial_hair()
	)
	column.add_child(beard_enabled)
	_advanced_controls[_advanced_control_key("Enabled")] = beard_enabled

	_add_slider(column, "Density", 0.05, 1.0, 0.01, float(_beard_settings["density"]), "", func(v: float) -> void:
		_beard_settings["density"] = v
		_schedule_facial_hair()
	)
	_add_slider(column, "Cheek coverage", 0.0, 1.0, 0.01, float(_beard_settings["coverage"]), "", func(v: float) -> void:
		_beard_settings["coverage"] = v
		_schedule_facial_hair()
	)
	_add_slider(column, "Jaw fullness", 0.15, 1.35, 0.01, float(_beard_settings["fullness"]), "×", func(v: float) -> void:
		_beard_settings["fullness"] = v
		_schedule_facial_hair()
	)
	_add_slider(column, "Side length", 1.0, 45.0, 0.5, float(_beard_settings["length"]) * 1000.0, " mm", func(v: float) -> void:
		_beard_settings["length"] = v * 0.001
		_schedule_facial_hair()
	)
	_add_slider(column, "Chin length", 1.0, 60.0, 0.5, float(_beard_settings["chin_length"]) * 1000.0, " mm", func(v: float) -> void:
		_beard_settings["chin_length"] = v * 0.001
		_schedule_facial_hair()
	)
	_add_slider(column, "Render strand width", 0.12, 1.50, 0.02, float(_beard_settings["strand_width"]) * 1000.0, " mm", func(v: float) -> void:
		_beard_settings["strand_width"] = v * 0.001
		_schedule_facial_hair()
	)
	_add_slider(column, "Vertical offset", -35.0, 35.0, 0.5, float(_beard_settings["height_offset"]) * 1000.0, " mm", func(v: float) -> void:
		_beard_settings["height_offset"] = v * 0.001
		_schedule_facial_hair()
	)
	_add_slider(column, "Surface offset", -2.0, 10.0, 0.1, float(_beard_settings["forward_offset"]) * 1000.0, " mm", func(v: float) -> void:
		_beard_settings["forward_offset"] = v * 0.001
		_schedule_facial_hair()
	)
	_add_slider(column, "Messiness", 0.0, 1.0, 0.01, float(_beard_settings["messiness"]), "", func(v: float) -> void:
		_beard_settings["messiness"] = v
		_schedule_facial_hair()
	)

	var note := Label.new()
	note.text = "LOD0/1 are surface-bound to the imported facial morph field. The existing debug LOD slider also forces mustache + beard LOD for close-up testing."
	note.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	note.modulate = Color(0.63, 0.70, 0.76)
	note.add_theme_font_size_override("font_size", 11)
	column.add_child(note)

	var regenerate := Button.new()
	regenerate.text = "Regenerate mustache + beard"
	regenerate.pressed.connect(_apply_facial_hair_now)
	column.add_child(regenerate)

func _schedule_facial_hair() -> void:
	if _facial_hair_timer != null:
		_facial_hair_timer.start()

func _sync_facial_color_from_hair() -> void:
	var color: Color = Color(_hair_settings["color"]).darkened(0.10)
	_mustache_settings["color"] = color
	_beard_settings["color"] = color

func _apply_facial_hair_now() -> void:
	if _facial_hair_timer != null:
		_facial_hair_timer.stop()
	if _groom == null:
		return
	if _link_facial_color:
		_sync_facial_color_from_hair()
	if _groom.has_method("apply_facial_hair"):
		_groom.apply_facial_hair(_mustache_settings, _beard_settings)
	_refresh_status()

func _apply_hair_now() -> void:
	super._apply_hair_now()
	if _link_facial_color:
		_sync_facial_color_from_hair()
		_schedule_facial_hair()
