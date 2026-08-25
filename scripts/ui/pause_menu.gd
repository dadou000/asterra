class_name PauseMenu
extends CanvasLayer
## Normal in-game pause flow. Escape owns this menu; the developer debug menu
## remains on '&'. Settings mirror the launcher and write through AppSettings.

signal opened
signal closed

const START_MENU_SCENE := "res://scenes/StartMenu.tscn"
const GRAPHICS_DEBUG_VIEWS: Array[Dictionary] = [
	{
		"name": "Final composite",
		"mode": Viewport.DEBUG_DRAW_DISABLED,
		"description": "Normal rendering with every enabled effect composited together.",
	},
	{
		"name": "Lighting only",
		"mode": Viewport.DEBUG_DRAW_LIGHTING,
		"description": "Lighting without material textures, useful for reading direct-light and shadow shape.",
	},
	{
		"name": "Unshaded materials",
		"mode": Viewport.DEBUG_DRAW_UNSHADED,
		"description": "Material color without lighting, for separating texture detail from illumination.",
	},
	{
		"name": "Normal buffer",
		"mode": Viewport.DEBUG_DRAW_NORMAL_BUFFER,
		"description": "World-space surface normals used by screen-space lighting effects.",
	},
	{
		"name": "SSAO only",
		"mode": Viewport.DEBUG_DRAW_SSAO,
		"description": "The raw screen-space ambient occlusion texture. SSAO must be enabled above.",
	},
	{
		"name": "SSIL only",
		"mode": Viewport.DEBUG_DRAW_SSIL,
		"description": "The raw screen-space indirect-light texture. SSIL must be enabled above.",
	},
	{
		"name": "GI result buffer",
		"mode": Viewport.DEBUG_DRAW_GI_BUFFER,
		"description": "The global-illumination result contributed by SDFGI or VoxelGI.",
	},
	{
		"name": "SDFGI cascades",
		"mode": Viewport.DEBUG_DRAW_SDFGI,
		"description": "The signed-distance cascades that move with the camera. SDFGI must be enabled.",
	},
	{
		"name": "SDFGI probes",
		"mode": Viewport.DEBUG_DRAW_SDFGI_PROBES,
		"description": "The lighting probes populated inside the SDFGI cascades.",
	},
	{
		"name": "Directional shadow atlas",
		"mode": Viewport.DEBUG_DRAW_DIRECTIONAL_SHADOW_ATLAS,
		"description": "The sun shadow-map atlas, displayed in the upper-left of the scene.",
	},
	{
		"name": "Sun cascade regions",
		"mode": Viewport.DEBUG_DRAW_PSSM_SPLITS,
		"description": "Colors directional-shadow splits red, green, blue and yellow from near to far.",
	},
	{
		"name": "Scene luminance",
		"mode": Viewport.DEBUG_DRAW_SCENE_LUMINANCE,
		"description": "The luminance buffer used by automatic exposure.",
	},
	{
		"name": "Internal pre-FX buffer",
		"mode": Viewport.DEBUG_DRAW_INTERNAL_BUFFER,
		"description": "The internal-resolution image before tonemapping and post-processing.",
	},
	{
		"name": "Motion vectors",
		"mode": Viewport.DEBUG_DRAW_MOTION_VECTORS,
		"description": "Per-pixel motion vectors used by temporal rendering features.",
	},
	{
		"name": "Overdraw",
		"mode": Viewport.DEBUG_DRAW_OVERDRAW,
		"description": "Additive mesh coverage; brighter regions redraw the same pixels more often.",
	},
	{
		"name": "Wireframe",
		"mode": Viewport.DEBUG_DRAW_WIREFRAME,
		"description": "Triangle edges only, useful for checking terrain density and mesh LOD.",
	},
]

var enabled: bool = true
var _root: Control
var _pause_center: CenterContainer
var _settings_center: CenterContainer


func _ready() -> void:
	layer = 30
	process_mode = Node.PROCESS_MODE_ALWAYS
	visible = false
	# Render diagnostics are deliberately session-only. Never strand the next game
	# or launcher scene in a debug buffer because the previous run ended there.
	get_viewport().debug_draw = Viewport.DEBUG_DRAW_DISABLED
	OceanSystem.set_debug_waves_disabled(false)
	_build_ui()


func set_enabled(value: bool) -> void:
	enabled = value
	if not enabled and visible:
		resume()


func is_open() -> bool:
	return visible


func _input(event: InputEvent) -> void:
	if not (event is InputEventKey):
		return
	var key := event as InputEventKey
	if not key.pressed or key.echo:
		return

	# Do not let the developer '&' overlay open behind the pause layer.
	if visible and key.unicode == 38:
		get_viewport().set_input_as_handled()
		return
	if key.keycode != KEY_ESCAPE:
		return

	if visible:
		if _settings_center != null and _settings_center.visible:
			_show_pause_menu()
		else:
			resume()
	elif _can_pause():
		open()
	else:
		return
	get_viewport().set_input_as_handled()


func _can_pause() -> bool:
	return enabled and Planet.ready_state and _find_player() != null


func open() -> void:
	if visible or not _can_pause():
		return
	# The debug menu is a separate developer flow. Close it before pausing so there
	# is only one owner of mouse capture and player input.
	var debug_menu := _find_debug_menu()
	if debug_menu != null and debug_menu.visible:
		debug_menu.toggle()

	var player := _find_player()
	if player != null:
		player.set_mouse_captured(false)
		player.input_enabled = false
	visible = true
	_show_pause_menu()
	Input.set_mouse_mode(Input.MOUSE_MODE_VISIBLE)
	opened.emit()
	get_tree().paused = true


func resume() -> void:
	if not visible:
		return
	get_tree().paused = false
	visible = false
	var player := _find_player()
	var debug_menu := _find_debug_menu()
	if player != null:
		var debug_open: bool = debug_menu != null and debug_menu.visible
		player.input_enabled = not debug_open
		if not debug_open:
			player.set_mouse_captured(true)
	closed.emit()


func _build_ui() -> void:
	_root = Control.new()
	_root.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_root.mouse_filter = Control.MOUSE_FILTER_STOP
	add_child(_root)

	var dim := ColorRect.new()
	dim.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	dim.color = Color(0.018, 0.026, 0.038, 0.78)
	dim.mouse_filter = Control.MOUSE_FILTER_STOP
	_root.add_child(dim)

	var shade := ColorRect.new()
	shade.set_anchors_preset(Control.PRESET_CENTER)
	shade.position = Vector2(-380.0, -300.0)
	shade.size = Vector2(760.0, 600.0)
	shade.color = Color(0.055, 0.085, 0.115, 0.44)
	shade.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_root.add_child(shade)

	_build_pause_menu()
	_build_settings_menu()


func _build_pause_menu() -> void:
	_pause_center = CenterContainer.new()
	_pause_center.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_root.add_child(_pause_center)

	var panel := PanelContainer.new()
	panel.custom_minimum_size = Vector2(470.0, 0.0)
	panel.add_theme_stylebox_override("panel", _panel_style())
	_pause_center.add_child(panel)

	var margin := MarginContainer.new()
	margin.add_theme_constant_override("margin_left", 24)
	margin.add_theme_constant_override("margin_right", 24)
	margin.add_theme_constant_override("margin_top", 24)
	margin.add_theme_constant_override("margin_bottom", 24)
	panel.add_child(margin)

	var column := VBoxContainer.new()
	column.add_theme_constant_override("separation", 12)
	margin.add_child(column)

	var title := Label.new()
	title.text = "ASTERRA"
	title.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	title.add_theme_font_size_override("font_size", 42)
	column.add_child(title)

	var subtitle := Label.new()
	subtitle.text = "PAUSED"
	subtitle.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	subtitle.modulate = Color(0.62, 0.69, 0.75)
	subtitle.add_theme_font_size_override("font_size", 14)
	column.add_child(subtitle)

	var spacer := Control.new()
	spacer.custom_minimum_size.y = 12.0
	column.add_child(spacer)

	_add_button(column, "RESUME", resume)
	_add_button(column, "SETTINGS", _show_settings_menu)

	column.add_child(HSeparator.new())

	var main_menu := Button.new()
	main_menu.text = "Return to main menu"
	main_menu.custom_minimum_size.y = 42.0
	main_menu.pressed.connect(_return_to_main_menu)
	column.add_child(main_menu)

	var quit := Button.new()
	quit.text = "Quit"
	quit.custom_minimum_size.y = 42.0
	quit.pressed.connect(_quit)
	column.add_child(quit)

	var hint := Label.new()
	hint.text = "Esc resumes  •  & opens developer debug while playing"
	hint.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	hint.modulate = Color(0.46, 0.53, 0.60)
	hint.add_theme_font_size_override("font_size", 12)
	column.add_child(hint)


func _build_settings_menu() -> void:
	_settings_center = CenterContainer.new()
	_settings_center.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_root.add_child(_settings_center)

	var scroll := ScrollContainer.new()
	# CenterContainer sizes children from their minimum size. A zero minimum
	# height collapsed this entire settings tree, leaving only the dark backdrop
	# visible after pressing SETTINGS. Keep a viewport-friendly window and let the
	# scroll container handle any content that does not fit vertically.
	scroll.custom_minimum_size = Vector2(620.0, 560.0)
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	_settings_center.add_child(scroll)

	var panel := PanelContainer.new()
	panel.custom_minimum_size = Vector2(590.0, 0.0)
	panel.add_theme_stylebox_override("panel", _panel_style())
	scroll.add_child(panel)

	var margin := MarginContainer.new()
	margin.add_theme_constant_override("margin_left", 24)
	margin.add_theme_constant_override("margin_right", 24)
	margin.add_theme_constant_override("margin_top", 20)
	margin.add_theme_constant_override("margin_bottom", 20)
	panel.add_child(margin)

	var column := VBoxContainer.new()
	column.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	column.add_theme_constant_override("separation", 12)
	margin.add_child(column)

	var title := Label.new()
	title.text = "SETTINGS"
	title.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	title.add_theme_font_size_override("font_size", 32)
	column.add_child(title)

	var subtitle := Label.new()
	subtitle.text = "Changes are saved to user://settings.cfg"
	subtitle.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	subtitle.modulate = Color(0.62, 0.69, 0.75)
	subtitle.add_theme_font_size_override("font_size", 13)
	column.add_child(subtitle)

	column.add_child(HSeparator.new())
	column.add_child(_section_title("GRAPHICS"))

	var graphics_row := HBoxContainer.new()
	graphics_row.add_theme_constant_override("separation", 16)
	column.add_child(graphics_row)

	var graphics_text := VBoxContainer.new()
	graphics_text.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	graphics_text.add_theme_constant_override("separation", 3)
	graphics_row.add_child(graphics_text)

	var graphics_label := Label.new()
	graphics_label.text = "Rendering preset"
	graphics_label.add_theme_font_size_override("font_size", 17)
	graphics_text.add_child(graphics_label)

	var graphics_hint := Label.new()
	graphics_hint.text = GraphicsQuality.preset_description(AppSettings.graphics_quality)
	graphics_hint.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	graphics_hint.modulate = Color(0.62, 0.69, 0.75)
	graphics_hint.add_theme_font_size_override("font_size", 13)
	graphics_text.add_child(graphics_hint)

	var graphics_preset := OptionButton.new()
	graphics_preset.custom_minimum_size = Vector2(150.0, 42.0)
	for preset: int in range(GraphicsQuality.Preset.PERFORMANCE, GraphicsQuality.Preset.ULTRA + 1):
		graphics_preset.add_item(GraphicsQuality.preset_name(preset), preset)
	graphics_preset.select(AppSettings.graphics_quality)
	graphics_row.add_child(graphics_preset)

	var live_note := Label.new()
	live_note.text = "Applied live to render scale, GI, screen-space effects, shadows and cloud quality."
	live_note.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	live_note.modulate = Color(0.48, 0.56, 0.63)
	live_note.add_theme_font_size_override("font_size", 12)
	column.add_child(live_note)

	var advanced_row := HBoxContainer.new()
	advanced_row.add_theme_constant_override("separation", 16)
	column.add_child(advanced_row)
	var advanced_text := VBoxContainer.new()
	advanced_text.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	advanced_row.add_child(advanced_text)
	var advanced_label := Label.new()
	advanced_label.text = "Advanced overrides"
	advanced_label.add_theme_font_size_override("font_size", 17)
	advanced_text.add_child(advanced_label)
	var advanced_hint := Label.new()
	advanced_hint.text = "Manual controls for individual GPU costs. Your custom values are kept when you return to a preset."
	advanced_hint.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	advanced_hint.modulate = Color(0.62, 0.69, 0.75)
	advanced_hint.add_theme_font_size_override("font_size", 13)
	advanced_text.add_child(advanced_hint)
	var advanced_toggle := CheckButton.new()
	advanced_toggle.button_pressed = AppSettings.graphics_advanced_enabled
	advanced_row.add_child(advanced_toggle)

	var advanced_controls := VBoxContainer.new()
	advanced_controls.visible = AppSettings.graphics_advanced_enabled
	advanced_controls.add_theme_constant_override("separation", 10)
	column.add_child(advanced_controls)

	_add_advanced_slider(
		advanced_controls, "3D render scale",
		"Internal 3D resolution. UI remains full resolution.",
		0.50, 1.00, 0.05, AppSettings.advanced_render_scale,
		AppSettings.KEY_RENDER_SCALE, 100.0, 0, "%")
	_add_advanced_toggle(
		advanced_controls, "SDFGI",
		"Dynamic global illumination. The largest cost while moving over terrain.",
		AppSettings.advanced_sdfgi_enabled, AppSettings.KEY_SDFGI_ENABLED)
	_add_advanced_slider(
		advanced_controls, "SDFGI cascades",
		"More cascades extend GI farther but increase movement update cost.",
		2.0, 8.0, 1.0, float(AppSettings.advanced_sdfgi_cascades),
		AppSettings.KEY_SDFGI_CASCADES, 1.0, 0, "")
	_add_advanced_slider(
		advanced_controls, "SDFGI minimum cell",
		"Smaller cells add detail and make cascade movement more expensive.",
		0.25, 2.00, 0.05, AppSettings.advanced_sdfgi_cell_size,
		AppSettings.KEY_SDFGI_CELL_SIZE, 1.0, 2, " m")
	_add_advanced_toggle(
		advanced_controls, "SSAO", "Screen-space ambient contact shadows.",
		AppSettings.advanced_ssao_enabled, AppSettings.KEY_SSAO_ENABLED)
	_add_advanced_toggle(
		advanced_controls, "SSIL", "Screen-space indirect lighting.",
		AppSettings.advanced_ssil_enabled, AppSettings.KEY_SSIL_ENABLED)
	_add_advanced_toggle(
		advanced_controls, "SSR", "Screen-space reflections.",
		AppSettings.advanced_ssr_enabled, AppSettings.KEY_SSR_ENABLED)
	_add_advanced_toggle(
		advanced_controls, "Glow", "Bright-light bloom post-processing.",
		AppSettings.advanced_glow_enabled, AppSettings.KEY_GLOW_ENABLED)

	var shadow_row := _advanced_option_row(
		advanced_controls, "Directional shadow splits",
		"More splits sharpen sun shadows across a larger range.")
	var shadow_option := shadow_row["option"] as OptionButton
	for splits: int in [0, 1, 2, 4]:
		shadow_option.add_item("Off" if splits == 0 else str(splits), splits)
	_select_option_id(shadow_option, AppSettings.advanced_shadow_splits)
	shadow_option.item_selected.connect(func(index: int) -> void:
		AppSettings.set_advanced_graphics_value(
			AppSettings.KEY_SHADOW_SPLITS, shadow_option.get_item_id(index))
		_apply_graphics_quality(AppSettings.graphics_quality)
	)

	var cloud_row := _advanced_option_row(
		advanced_controls, "Cloud quality",
		"Controls volumetric cloud ray-march sample counts.")
	var cloud_option := cloud_row["option"] as OptionButton
	for preset: int in range(GraphicsQuality.Preset.PERFORMANCE, GraphicsQuality.Preset.ULTRA + 1):
		cloud_option.add_item(GraphicsQuality.preset_name(preset), preset)
	_select_option_id(cloud_option, AppSettings.advanced_cloud_quality)
	cloud_option.item_selected.connect(func(index: int) -> void:
		AppSettings.set_advanced_graphics_value(
			AppSettings.KEY_CLOUD_QUALITY, cloud_option.get_item_id(index))
		_apply_graphics_quality(AppSettings.graphics_quality)
	)

	advanced_toggle.toggled.connect(func(value: bool) -> void:
		AppSettings.set_graphics_advanced_enabled(value)
		advanced_controls.visible = value
		graphics_hint.text = (
			"Custom per-feature overrides are active."
			if value else GraphicsQuality.preset_description(AppSettings.graphics_quality))
		_apply_graphics_quality(AppSettings.graphics_quality)
	)
	graphics_preset.item_selected.connect(func(index: int) -> void:
		var preset: int = graphics_preset.get_item_id(index)
		AppSettings.set_graphics_quality(preset)
		advanced_toggle.set_pressed_no_signal(false)
		advanced_controls.visible = false
		graphics_hint.text = GraphicsQuality.preset_description(preset)
		_apply_graphics_quality(preset)
	)
	if AppSettings.graphics_advanced_enabled:
		graphics_hint.text = "Custom per-feature overrides are active."

	column.add_child(HSeparator.new())
	column.add_child(_section_title("RENDER DIAGNOSTICS"))

	var debug_view_row := HBoxContainer.new()
	debug_view_row.add_theme_constant_override("separation", 16)
	column.add_child(debug_view_row)
	var debug_view_text := VBoxContainer.new()
	debug_view_text.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	debug_view_text.add_theme_constant_override("separation", 3)
	debug_view_row.add_child(debug_view_text)
	var debug_view_label := Label.new()
	debug_view_label.text = "Graphics debug view"
	debug_view_label.add_theme_font_size_override("font_size", 17)
	debug_view_text.add_child(debug_view_label)
	var debug_view_hint := Label.new()
	debug_view_hint.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	debug_view_hint.modulate = Color(0.62, 0.69, 0.75)
	debug_view_hint.add_theme_font_size_override("font_size", 13)
	debug_view_text.add_child(debug_view_hint)
	var debug_view_option := OptionButton.new()
	debug_view_option.custom_minimum_size = Vector2(210.0, 42.0)
	var debug_view_descriptions: Dictionary = {}
	for view: Dictionary in GRAPHICS_DEBUG_VIEWS:
		var mode := int(view["mode"])
		debug_view_option.add_item(str(view["name"]), mode)
		debug_view_descriptions[mode] = str(view["description"])
	_select_option_id(debug_view_option, int(get_viewport().debug_draw))
	debug_view_hint.text = str(debug_view_descriptions.get(
		int(get_viewport().debug_draw), "Select a render buffer to inspect."))
	debug_view_option.item_selected.connect(func(index: int) -> void:
		var mode := debug_view_option.get_item_id(index)
		get_viewport().debug_draw = mode
		debug_view_hint.text = str(debug_view_descriptions.get(mode, ""))
	)
	debug_view_row.add_child(debug_view_option)

	var debug_view_note := Label.new()
	debug_view_note.text = "Choose a view, then Back → Resume to inspect it. Return to Final composite when finished."
	debug_view_note.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	debug_view_note.modulate = Color(0.48, 0.56, 0.63)
	debug_view_note.add_theme_font_size_override("font_size", 12)
	column.add_child(debug_view_note)

	var waves_row := HBoxContainer.new()
	waves_row.add_theme_constant_override("separation", 16)
	column.add_child(waves_row)
	var waves_text := VBoxContainer.new()
	waves_text.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	waves_row.add_child(waves_text)
	var waves_label := Label.new()
	waves_label.text = "Disable ocean waves"
	waves_label.add_theme_font_size_override("font_size", 17)
	waves_text.add_child(waves_label)
	var waves_hint := Label.new()
	waves_hint.text = "Flattens the rendered ocean and GPU buoyancy wave field for this session."
	waves_hint.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	waves_hint.modulate = Color(0.62, 0.69, 0.75)
	waves_hint.add_theme_font_size_override("font_size", 13)
	waves_text.add_child(waves_hint)
	var waves_toggle := CheckButton.new()
	waves_toggle.button_pressed = OceanSystem.debug_waves_disabled()
	waves_toggle.toggled.connect(func(disabled: bool) -> void:
		OceanSystem.set_debug_waves_disabled(disabled)
	)
	waves_row.add_child(waves_toggle)

	column.add_child(HSeparator.new())
	column.add_child(_section_title("DEBUG"))

	var distance_row := HBoxContainer.new()
	distance_row.add_theme_constant_override("separation", 16)
	column.add_child(distance_row)
	var distance_text := VBoxContainer.new()
	distance_text.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	distance_row.add_child(distance_text)
	var distance_label := Label.new()
	distance_label.text = "Distance from closest character"
	distance_label.add_theme_font_size_override("font_size", 17)
	distance_text.add_child(distance_label)
	var distance_hint := Label.new()
	distance_hint.text = "Shows the live camera distance to the nearest registered character."
	distance_hint.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	distance_hint.modulate = Color(0.62, 0.69, 0.75)
	distance_hint.add_theme_font_size_override("font_size", 13)
	distance_text.add_child(distance_hint)
	var distance_toggle := CheckButton.new()
	distance_toggle.button_pressed = AppSettings.debug_closest_character_distance
	distance_toggle.toggled.connect(func(value: bool) -> void:
		AppSettings.set_debug_closest_character_distance(value)
	)
	distance_row.add_child(distance_toggle)

	column.add_child(HSeparator.new())

	var brow_row := HBoxContainer.new()
	brow_row.add_theme_constant_override("separation", 16)
	column.add_child(brow_row)
	var brow_text := VBoxContainer.new()
	brow_text.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	brow_row.add_child(brow_text)
	var brow_label := Label.new()
	brow_label.text = "Brow LOD debug"
	brow_label.add_theme_font_size_override("font_size", 17)
	brow_text.add_child(brow_label)
	var brow_hint := Label.new()
	brow_hint.text = "Enable manual brow LOD testing and force the active level."
	brow_hint.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	brow_hint.modulate = Color(0.62, 0.69, 0.75)
	brow_hint.add_theme_font_size_override("font_size", 13)
	brow_text.add_child(brow_hint)
	var brow_toggle := CheckButton.new()
	brow_toggle.button_pressed = AppSettings.debug_brow_lod_controls
	brow_row.add_child(brow_toggle)

	var lod_controls := VBoxContainer.new()
	lod_controls.visible = AppSettings.debug_brow_lod_controls
	lod_controls.add_theme_constant_override("separation", 7)
	column.add_child(lod_controls)

	var lod_header := HBoxContainer.new()
	lod_controls.add_child(lod_header)
	var lod_label := Label.new()
	lod_label.text = "Force brow LOD"
	lod_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	lod_label.add_theme_font_size_override("font_size", 16)
	lod_header.add_child(lod_label)
	var lod_value := Label.new()
	lod_value.custom_minimum_size.x = 90.0
	lod_value.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	lod_value.text = _brow_lod_label(AppSettings.debug_forced_brow_lod)
	lod_header.add_child(lod_value)

	var lod_slider := HSlider.new()
	lod_slider.min_value = -1.0
	lod_slider.max_value = 2.0
	lod_slider.step = 1.0
	lod_slider.value = float(AppSettings.debug_forced_brow_lod)
	lod_slider.value_changed.connect(func(value: float) -> void:
		var lod: int = int(round(value))
		lod_value.text = _brow_lod_label(lod)
		AppSettings.set_debug_forced_brow_lod(lod)
	)
	lod_controls.add_child(lod_slider)

	brow_toggle.toggled.connect(func(value: bool) -> void:
		AppSettings.set_debug_brow_lod_controls(value)
		lod_controls.visible = value
		if not value:
			lod_slider.set_value_no_signal(-1.0)
			lod_value.text = "AUTO"
	)

	column.add_child(HSeparator.new())
	var back := Button.new()
	back.text = "Back"
	back.custom_minimum_size.y = 44.0
	back.pressed.connect(_show_pause_menu)
	column.add_child(back)


func _apply_graphics_quality(_preset: int) -> void:
	AppSettings.apply_viewport(get_viewport())
	var parent := get_parent()
	if parent != null:
		for child: Node in parent.get_children():
			if child is WorldEnvironment:
				var world_environment := child as WorldEnvironment
				if world_environment.environment != null:
					AppSettings.apply_world_environment(world_environment.environment)
			elif child is DirectionalLight3D:
				var light := child as DirectionalLight3D
				AppSettings.apply_sun(light)
				# Preserve Helion's physical apparent disc after the quality helper
				# changes the generic soft-shadow preset.
				light.light_angular_distance = Frames.helion_angular_diameter_deg()

	# Update only cloud sampling budgets; this preserves whichever cloud renderer
	# (depth compositor or fallback sky) already owns the visible cloud pass.
	VolumetricClouds.set_quality(AppSettings.effective_cloud_quality())


func _add_advanced_toggle(parent: VBoxContainer, title: String, hint: String,
		pressed: bool, key: String) -> CheckButton:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 16)
	parent.add_child(row)
	var text_column := VBoxContainer.new()
	text_column.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(text_column)
	var label := Label.new()
	label.text = title
	label.add_theme_font_size_override("font_size", 15)
	text_column.add_child(label)
	var description := Label.new()
	description.text = hint
	description.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	description.modulate = Color(0.55, 0.63, 0.70)
	description.add_theme_font_size_override("font_size", 12)
	text_column.add_child(description)
	var toggle := CheckButton.new()
	toggle.button_pressed = pressed
	toggle.toggled.connect(func(value: bool) -> void:
		AppSettings.set_advanced_graphics_value(key, value)
		_apply_graphics_quality(AppSettings.graphics_quality)
	)
	row.add_child(toggle)
	return toggle


func _add_advanced_slider(parent: VBoxContainer, title: String, hint: String,
		minimum: float, maximum: float, increment: float, current: float,
		key: String, display_scale: float, decimals: int, suffix: String) -> HSlider:
	var block := VBoxContainer.new()
	block.add_theme_constant_override("separation", 3)
	parent.add_child(block)
	var header := HBoxContainer.new()
	block.add_child(header)
	var title_label := Label.new()
	title_label.text = title
	title_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	title_label.add_theme_font_size_override("font_size", 15)
	header.add_child(title_label)
	var value_label := Label.new()
	value_label.custom_minimum_size.x = 80.0
	value_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	value_label.text = _advanced_value_text(current, display_scale, decimals, suffix)
	header.add_child(value_label)
	var description := Label.new()
	description.text = hint
	description.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	description.modulate = Color(0.55, 0.63, 0.70)
	description.add_theme_font_size_override("font_size", 12)
	block.add_child(description)
	var slider := HSlider.new()
	slider.min_value = minimum
	slider.max_value = maximum
	slider.step = increment
	slider.value = current
	slider.value_changed.connect(func(value: float) -> void:
		value_label.text = _advanced_value_text(value, display_scale, decimals, suffix)
		AppSettings.set_advanced_graphics_value(key, value)
		_apply_graphics_quality(AppSettings.graphics_quality)
	)
	block.add_child(slider)
	return slider


func _advanced_option_row(parent: VBoxContainer, title: String, hint: String) -> Dictionary:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 16)
	parent.add_child(row)
	var text_column := VBoxContainer.new()
	text_column.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(text_column)
	var label := Label.new()
	label.text = title
	label.add_theme_font_size_override("font_size", 15)
	text_column.add_child(label)
	var description := Label.new()
	description.text = hint
	description.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	description.modulate = Color(0.55, 0.63, 0.70)
	description.add_theme_font_size_override("font_size", 12)
	text_column.add_child(description)
	var option := OptionButton.new()
	option.custom_minimum_size = Vector2(130.0, 38.0)
	row.add_child(option)
	return {"row": row, "option": option}


func _select_option_id(option: OptionButton, id: int) -> void:
	for index: int in option.item_count:
		if option.get_item_id(index) == id:
			option.select(index)
			return


func _advanced_value_text(value: float, display_scale: float,
		decimals: int, suffix: String) -> String:
	var scaled := value * display_scale
	return ("%.*f%s" % [decimals, scaled, suffix])


func _find_player() -> AsterraPlayer:
	var parent := get_parent()
	if parent == null:
		return null
	for child: Node in parent.get_children():
		if child is AsterraPlayer:
			return child as AsterraPlayer
	return null


func _find_debug_menu() -> DebugMenu:
	var parent := get_parent()
	if parent == null:
		return null
	for child: Node in parent.get_children():
		if child is DebugMenu:
			return child as DebugMenu
	return null


func _show_pause_menu() -> void:
	if _pause_center != null:
		_pause_center.visible = true
	if _settings_center != null:
		_settings_center.visible = false


func _show_settings_menu() -> void:
	if _pause_center != null:
		_pause_center.visible = false
	if _settings_center != null:
		_settings_center.visible = true


func _return_to_main_menu() -> void:
	get_tree().paused = false
	visible = false
	get_viewport().debug_draw = Viewport.DEBUG_DRAW_DISABLED
	OceanSystem.set_debug_waves_disabled(false)
	get_tree().change_scene_to_file(START_MENU_SCENE)


func _quit() -> void:
	get_tree().paused = false
	get_tree().quit()


func _add_button(parent: VBoxContainer, text: String, callback: Callable) -> void:
	var button := Button.new()
	button.text = text
	button.custom_minimum_size.y = 48.0
	button.add_theme_font_size_override("font_size", 18)
	button.pressed.connect(callback)
	parent.add_child(button)


func _section_title(text: String) -> Label:
	var label := Label.new()
	label.text = text
	label.modulate = Color(0.58, 0.68, 0.77)
	label.add_theme_font_size_override("font_size", 13)
	return label


func _panel_style() -> StyleBoxFlat:
	var style := StyleBoxFlat.new()
	style.bg_color = Color(0.055, 0.078, 0.105, 0.985)
	style.border_color = Color(0.18, 0.26, 0.33)
	style.set_border_width_all(1)
	style.set_corner_radius_all(10)
	return style


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
