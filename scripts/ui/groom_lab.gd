extends Node

const GroomGenerator = preload("res://scripts/character/procedural_groom.gd")
const LASH_TEXTURES := [
	"res://assets/character/asterrahuman_eyelashes01.png",
	"res://assets/character/asterrahuman_eyelashes02.png",
	"res://assets/character/asterrahuman_eyelashes03.png",
	"res://assets/character/asterrahuman_eyelashes04.png"
]

var _character: Node3D
var _meshes: Array[MeshInstance3D] = []
var _lash_meshes: Array[MeshInstance3D] = []
var _lash_original_materials: Dictionary = {}
var _lash_selector: OptionButton
var _lash_tint := Color.WHITE
var _status: Label
var _groom
var _hair_timer: Timer
var _brow_timer: Timer

var _hair_settings := {
	"enabled": true,
	"style": 0,
	"density": 0.58,
	"length": 0.085,
	"width": 0.0009,
	"curl": 0.08,
	"gravity": 0.42,
	"hairline": 0.46,
	"scalp_scale": 1.0,
	"root_lift": 0.0025,
	"color": Color("4b3426")
}

var _brow_settings := {
	"enabled": true,
	"density": 0.68,
	"width": 1.0,
	"strand_width": 0.00045,
	"arch": 0.45,
	"height_offset": 0.0,
	"forward_offset": 0.0015,
	"color": Color("3a281e")
}

var _link_brow_color := true
var _front_sign := 1.0

func _ready() -> void:
	# CharacterEditor creates the GLB in its own _ready(). Children become ready
	# first, so wait until the parent's setup has completed.
	call_deferred("_late_setup")

func _late_setup() -> void:
	await get_tree().process_frame
	var root := get_parent()
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
	var bounds := _world_bounds()
	var height := maxf(bounds.size.y, 0.5)
	var bottom := bounds.position.y

	_groom = GroomGenerator.new()
	_groom.name = "ProceduralGroomGenerator"
	add_child(_groom)
	_groom.configure(_character, _meshes, bottom, height, _front_sign)
	_setup_ui()
	_apply_lash_variant(1 if _available_lash_count() > 0 else 0)
	_refresh_status()

func _setup_timers() -> void:
	_hair_timer = Timer.new()
	_hair_timer.one_shot = true
	_hair_timer.wait_time = 0.09
	_hair_timer.timeout.connect(_apply_hair_now)
	add_child(_hair_timer)

	_brow_timer = Timer.new()
	_brow_timer.one_shot = true
	_brow_timer.wait_time = 0.06
	_brow_timer.timeout.connect(_apply_brows_now)
	add_child(_brow_timer)

func _setup_ui() -> void:
	var layer := CanvasLayer.new()
	layer.name = "GroomLabUI"
	add_child(layer)

	var panel := PanelContainer.new()
	panel.set_anchors_preset(Control.PRESET_LEFT_WIDE)
	panel.offset_left = 18.0
	panel.offset_top = 94.0
	panel.offset_right = 372.0
	panel.offset_bottom = -18.0
	panel.add_theme_stylebox_override("panel", _panel_style(Color(0.045, 0.065, 0.085, 0.96)))
	layer.add_child(panel)

	var margin := MarginContainer.new()
	margin.add_theme_constant_override("margin_left", 14)
	margin.add_theme_constant_override("margin_right", 14)
	margin.add_theme_constant_override("margin_top", 14)
	margin.add_theme_constant_override("margin_bottom", 14)
	panel.add_child(margin)

	var scroll := ScrollContainer.new()
	scroll.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	margin.add_child(scroll)

	var column := VBoxContainer.new()
	column.custom_minimum_size.x = 300.0
	column.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	column.add_theme_constant_override("separation", 8)
	scroll.add_child(column)

	var title := Label.new()
	title.text = "GROOM"
	title.modulate = Color(0.58, 0.68, 0.77)
	title.add_theme_font_size_override("font_size", 13)
	column.add_child(title)

	var note := Label.new()
	note.text = "Runtime prototype: strand ribbons mounted to the head bone. Hair collision/secondary physics comes later."
	note.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	note.modulate = Color(0.63, 0.70, 0.76)
	note.add_theme_font_size_override("font_size", 12)
	column.add_child(note)

	_add_section(column, "EYELASHES")
	_lash_selector = OptionButton.new()
	_lash_selector.add_item("None")
	for i in LASH_TEXTURES.size():
		if ResourceLoader.exists(LASH_TEXTURES[i]):
			_lash_selector.add_item("Lashes %02d" % [i + 1], i + 1)
	_lash_selector.item_selected.connect(func(index: int) -> void:
		_apply_lash_variant(_lash_selector.get_item_id(index))
	)
	column.add_child(_lash_selector)
	_add_color_control(column, "Lash tint", Color.WHITE, func(color: Color) -> void:
		_lash_tint = color
		if _lash_selector != null and _lash_selector.selected >= 0:
			_apply_lash_variant(_lash_selector.get_item_id(_lash_selector.selected))
	)

	_add_section(column, "PROCEDURAL HAIR")
	var hair_enabled := CheckButton.new()
	hair_enabled.text = "Enabled"
	hair_enabled.button_pressed = bool(_hair_settings["enabled"])
	hair_enabled.toggled.connect(func(value: bool) -> void:
		_hair_settings["enabled"] = value
		_schedule_hair()
	)
	column.add_child(hair_enabled)

	var style := OptionButton.new()
	style.add_item("Natural growth")
	style.add_item("Swept back")
	style.add_item("Side part — left")
	style.add_item("Side part — right")
	style.add_item("Spiky / upright")
	style.item_selected.connect(func(index: int) -> void:
		_hair_settings["style"] = index
		_schedule_hair()
	)
	column.add_child(style)

	_add_color_control(column, "Hair color", _hair_settings["color"], func(color: Color) -> void:
		_hair_settings["color"] = color
		if _link_brow_color:
			_brow_settings["color"] = color.darkened(0.12)
			_schedule_brows()
		_schedule_hair()
	)
	_add_slider(column, "Density", 0.10, 1.0, 0.01, float(_hair_settings["density"]), "", func(v: float) -> void:
		_hair_settings["density"] = v
		_schedule_hair()
	)
	_add_slider(column, "Length", 0.5, 45.0, 0.5, float(_hair_settings["length"]) * 100.0, " cm", func(v: float) -> void:
		_hair_settings["length"] = v * 0.01
		_schedule_hair()
	)
	_add_slider(column, "Render strand width", 0.20, 2.50, 0.05, float(_hair_settings["width"]) * 1000.0, " mm", func(v: float) -> void:
		_hair_settings["width"] = v * 0.001
		_schedule_hair()
	)
	_add_slider(column, "Curl", 0.0, 1.0, 0.01, float(_hair_settings["curl"]), "", func(v: float) -> void:
		_hair_settings["curl"] = v
		_schedule_hair()
	)
	_add_slider(column, "Gravity / fall", 0.0, 1.0, 0.01, float(_hair_settings["gravity"]), "", func(v: float) -> void:
		_hair_settings["gravity"] = v
		_schedule_hair()
	)
	_add_slider(column, "Hairline height", 0.0, 1.0, 0.01, float(_hair_settings["hairline"]), "", func(v: float) -> void:
		_hair_settings["hairline"] = v
		_schedule_hair()
	)
	_add_slider(column, "Scalp scale", 0.82, 1.18, 0.01, float(_hair_settings["scalp_scale"]), "×", func(v: float) -> void:
		_hair_settings["scalp_scale"] = v
		_schedule_hair()
	)
	_add_slider(column, "Root lift", 0.0, 8.0, 0.25, float(_hair_settings["root_lift"]) * 1000.0, " mm", func(v: float) -> void:
		_hair_settings["root_lift"] = v * 0.001
		_schedule_hair()
	)
	var hair_rebuild := Button.new()
	hair_rebuild.text = "Regenerate hair"
	hair_rebuild.pressed.connect(_apply_hair_now)
	column.add_child(hair_rebuild)

	_add_section(column, "PROCEDURAL BROWS")
	var brows_enabled := CheckButton.new()
	brows_enabled.text = "Enabled"
	brows_enabled.button_pressed = bool(_brow_settings["enabled"])
	brows_enabled.toggled.connect(func(value: bool) -> void:
		_brow_settings["enabled"] = value
		_schedule_brows()
	)
	column.add_child(brows_enabled)

	var link_color := CheckButton.new()
	link_color.text = "Link brow color to hair"
	link_color.button_pressed = _link_brow_color
	link_color.toggled.connect(func(value: bool) -> void:
		_link_brow_color = value
		if value:
			_brow_settings["color"] = Color(_hair_settings["color"]).darkened(0.12)
			_schedule_brows()
	)
	column.add_child(link_color)

	_add_color_control(column, "Brow color", _brow_settings["color"], func(color: Color) -> void:
		_link_brow_color = false
		link_color.button_pressed = false
		_brow_settings["color"] = color
		_schedule_brows()
	)
	_add_slider(column, "Density", 0.10, 1.0, 0.01, float(_brow_settings["density"]), "", func(v: float) -> void:
		_brow_settings["density"] = v
		_schedule_brows()
	)
	_add_slider(column, "Brow width", 0.65, 1.35, 0.01, float(_brow_settings["width"]), "×", func(v: float) -> void:
		_brow_settings["width"] = v
		_schedule_brows()
	)
	_add_slider(column, "Render strand width", 0.15, 1.20, 0.05, float(_brow_settings["strand_width"]) * 1000.0, " mm", func(v: float) -> void:
		_brow_settings["strand_width"] = v * 0.001
		_schedule_brows()
	)
	_add_slider(column, "Arch", 0.0, 1.0, 0.01, float(_brow_settings["arch"]), "", func(v: float) -> void:
		_brow_settings["arch"] = v
		_schedule_brows()
	)
	_add_slider(column, "Vertical offset", -20.0, 20.0, 0.5, float(_brow_settings["height_offset"]) * 1000.0, " mm", func(v: float) -> void:
		_brow_settings["height_offset"] = v * 0.001
		_schedule_brows()
	)
	_add_slider(column, "Forward offset", -15.0, 20.0, 0.5, float(_brow_settings["forward_offset"]) * 1000.0, " mm", func(v: float) -> void:
		_brow_settings["forward_offset"] = v * 0.001
		_schedule_brows()
	)

	_add_section(column, "CALIBRATION")
	var front_axis := OptionButton.new()
	front_axis.add_item("Face points toward +Z", 1)
	front_axis.add_item("Face points toward -Z", -1)
	front_axis.item_selected.connect(func(index: int) -> void:
		_front_sign = float(front_axis.get_item_id(index))
		if _groom != null:
			_groom.set_front_sign(_front_sign)
		_refresh_status()
	)
	column.add_child(front_axis)

	var rebuild := Button.new()
	rebuild.text = "Regenerate hair + brows"
	rebuild.pressed.connect(func() -> void:
		_apply_hair_now()
		_apply_brows_now()
	)
	column.add_child(rebuild)

	_status = Label.new()
	_status.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_status.modulate = Color(0.63, 0.70, 0.76)
	_status.add_theme_font_size_override("font_size", 12)
	column.add_child(_status)

func _add_section(parent: VBoxContainer, text: String) -> void:
	parent.add_child(HSeparator.new())
	var label := Label.new()
	label.text = text
	label.modulate = Color(0.58, 0.68, 0.77)
	label.add_theme_font_size_override("font_size", 13)
	parent.add_child(label)

func _add_slider(parent: VBoxContainer, text: String, min_value: float, max_value: float, step: float, initial: float, suffix: String, changed: Callable) -> HSlider:
	var header := HBoxContainer.new()
	parent.add_child(header)
	var label := Label.new()
	label.text = text
	label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	header.add_child(label)
	var value_label := Label.new()
	value_label.custom_minimum_size.x = 66.0
	value_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	value_label.text = _format_value(initial, step, suffix)
	header.add_child(value_label)

	var slider := HSlider.new()
	slider.min_value = min_value
	slider.max_value = max_value
	slider.step = step
	slider.value = initial
	slider.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	slider.value_changed.connect(func(value: float) -> void:
		value_label.text = _format_value(value, step, suffix)
		changed.call(value)
	)
	parent.add_child(slider)
	return slider

func _add_color_control(parent: VBoxContainer, text: String, initial: Color, changed: Callable) -> ColorPickerButton:
	var row := HBoxContainer.new()
	parent.add_child(row)
	var label := Label.new()
	label.text = text
	label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(label)
	var picker := ColorPickerButton.new()
	picker.color = initial
	picker.custom_minimum_size = Vector2(92.0, 32.0)
	picker.color_changed.connect(func(color: Color) -> void: changed.call(color))
	row.add_child(picker)
	return picker

func _format_value(value: float, step: float, suffix: String) -> String:
	if step >= 0.5:
		return "%.1f%s" % [value, suffix]
	if step >= 0.05:
		return "%.2f%s" % [value, suffix]
	return "%.2f%s" % [value, suffix]

func _schedule_hair() -> void:
	if _hair_timer != null:
		_hair_timer.start()

func _schedule_brows() -> void:
	if _brow_timer != null:
		_brow_timer.start()

func _apply_hair_now() -> void:
	if _groom != null:
		_groom.apply_hair(_hair_settings)
		_refresh_status()

func _apply_brows_now() -> void:
	if _groom != null:
		_groom.apply_brows(_brow_settings)
		_refresh_status()

func _collect_meshes(node: Node) -> void:
	if node is MeshInstance3D:
		_meshes.append(node as MeshInstance3D)
	for child in node.get_children():
		_collect_meshes(child)

func _discover_lashes() -> void:
	_lash_meshes.clear()
	_lash_original_materials.clear()
	for mesh_instance in _meshes:
		var mesh_match := mesh_instance.name.to_lower().contains("lash")
		if mesh_instance.mesh == null:
			continue
		for surface in mesh_instance.mesh.get_surface_count():
			var mat := mesh_instance.get_active_material(surface)
			if _material_looks_like_lash(mat):
				mesh_match = true
			if mat != null:
				_lash_original_materials[_lash_key(mesh_instance, surface)] = mat
		if mesh_match:
			_lash_meshes.append(mesh_instance)

func _material_looks_like_lash(material: Material) -> bool:
	if material == null:
		return false
	if material.resource_name.to_lower().contains("lash"):
		return true
	if material is BaseMaterial3D:
		var base := material as BaseMaterial3D
		if base.albedo_texture != null:
			var path := base.albedo_texture.resource_path.to_lower()
			if path.contains("eyelash") or path.contains("lash"):
				return true
	return false

func _apply_lash_variant(variant: int) -> void:
	for mesh_instance in _lash_meshes:
		mesh_instance.visible = variant != 0
	if variant <= 0 or variant > LASH_TEXTURES.size():
		_refresh_status()
		return
	var path: String = LASH_TEXTURES[variant - 1]
	if not ResourceLoader.exists(path):
		_refresh_status()
		return
	var texture := load(path) as Texture2D
	if texture == null:
		return

	for mesh_instance in _lash_meshes:
		if mesh_instance.mesh == null:
			continue
		for surface in mesh_instance.mesh.get_surface_count():
			var key := _lash_key(mesh_instance, surface)
			var original: Material = _lash_original_materials.get(key, mesh_instance.get_active_material(surface))
			if original == null:
				continue
			var duplicate := original.duplicate(true) as Material
			if duplicate is BaseMaterial3D:
				var base := duplicate as BaseMaterial3D
				base.albedo_texture = texture
				base.albedo_color = _lash_tint
				mesh_instance.set_surface_override_material(surface, base)
	_refresh_status()

func _lash_key(mesh_instance: MeshInstance3D, surface: int) -> String:
	return "%d:%d" % [mesh_instance.get_instance_id(), surface]

func _available_lash_count() -> int:
	var count := 0
	for path in LASH_TEXTURES:
		if ResourceLoader.exists(path):
			count += 1
	return count

func _world_bounds() -> AABB:
	var min_point := Vector3(INF, INF, INF)
	var max_point := Vector3(-INF, -INF, -INF)
	var found := false
	for mesh_instance in _meshes:
		if mesh_instance.mesh == null:
			continue
		var local := mesh_instance.get_aabb()
		for x in 2:
			for y in 2:
				for z in 2:
					var p := local.position + Vector3(local.size.x * x, local.size.y * y, local.size.z * z)
					var world := mesh_instance.to_global(p)
					min_point.x = minf(min_point.x, world.x)
					min_point.y = minf(min_point.y, world.y)
					min_point.z = minf(min_point.z, world.z)
					max_point.x = maxf(max_point.x, world.x)
					max_point.y = maxf(max_point.y, world.y)
					max_point.z = maxf(max_point.z, world.z)
					found = true
	if not found:
		return AABB(Vector3.ZERO, Vector3(0.5, 1.75, 0.5))
	return AABB(min_point, max_point - min_point)

func _refresh_status() -> void:
	if _status == null:
		return
	var mesh_names: Array[String] = []
	for mesh_instance in _meshes:
		mesh_names.append(str(mesh_instance.name))
	var lash_note := "%d lash mesh(es), %d variants" % [_lash_meshes.size(), _available_lash_count()]
	var groom_note := _groom.diagnostics() if _groom != null else "Groom generator unavailable"
	_status.text = "%s\n%s" % [lash_note, groom_note]
	if _lash_meshes.is_empty():
		_status.text += "\nNo lash mesh auto-detected. Meshes: %s" % ", ".join(mesh_names)

func _panel_style(color: Color) -> StyleBoxFlat:
	var style := StyleBoxFlat.new()
	style.bg_color = color
	style.border_color = Color(0.16, 0.22, 0.28)
	style.set_border_width_all(1)
	style.corner_radius_top_left = 8
	style.corner_radius_top_right = 8
	style.corner_radius_bottom_left = 8
	style.corner_radius_bottom_right = 8
	return style
