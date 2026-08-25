class_name DebugMenu
extends CanvasLayer
## Runtime debug menu for terrain, procedural sky and world-generation tools.
##
## The menu is intentionally bound to '&' rather than Escape. Escape remains free
## for the normal game/pause flow and never opens or closes this debug interface.

signal opened
signal closed
signal rebake_requested
signal coast_profile_requested

## [property, label, note, default]
const TERRAIN_ROWS := [
	["gpu_scatter", "GPU terrain scatter", "Toggles deterministic grass + geological/river stone candidate layers without changing terrain generation", true],
	["microrelief", "Near-field geometric microrelief", "Toggles actual dense 1/4-L0 vertex displacement; the dense handoff patch itself remains active", true],
	["pbr_detail", "Scanned PBR detail", "Toggles close-range scanned albedo, roughness and normal detail without changing geomorph or material weights", true],
	["wireframe", "Wireframe", "Shows the actual concentric clipmap triangles", false],
	["ring_indicator", "LOD ring labels", "Shows L0, L1, L2... directly on each active concentric ring", false],
	["freeze_terrain", "Freeze terrain clipmap", "Locks centre + active LODs in planet space while you fly around", false],
	["side_cut", "Side cut / sinking view", "Cuts away half the terrain and colours each LOD so overlap is visible", false],
]

const GEOMORPH_DEBUG_MODES := [
	"Final material",
	"Landforms — R mountain / G arid / B glacial / white deposition",
	"Primary materials — rock / soil / vegetation / sand",
	"Secondary materials — mud / snow / scree / gravel",
	"Soil context — sand / silt / clay, organic darkening",
	"Geology / biome — rock ID / biome ID / erodibility",
	"Hydrology — flow direction / discharge / deposition",
]

const DEFAULT_SINK_SCALE := 2.0
const SKY_DEFAULT := 1.0

var debug: TerrainDebug
var procedural_sky: ProceduralSky
var sky_material: ShaderMaterial
var sky_resource: Sky

var _panel: PanelContainer
var _buttons := {}
var _rebake_button: Button
var _sink_label: Label
var _sink_slider: HSlider
var _geomorph_selector: OptionButton
var _sky_labels := {}
var _sky_sliders := {}
var _sky_target_label: Label


func _ready() -> void:
	layer = 20
	visible = false
	process_mode = Node.PROCESS_MODE_ALWAYS

	var viewport_h: float = get_viewport().get_visible_rect().size.y
	var panel_h: float = clampf(viewport_h - 80.0, 420.0, 720.0)

	_panel = PanelContainer.new()
	_panel.set_anchors_preset(Control.PRESET_CENTER_LEFT)
	_panel.position = Vector2(36, -panel_h * 0.5)
	_panel.custom_minimum_size = Vector2(550, panel_h)
	var style := StyleBoxFlat.new()
	style.bg_color = Color(0.05, 0.06, 0.09, 0.94)
	style.border_color = Color(0.35, 0.45, 0.60, 0.9)
	style.set_border_width_all(1)
	style.set_corner_radius_all(4)
	style.set_content_margin_all(14)
	_panel.add_theme_stylebox_override("panel", style)
	add_child(_panel)

	var root_box := VBoxContainer.new()
	root_box.add_theme_constant_override("separation", 7)
	_panel.add_child(root_box)

	var title := Label.new()
	title.text = "ASTERRA DEBUG"
	title.add_theme_font_size_override("font_size", 15)
	title.add_theme_color_override("font_color", Color(0.95, 0.97, 1.0))
	root_box.add_child(title)

	var tabs := TabContainer.new()
	tabs.custom_minimum_size = Vector2(520, maxf(340.0, panel_h - 80.0))
	tabs.size_flags_vertical = Control.SIZE_EXPAND_FILL
	root_box.add_child(tabs)

	_build_terrain_tab(tabs)
	_build_sky_tab(tabs)
	_build_world_tab(tabs)

	var hint := Label.new()
	hint.text = "& toggles debug  •  Esc is not bound to this menu"
	hint.add_theme_font_size_override("font_size", 11)
	hint.add_theme_color_override("font_color", Color(0.55, 0.62, 0.72))
	root_box.add_child(hint)


func _build_terrain_tab(tabs: TabContainer) -> void:
	var scroll := ScrollContainer.new()
	scroll.name = "Terrain"
	scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	scroll.vertical_scroll_mode = ScrollContainer.SCROLL_MODE_AUTO
	scroll.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	tabs.add_child(scroll)

	var box := VBoxContainer.new()
	box.name = "TerrainControls"
	box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.add_theme_constant_override("separation", 6)
	scroll.add_child(box)

	box.add_child(_section_title("Concentric GPU clipmap"))
	box.add_child(_note("Current renderer: coarse baked planet context → GPU geomorph → GPU materials → dense microgeometry → GPU scatter"))

	_geomorph_selector = OptionButton.new()
	_geomorph_selector.custom_minimum_size = Vector2(0, 34)
	_geomorph_selector.add_theme_font_size_override("font_size", 13)
	for mode_name in GEOMORPH_DEBUG_MODES:
		_geomorph_selector.add_item(mode_name)
	_geomorph_selector.select(0)
	_geomorph_selector.item_selected.connect(_on_geomorph_mode_selected)
	box.add_child(_geomorph_selector)
	box.add_child(_note("Diagnostic views are emissive/unlit so field values remain readable at night and at the terminator."))

	for row in TERRAIN_ROWS:
		var key: String = row[0]
		var button := CheckButton.new()
		button.text = row[1]
		button.button_pressed = row[3]
		button.custom_minimum_size = Vector2(0, 34)
		button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		button.add_theme_font_size_override("font_size", 13)
		button.toggled.connect(_on_toggled.bind(key))
		box.add_child(button)
		_buttons[key] = button
		box.add_child(_note(row[2]))

	_sink_label = Label.new()
	_sink_label.text = "Sink depth: %.2f × LOD spacing" % DEFAULT_SINK_SCALE
	_sink_label.custom_minimum_size = Vector2(0, 26)
	_sink_label.add_theme_font_size_override("font_size", 13)
	box.add_child(_sink_label)
	box.add_child(_note("Default is 2×. Increase only while inspecting overlap."))

	_sink_slider = HSlider.new()
	_sink_slider.min_value = 0.0
	_sink_slider.max_value = 12.0
	_sink_slider.step = 0.25
	_sink_slider.value = DEFAULT_SINK_SCALE
	_sink_slider.custom_minimum_size = Vector2(0, 28)
	_sink_slider.value_changed.connect(_on_sink_scale_changed)
	box.add_child(_sink_slider)

	var reset := Button.new()
	reset.text = "Reset terrain inspection"
	reset.custom_minimum_size = Vector2(0, 34)
	reset.add_theme_font_size_override("font_size", 13)
	reset.pressed.connect(_on_reset_inspection)
	box.add_child(reset)
	box.add_child(_note("Restores final materials + scatter + microrelief + PBR, clears ring labels, unfreezes, closes the cut and restores the normal 2× sink depth"))


func _build_sky_tab(tabs: TabContainer) -> void:
	var scroll := ScrollContainer.new()
	scroll.name = "Sky"
	scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	scroll.vertical_scroll_mode = ScrollContainer.SCROLL_MODE_AUTO
	scroll.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	tabs.add_child(scroll)

	var box := VBoxContainer.new()
	box.name = "SkyControls"
	box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.add_theme_constant_override("separation", 4)
	scroll.add_child(box)

	box.add_child(_section_title("Instanced stars"))
	box.add_child(_note("1.00 values are the physical defaults. Controls are manual diagnostics only."))
	box.add_child(_note("Set stellar radiance to 0.00 to verify the live material binding immediately."))
	_sky_target_label = _note("Live sky target: unresolved")
	box.add_child(_sky_target_label)

	_add_sky_slider(box, "radiance", "Stellar radiance calibration", "Global fixed stellar photometric calibration; never driven by sun altitude.", 0.0, 2.0, 0.05, SKY_DEFAULT)
	_add_sky_slider(box, "magnitude", "Magnitude flux response", "1.00 = physical Pogson law: 5 magnitudes = 100× flux.", 0.5, 1.5, 0.05, SKY_DEFAULT)
	_add_sky_slider(box, "seeing", "Atmospheric seeing / scintillation", "Scales stochastic turbulence only; geometry still makes the horizon stronger.", 0.0, 2.0, 0.05, SKY_DEFAULT)
	_add_sky_slider(box, "colour", "Naked-eye stellar colour response", "0.00 = luminance-preserving monochrome; 1.00 = temperature-derived colour.", 0.0, 1.0, 0.05, SKY_DEFAULT)
	_add_sky_slider(box, "chromatic", "Chromatic scintillation", "Low-altitude red/blue turbulence flashes; luminance remains preserved.", 0.0, 2.0, 0.05, SKY_DEFAULT)

	box.add_child(HSeparator.new())
	box.add_child(_section_title("Diffuse deep sky / sky shader"))
	_add_sky_slider(box, "deep_sky", "Deep-sky radiance", "Master multiplier for the fixed galaxy + nebula radiance. 0.00 disables the diffuse deep sky.", 0.0, 2.0, 0.05, SKY_DEFAULT)
	_add_sky_slider(box, "galaxy", "Galaxy radiance", "Debug multiplier for the procedural galactic band only.", 0.0, 2.0, 0.05, SKY_DEFAULT)
	_add_sky_slider(box, "nebula", "Nebula radiance", "Debug multiplier for the sparse procedural nebula complexes only.", 0.0, 2.0, 0.05, SKY_DEFAULT)

	var reset := Button.new()
	reset.text = "Reset sky calibration to physical defaults"
	reset.add_theme_font_size_override("font_size", 13)
	reset.pressed.connect(_on_reset_sky)
	box.add_child(reset)
	box.add_child(_note("Catalogue density remains magnitude-limited; lower graphics tiers remove the faintest stars first."))


func _build_world_tab(tabs: TabContainer) -> void:
	var box := VBoxContainer.new()
	box.name = "World"
	box.add_theme_constant_override("separation", 5)
	tabs.add_child(box)

	box.add_child(_section_title("World generation"))

	var coast_profile := Button.new()
	coast_profile.text = "Edit coastline terrain profile..."
	coast_profile.add_theme_font_size_override("font_size", 13)
	coast_profile.pressed.connect(func(): coast_profile_requested.emit())
	box.add_child(coast_profile)
	box.add_child(_note("Edits the global sea-side terrain profile"))

	_rebake_button = Button.new()
	_rebake_button.text = "Rebake planet (ignore cache)"
	_rebake_button.add_theme_font_size_override("font_size", 13)
	_rebake_button.pressed.connect(_on_rebake_pressed)
	box.add_child(_rebake_button)
	box.add_child(_note("Regenerates the macro planet fields from the current seed"))


func _add_sky_slider(
	parent: VBoxContainer,
	key: String,
	title: String,
	note_text: String,
	minimum: float,
	maximum: float,
	step_value: float,
	default_value: float
) -> void:
	var label := Label.new()
	label.add_theme_font_size_override("font_size", 13)
	parent.add_child(label)
	_sky_labels[key] = label

	var slider := HSlider.new()
	slider.min_value = minimum
	slider.max_value = maximum
	slider.step = step_value
	slider.value = default_value
	slider.custom_minimum_size = Vector2(0, 20)
	slider.value_changed.connect(_on_sky_slider_changed.bind(key))
	parent.add_child(slider)
	_sky_sliders[key] = slider
	parent.add_child(_note(note_text))
	_update_sky_label(key, default_value, title)


func _input(event: InputEvent) -> void:
	if not (event is InputEventKey):
		return
	var key_event := event as InputEventKey
	if not key_event.pressed or key_event.echo:
		return
	if key_event.unicode == 38:
		toggle()
		get_viewport().set_input_as_handled()


func toggle() -> void:
	visible = not visible
	if visible:
		_resolve_sky_targets()
		opened.emit()
	else:
		closed.emit()


func _section_title(text: String) -> Label:
	var label := Label.new()
	label.text = text
	label.add_theme_font_size_override("font_size", 14)
	label.add_theme_color_override("font_color", Color(0.86, 0.91, 1.0))
	return label


func _note(text: String) -> Label:
	var note := Label.new()
	note.text = "     %s" % text
	note.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	note.add_theme_font_size_override("font_size", 11)
	note.add_theme_color_override("font_color", Color(0.58, 0.65, 0.76))
	return note


func _on_toggled(pressed: bool, key: String) -> void:
	if key == "gpu_scatter":
		if TerrainScatter.has_method("set_debug_enabled"):
			TerrainScatter.call("set_debug_enabled", pressed)
		return
	if key == "ring_indicator":
		if GroundGeometryClipmap.has_method("set_debug_ring_indicator"):
			GroundGeometryClipmap.call("set_debug_ring_indicator", pressed)
		return
	if debug != null:
		debug.set(key, pressed)


func _on_geomorph_mode_selected(index: int) -> void:
	if debug != null:
		debug.geomorph_mode = index
	elif GroundGeometryClipmap.has_method("set_debug_geomorph_mode"):
		GroundGeometryClipmap.call("set_debug_geomorph_mode", index)


func _on_sink_scale_changed(value: float) -> void:
	if _sink_label != null:
		_sink_label.text = "Sink depth: %.2f × LOD spacing" % value
	if debug != null:
		debug.sink_scale = value


func _on_reset_inspection() -> void:
	for key_value: Variant in ["ring_indicator", "freeze_terrain", "side_cut"]:
		var key := String(key_value)
		var button_value: Variant = _buttons.get(key)
		if button_value is CheckButton:
			(button_value as CheckButton).set_pressed_no_signal(false)
	for key_value: Variant in ["gpu_scatter", "microrelief", "pbr_detail"]:
		var key := String(key_value)
		var button_value: Variant = _buttons.get(key)
		if button_value is CheckButton:
			(button_value as CheckButton).set_pressed_no_signal(true)
	if TerrainScatter.has_method("set_debug_enabled"):
		TerrainScatter.call("set_debug_enabled", true)
	if GroundGeometryClipmap.has_method("set_debug_ring_indicator"):
		GroundGeometryClipmap.call("set_debug_ring_indicator", false)
	if _geomorph_selector != null:
		_geomorph_selector.select(0)
	if _sink_slider != null:
		_sink_slider.set_value_no_signal(DEFAULT_SINK_SCALE)
	if _sink_label != null:
		_sink_label.text = "Sink depth: %.2f × LOD spacing" % DEFAULT_SINK_SCALE
	if debug != null:
		debug.reset_inspection()
	else:
		if GroundGeometryClipmap.has_method("set_debug_geomorph_mode"):
			GroundGeometryClipmap.call("set_debug_geomorph_mode", 0)
		if GroundGeometryClipmap.has_method("set_debug_microrelief_enabled"):
			GroundGeometryClipmap.call("set_debug_microrelief_enabled", true)
		if GroundGeometryClipmap.has_method("set_debug_pbr_enabled"):
			GroundGeometryClipmap.call("set_debug_pbr_enabled", true)


func _on_sky_slider_changed(value: float, key: String) -> void:
	_update_sky_label(key, value)
	_resolve_sky_targets()

	match key:
		"radiance":
			if procedural_sky != null:
				procedural_sky.set_star_radiance_scale(value)
		"magnitude":
			if procedural_sky != null:
				procedural_sky.set_magnitude_flux_exponent(value)
		"seeing":
			if procedural_sky != null:
				procedural_sky.set_seeing_strength(value)
		"colour":
			if procedural_sky != null:
				procedural_sky.set_colour_strength(value)
		"chromatic":
			if procedural_sky != null:
				procedural_sky.set_chromatic_scintillation_strength(value)
		"deep_sky":
			if sky_material != null:
				sky_material.set_shader_parameter("u_deep_sky_scale", value)
		"galaxy":
			if sky_material != null:
				sky_material.set_shader_parameter("u_galaxy_scale", value)
		"nebula":
			if sky_material != null:
				sky_material.set_shader_parameter("u_nebula_scale", value)


func _update_sky_label(key: String, value: float, explicit_title: String = "") -> void:
	var label_value: Variant = _sky_labels.get(key)
	if not (label_value is Label):
		return

	var title := explicit_title
	if title.is_empty():
		match key:
			"radiance":
				title = "Stellar radiance calibration"
			"magnitude":
				title = "Magnitude flux response"
			"seeing":
				title = "Atmospheric seeing / scintillation"
			"colour":
				title = "Naked-eye stellar colour response"
			"chromatic":
				title = "Chromatic scintillation"
			"deep_sky":
				title = "Deep-sky radiance"
			"galaxy":
				title = "Galaxy radiance"
			"nebula":
				title = "Nebula radiance"
	(label_value as Label).text = "%s: %.2f ×" % [title, value]


func _on_reset_sky() -> void:
	_resolve_sky_targets()
	if procedural_sky != null:
		procedural_sky.reset_debug_calibration()
	if sky_material != null:
		sky_material.set_shader_parameter("u_deep_sky_scale", 1.0)
		sky_material.set_shader_parameter("u_galaxy_scale", 1.0)
		sky_material.set_shader_parameter("u_nebula_scale", 1.0)

	for key_value: Variant in ["radiance", "magnitude", "seeing", "colour", "chromatic", "deep_sky", "galaxy", "nebula"]:
		var key := String(key_value)
		var slider_value: Variant = _sky_sliders.get(key)
		if slider_value is HSlider:
			(slider_value as HSlider).set_value_no_signal(SKY_DEFAULT)
		_update_sky_label(key, SKY_DEFAULT)


func _resolve_sky_targets() -> void:
	var root := get_parent()
	if root == null:
		return

	if procedural_sky == null or not is_instance_valid(procedural_sky):
		var candidate := root.get_node_or_null("ProceduralSky")
		if candidate is ProceduralSky:
			procedural_sky = candidate as ProceduralSky

	sky_resource = null
	sky_material = null
	for child in root.get_children():
		if not (child is WorldEnvironment):
			continue
		var world_environment := child as WorldEnvironment
		var environment := world_environment.environment
		if environment == null or environment.sky == null:
			continue
		sky_resource = environment.sky
		sky_resource.radiance_size = Sky.RADIANCE_SIZE_256
		sky_resource.process_mode = Sky.PROCESS_MODE_REALTIME
		var live_material := sky_resource.sky_material
		if live_material is ShaderMaterial:
			sky_material = live_material as ShaderMaterial
			break

	if sky_material == null:
		var material_value: Variant = root.get("sky_mat")
		if material_value is ShaderMaterial:
			sky_material = material_value as ShaderMaterial

	if _sky_target_label != null:
		if sky_material != null and sky_resource != null:
			_sky_target_label.text = "     Live sky target: WorldEnvironment / REALTIME"
		elif sky_material != null:
			_sky_target_label.text = "     Live sky target: shader material fallback"
		else:
			_sky_target_label.text = "     Live sky target: NOT FOUND"


func _on_rebake_pressed() -> void:
	if _rebake_button == null or _rebake_button.disabled:
		return
	set_rebake_busy(true)
	rebake_requested.emit()


func set_rebake_busy(busy: bool) -> void:
	if _rebake_button == null:
		return
	_rebake_button.disabled = busy
	_rebake_button.text = "Rebaking planet..." if busy else "Rebake planet (ignore cache)"
