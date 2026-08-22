extends Node3D

const CHARACTER_PATH := "res://assets/character/asterrahuman.glb"
const MENU_SCENE := "res://scenes/StartMenu.tscn"
const PLATFORM_TOP := 0.10

var _character: Node3D
var _meshes: Array[MeshInstance3D] = []
var _shape_names: Array[String] = []
var _filtered_shapes: Array[String] = []
var _default_shape_values: Dictionary = {}

var _camera_pivot: Node3D
var _camera: Camera3D
var _turntable_root: Node3D
var _camera_distance := 3.0
var _camera_target_position := Vector3.ZERO
var _camera_target_rotation := Vector3.ZERO
var _camera_target_distance := 3.0
var _camera_transitioning := false
var _camera_focus_local := Vector3.ZERO
var _follow_turntable_focus := false
var _character_height := 1.75
var _character_bottom := PLATFORM_TOP
var _orbiting := false
var _turntable_enabled := false

var _shape_selector: OptionButton
var _shape_slider: HSlider
var _shape_value_label: Label
var _shape_search: LineEdit
var _status_label: Label
var _updating_shape_ui := false

func _ready() -> void:
	Input.set_mouse_mode(Input.MOUSE_MODE_VISIBLE)
	GraphicsQuality.configure_viewport(get_viewport(), AppSettings.graphics_quality)
	_setup_environment()
	_setup_stage()
	_setup_camera()
	_load_character()
	_setup_ui()
	_refresh_shape_controls()

func _process(delta: float) -> void:
	if _turntable_enabled and _turntable_root != null:
		# Rotate one common parent so the imported rig, bone attachments and every
		# procedural groom surface share exactly the same transform for this frame.
		_turntable_root.rotate_y(delta * 0.35)
	_update_camera_transition(delta)

func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventKey and event.pressed and not event.echo and event.keycode == KEY_ESCAPE:
		get_tree().change_scene_to_file(MENU_SCENE)
		return

	if event is InputEventMouseButton:
		if event.button_index == MOUSE_BUTTON_RIGHT:
			_orbiting = event.pressed
			return
		if event.pressed and event.button_index == MOUSE_BUTTON_WHEEL_UP:
			_camera_transitioning = false
			_camera_distance = maxf(0.45, _camera_distance * 0.90)
			_camera_target_distance = _camera_distance
			_update_camera_distance()
			return
		if event.pressed and event.button_index == MOUSE_BUTTON_WHEEL_DOWN:
			_camera_transitioning = false
			_camera_distance = minf(12.0, _camera_distance * 1.11)
			_camera_target_distance = _camera_distance
			_update_camera_distance()
			return

	if event is InputEventMouseMotion and _orbiting:
		_camera_transitioning = false
		_camera_pivot.rotation.y -= event.relative.x * 0.006
		_camera_pivot.rotation.x = clampf(
			_camera_pivot.rotation.x - event.relative.y * 0.006,
			deg_to_rad(-65.0),
			deg_to_rad(65.0)
		)

func _setup_environment() -> void:
	var world_environment := WorldEnvironment.new()
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color("10161d")
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.ambient_light_color = Color("9eafbd")
	environment.ambient_light_energy = 0.38
	GraphicsQuality.configure_studio_environment(environment, AppSettings.graphics_quality)
	world_environment.environment = environment
	add_child(world_environment)

	var key := DirectionalLight3D.new()
	key.name = "KeyLight"
	key.rotation_degrees = Vector3(-38.0, -32.0, 0.0)
	key.light_color = Color("fff1df")
	key.light_energy = 1.35
	key.shadow_enabled = true
	key.directional_shadow_max_distance = 12.0
	GraphicsQuality.configure_sun(key, AppSettings.graphics_quality, true)
	add_child(key)

	var fill := DirectionalLight3D.new()
	fill.name = "FillLight"
	fill.rotation_degrees = Vector3(-15.0, 145.0, 20.0)
	fill.light_color = Color("c7d9ff")
	fill.light_energy = 0.48
	fill.shadow_enabled = false
	add_child(fill)

	var rim := DirectionalLight3D.new()
	rim.name = "RimLight"
	rim.rotation_degrees = Vector3(-10.0, 205.0, -15.0)
	rim.light_color = Color("dcecff")
	rim.light_energy = 0.72
	rim.shadow_enabled = false
	add_child(rim)

func _setup_stage() -> void:
	_turntable_root = Node3D.new()
	_turntable_root.name = "CharacterTurntable"
	add_child(_turntable_root)

	var floor := MeshInstance3D.new()
	floor.name = "StudioFloor"
	var plane := PlaneMesh.new()
	plane.size = Vector2(12.0, 12.0)
	floor.mesh = plane
	floor.material_override = _make_material(Color("202933"), 0.82)
	add_child(floor)

	var backdrop := MeshInstance3D.new()
	backdrop.name = "Backdrop"
	var backdrop_mesh := BoxMesh.new()
	backdrop_mesh.size = Vector3(8.0, 5.0, 0.12)
	backdrop.mesh = backdrop_mesh
	backdrop.position = Vector3(0.0, 2.5, -2.15)
	backdrop.material_override = _make_material(Color("18212a"), 0.9)
	add_child(backdrop)

	var platform := MeshInstance3D.new()
	platform.name = "CharacterPlatform"
	var cylinder := CylinderMesh.new()
	cylinder.top_radius = 1.25
	cylinder.bottom_radius = 1.25
	cylinder.height = PLATFORM_TOP
	platform.mesh = cylinder
	platform.position.y = PLATFORM_TOP * 0.5
	platform.material_override = _make_material(Color("303b45"), 0.62)
	add_child(platform)

func _make_material(color: Color, roughness: float) -> StandardMaterial3D:
	var material := StandardMaterial3D.new()
	material.albedo_color = color
	material.roughness = roughness
	material.metallic = 0.0
	return material

func _setup_camera() -> void:
	_camera_pivot = Node3D.new()
	_camera_pivot.name = "CameraPivot"
	add_child(_camera_pivot)

	_camera = Camera3D.new()
	_camera.name = "StudioCamera"
	_camera.fov = 38.0
	_camera.near = 0.03
	_camera.current = true
	_camera_pivot.add_child(_camera)
	_update_camera_distance()

func _load_character() -> void:
	var packed := load(CHARACTER_PATH) as PackedScene
	if packed == null:
		push_error("Character editor could not load %s" % CHARACTER_PATH)
		return

	var instance := packed.instantiate()
	if not (instance is Node3D):
		push_error("Asterra human GLB root is not a Node3D")
		instance.queue_free()
		return

	_character = instance as Node3D
	_character.name = "AsterraHuman"
	_turntable_root.add_child(_character)
	_collect_meshes(_character)

	if _meshes.is_empty():
		push_warning("Asterra human contains no MeshInstance3D nodes")
		return

	_center_character_on_stage()
	_collect_shape_names()
	_capture_shape_defaults()

func _collect_meshes(node: Node) -> void:
	if node is MeshInstance3D:
		_meshes.append(node as MeshInstance3D)
	for child in node.get_children():
		_collect_meshes(child)

func _center_character_on_stage() -> void:
	var bounds := _world_bounds()
	if bounds.size.length_squared() <= 0.000001:
		return

	var center := bounds.position + bounds.size * 0.5
	_character.position += Vector3(-center.x, PLATFORM_TOP - bounds.position.y, -center.z)

	bounds = _world_bounds()
	_character_height = maxf(bounds.size.y, 0.5)
	_character_bottom = bounds.position.y
	_focus_full_body()

func _world_bounds() -> AABB:
	var min_point := Vector3(INF, INF, INF)
	var max_point := Vector3(-INF, -INF, -INF)
	var found := false

	for mesh_instance in _meshes:
		if mesh_instance.mesh == null:
			continue
		var local_bounds := mesh_instance.get_aabb()
		for x in 2:
			for y in 2:
				for z in 2:
					var point := local_bounds.position + Vector3(
						local_bounds.size.x * float(x),
						local_bounds.size.y * float(y),
						local_bounds.size.z * float(z)
					)
					var world_point := mesh_instance.to_global(point)
					min_point = Vector3(
						minf(min_point.x, world_point.x),
						minf(min_point.y, world_point.y),
						minf(min_point.z, world_point.z)
					)
					max_point = Vector3(
						maxf(max_point.x, world_point.x),
						maxf(max_point.y, world_point.y),
						maxf(max_point.z, world_point.z)
					)
					found = true

	if not found:
		return AABB()
	return AABB(min_point, max_point - min_point)

func _collect_shape_names() -> void:
	var unique := {}
	for mesh_instance in _meshes:
		if mesh_instance.mesh == null:
			continue
		for index in mesh_instance.mesh.get_blend_shape_count():
			unique[str(mesh_instance.mesh.get_blend_shape_name(index))] = true

	_shape_names.clear()
	for name in unique.keys():
		_shape_names.append(str(name))
	_shape_names.sort()

func _capture_shape_defaults() -> void:
	_default_shape_values.clear()
	for mesh_instance in _meshes:
		if mesh_instance.mesh == null:
			continue
		for index in mesh_instance.mesh.get_blend_shape_count():
			_default_shape_values[_shape_key(mesh_instance, index)] = mesh_instance.get_blend_shape_value(index)

func _shape_key(mesh_instance: MeshInstance3D, index: int) -> String:
	return "%d:%d" % [mesh_instance.get_instance_id(), index]

func _setup_ui() -> void:
	var layer := CanvasLayer.new()
	layer.name = "StudioUI"
	add_child(layer)

	var top_bar := PanelContainer.new()
	top_bar.set_anchors_preset(Control.PRESET_TOP_WIDE)
	top_bar.offset_left = 18.0
	top_bar.offset_top = 18.0
	top_bar.offset_right = -18.0
	top_bar.offset_bottom = 76.0
	top_bar.add_theme_stylebox_override("panel", _panel_style(Color(0.045, 0.065, 0.085, 0.94)))
	layer.add_child(top_bar)

	var top_row := HBoxContainer.new()
	top_row.add_theme_constant_override("separation", 12)
	top_bar.add_child(top_row)

	var back := Button.new()
	back.text = "←  Menu"
	back.custom_minimum_size.x = 110.0
	back.pressed.connect(func() -> void: get_tree().change_scene_to_file(MENU_SCENE))
	top_row.add_child(back)

	var title := Label.new()
	title.text = "Character Studio"
	title.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	title.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	title.add_theme_font_size_override("font_size", 22)
	top_row.add_child(title)

	var help := Label.new()
	help.text = "RMB drag: orbit   •   Wheel: zoom   •   Esc: menu"
	help.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	help.modulate = Color(0.67, 0.73, 0.79)
	top_row.add_child(help)

	var panel := PanelContainer.new()
	panel.name = "MorphPanel"
	panel.set_anchors_preset(Control.PRESET_RIGHT_WIDE)
	panel.offset_left = -380.0
	panel.offset_top = 94.0
	panel.offset_right = -18.0
	panel.offset_bottom = -18.0
	panel.add_theme_stylebox_override("panel", _panel_style(Color(0.045, 0.065, 0.085, 0.96)))
	layer.add_child(panel)

	var margin := MarginContainer.new()
	margin.add_theme_constant_override("margin_left", 16)
	margin.add_theme_constant_override("margin_right", 16)
	margin.add_theme_constant_override("margin_top", 16)
	margin.add_theme_constant_override("margin_bottom", 16)
	panel.add_child(margin)

	var column := VBoxContainer.new()
	column.add_theme_constant_override("separation", 10)
	margin.add_child(column)

	var section := Label.new()
	section.text = "VIEW"
	section.modulate = Color(0.58, 0.68, 0.77)
	section.add_theme_font_size_override("font_size", 13)
	column.add_child(section)

	var camera_row := HBoxContainer.new()
	camera_row.add_theme_constant_override("separation", 8)
	column.add_child(camera_row)

	var body_button := Button.new()
	body_button.text = "Full body"
	body_button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	body_button.pressed.connect(_focus_full_body)
	camera_row.add_child(body_button)

	var face_button := Button.new()
	face_button.text = "Face"
	face_button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	face_button.pressed.connect(_focus_face)
	camera_row.add_child(face_button)

	var turntable := CheckButton.new()
	turntable.text = "Slow turntable"
	turntable.toggled.connect(func(enabled: bool) -> void: _turntable_enabled = enabled)
	column.add_child(turntable)

	column.add_child(HSeparator.new())

	var morph_section := Label.new()
	morph_section.text = "FACE / MORPH TEST"
	morph_section.modulate = Color(0.58, 0.68, 0.77)
	morph_section.add_theme_font_size_override("font_size", 13)
	column.add_child(morph_section)

	_shape_search = LineEdit.new()
	_shape_search.placeholder_text = "Filter blend shapes…"
	_shape_search.text_changed.connect(func(_text: String) -> void: _refresh_shape_controls())
	column.add_child(_shape_search)

	_shape_selector = OptionButton.new()
	_shape_selector.item_selected.connect(_on_shape_selected)
	column.add_child(_shape_selector)

	var value_row := HBoxContainer.new()
	column.add_child(value_row)

	_shape_slider = HSlider.new()
	_shape_slider.min_value = 0.0
	_shape_slider.max_value = 1.0
	_shape_slider.step = 0.01
	_shape_slider.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_shape_slider.value_changed.connect(_on_shape_value_changed)
	value_row.add_child(_shape_slider)

	_shape_value_label = Label.new()
	_shape_value_label.text = "0.00"
	_shape_value_label.custom_minimum_size.x = 48.0
	_shape_value_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	value_row.add_child(_shape_value_label)

	var preset_label := Label.new()
	preset_label.text = "Quick tests"
	preset_label.modulate = Color(0.68, 0.74, 0.80)
	column.add_child(preset_label)

	var preset_row := HBoxContainer.new()
	preset_row.add_theme_constant_override("separation", 6)
	column.add_child(preset_row)
	_add_preset_button(preset_row, "Smile", "smile")
	_add_preset_button(preset_row, "Blink", "blink")
	_add_preset_button(preset_row, "Jaw", "jaw")

	var reset := Button.new()
	reset.text = "Reset to imported defaults"
	reset.pressed.connect(_reset_all_shapes)
	column.add_child(reset)

	column.add_child(HSeparator.new())

	_status_label = Label.new()
	_status_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_status_label.modulate = Color(0.65, 0.72, 0.78)
	_status_label.add_theme_font_size_override("font_size", 13)
	column.add_child(_status_label)

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

func _add_preset_button(parent: HBoxContainer, text: String, preset: String) -> void:
	var button := Button.new()
	button.text = text
	button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	button.pressed.connect(func() -> void: _apply_quick_test(preset))
	parent.add_child(button)

func _refresh_shape_controls() -> void:
	if _shape_selector == null:
		return

	var previous := ""
	if _shape_selector.item_count > 0 and _shape_selector.selected >= 0:
		previous = _shape_selector.get_item_text(_shape_selector.selected)

	var filter := _shape_search.text.strip_edges().to_lower() if _shape_search != null else ""
	_filtered_shapes.clear()
	for shape_name in _shape_names:
		if filter.is_empty() or shape_name.to_lower().contains(filter):
			_filtered_shapes.append(shape_name)

	_shape_selector.clear()
	var selected_index := 0
	for index in _filtered_shapes.size():
		_shape_selector.add_item(_filtered_shapes[index])
		if _filtered_shapes[index] == previous:
			selected_index = index

	if not _filtered_shapes.is_empty():
		_shape_selector.select(selected_index)
		_on_shape_selected(selected_index)
	else:
		_updating_shape_ui = true
		_shape_slider.value = 0.0
		_shape_value_label.text = "—"
		_updating_shape_ui = false

	if _status_label != null:
		_status_label.text = "%d mesh nodes • %d blend shapes detected\nSource: %s" % [
			_meshes.size(),
			_shape_names.size(),
			CHARACTER_PATH
		]

func _on_shape_selected(index: int) -> void:
	if index < 0 or index >= _filtered_shapes.size():
		return
	var shape_name := _filtered_shapes[index]
	var value := _get_shape_value(shape_name)
	_updating_shape_ui = true
	_shape_slider.value = value
	_shape_value_label.text = "%.2f" % value
	_updating_shape_ui = false

func _on_shape_value_changed(value: float) -> void:
	if _updating_shape_ui or _shape_selector == null or _shape_selector.selected < 0:
		return
	if _shape_selector.selected >= _filtered_shapes.size():
		return
	var shape_name := _filtered_shapes[_shape_selector.selected]
	_set_shape_value(shape_name, value)
	_shape_value_label.text = "%.2f" % value

func _get_shape_value(shape_name: String) -> float:
	for mesh_instance in _meshes:
		var index := mesh_instance.find_blend_shape_by_name(StringName(shape_name))
		if index >= 0:
			return mesh_instance.get_blend_shape_value(index)
	return 0.0

func _set_shape_value(shape_name: String, value: float) -> void:
	for mesh_instance in _meshes:
		var index := mesh_instance.find_blend_shape_by_name(StringName(shape_name))
		if index >= 0:
			mesh_instance.set_blend_shape_value(index, value)

func _reset_all_shapes() -> void:
	for mesh_instance in _meshes:
		if mesh_instance.mesh == null:
			continue
		for index in mesh_instance.mesh.get_blend_shape_count():
			var key := _shape_key(mesh_instance, index)
			mesh_instance.set_blend_shape_value(index, float(_default_shape_values.get(key, 0.0)))
	if _shape_selector != null and _shape_selector.selected >= 0:
		_on_shape_selected(_shape_selector.selected)

func _apply_quick_test(preset: String) -> void:
	_reset_all_shapes()
	match preset:
		"smile":
			_set_shape_by_hint(["mouthsmileleft", "smileleft"], 0.8)
			_set_shape_by_hint(["mouthsmileright", "smileright"], 0.8)
			_set_shape_by_hint(["cheeksquintleft"], 0.25)
			_set_shape_by_hint(["cheeksquintright"], 0.25)
		"blink":
			_set_shape_by_hint(["eyeblinkleft", "blinkleft"], 1.0)
			_set_shape_by_hint(["eyeblinkright", "blinkright"], 1.0)
		"jaw":
			_set_shape_by_hint(["jawopen"], 0.85)
	if _shape_selector != null and _shape_selector.selected >= 0:
		_on_shape_selected(_shape_selector.selected)

func _set_shape_by_hint(hints: Array[String], value: float) -> bool:
	for shape_name in _shape_names:
		var normalized := shape_name.to_lower().replace("_", "").replace("-", "")
		for hint in hints:
			var normalized_hint := hint.to_lower().replace("_", "").replace("-", "")
			if normalized.contains(normalized_hint):
				_set_shape_value(shape_name, value)
				return true
	return false

func _focus_full_body() -> void:
	if _camera_pivot == null:
		return
	_set_turntable_camera_target(
		Vector3(0.0, _character_bottom + _character_height * 0.52, 0.0),
		maxf(2.2, _character_height * 1.65),
		Vector3(deg_to_rad(-2.0), 0.0, 0.0)
	)

func _focus_face() -> void:
	if _camera_pivot == null:
		return
	_set_turntable_camera_target(
		Vector3(0.0, _character_bottom + _character_height * 0.87, 0.0),
		maxf(0.65, _character_height * 0.48),
		Vector3.ZERO
	)

func focus_customization_part(part: String) -> void:
	var height := maxf(_character_height, 0.5)
	var y_ratio := 0.87
	var distance_ratio := 0.34
	var minimum_distance := 0.52
	match part:
		"hair":
			y_ratio = 1.02
			distance_ratio = 0.36
			minimum_distance = 0.58
		"brows":
			y_ratio = 0.98
			distance_ratio = 0.30
			minimum_distance = 0.49
		"eyes":
			y_ratio = 0.965
			distance_ratio = 0.29
			minimum_distance = 0.48
		"mustache":
			y_ratio = 0.92
			distance_ratio = 0.30
			minimum_distance = 0.50
		"beard":
			y_ratio = 0.875
			distance_ratio = 0.34
			minimum_distance = 0.56
		"skin":
			y_ratio = 0.80
			distance_ratio = 0.58
			minimum_distance = 0.88
	_set_turntable_camera_target(
		Vector3(0.0, _character_bottom + height * y_ratio, 0.0),
		maxf(minimum_distance, height * distance_ratio),
		Vector3.ZERO
	)

func _set_turntable_camera_target(local_position: Vector3, distance: float, rotation: Vector3) -> void:
	_camera_focus_local = local_position
	_follow_turntable_focus = _turntable_root != null
	var world_position := _turntable_root.to_global(local_position) if _turntable_root != null else local_position
	_set_camera_target(world_position, distance, rotation)

func _set_camera_target(position: Vector3, distance: float, rotation: Vector3) -> void:
	_camera_target_position = position
	_camera_target_distance = clampf(distance, 0.45, 12.0)
	_camera_target_rotation = rotation
	_camera_transitioning = true

func _update_camera_transition(delta: float) -> void:
	if _camera_pivot == null or _camera == null:
		return
	if _follow_turntable_focus and _turntable_root != null:
		_camera_target_position = _turntable_root.to_global(_camera_focus_local)
	if not _camera_transitioning and not _follow_turntable_focus:
		return
	var weight := 1.0 - exp(-delta * 7.5)
	_camera_pivot.position = _camera_pivot.position.lerp(_camera_target_position, weight)
	var current_rotation := _camera_pivot.rotation
	if _camera_transitioning:
		current_rotation.x = lerp_angle(current_rotation.x, _camera_target_rotation.x, weight)
		current_rotation.y = lerp_angle(current_rotation.y, _camera_target_rotation.y, weight)
		current_rotation.z = lerp_angle(current_rotation.z, _camera_target_rotation.z, weight)
		_camera_pivot.rotation = current_rotation
		_camera_distance = lerpf(_camera_distance, _camera_target_distance, weight)
	_update_camera_distance()
	if _camera_transitioning and _camera_pivot.position.distance_to(_camera_target_position) < 0.0005 \
	and absf(_camera_distance - _camera_target_distance) < 0.0005 \
	and current_rotation.distance_to(_camera_target_rotation) < 0.0005:
		_camera_pivot.position = _camera_target_position
		_camera_pivot.rotation = _camera_target_rotation
		_camera_distance = _camera_target_distance
		_update_camera_distance()
		_camera_transitioning = false

func _update_camera_distance() -> void:
	if _camera != null:
		_camera.position = Vector3(0.0, 0.0, _camera_distance)
