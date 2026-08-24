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
	["wireframe", "Wireframe", "Shows the actual concentric clipmap triangles", false],
	["ring_indicator", "LOD ring labels", "Shows L0, L1, L2... directly on each active concentric ring", false],
	["freeze_terrain", "Freeze terrain clipmap", "Locks centre + active LODs in planet space while you fly around", false],
	["side_cut", "Side cut / sinking view", "Cuts away half the terrain and colours each LOD so overlap is visible", false],
]

const DEFAULT_SINK_SCALE := 2.0
const SKY_DEFAULT := 1.0

var debug: TerrainDebug
var procedural_sky: ProceduralSky

var _panel: PanelContainer
var _buttons := {}
var _rebake_button: Button
var _sink_label: Label
var _sink_slider: HSlider
var _sky_labels := {}
var _sky_sliders := {}


func _ready() -> void:
	layer = 20
	visible = false
	process_mode = Node.PROCESS_MODE_ALWAYS

	_panel = PanelContainer.new()
	_panel.set_anchors_preset(Control.PRESET_CENTER_LEFT)
	_panel.position = Vector2(36, -275)
	_panel.custom_minimum_size = Vector2(550, 550)
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
	tabs.custom_minimum_size = Vector2(520, 470)
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
	var box := VBoxContainer.new()
	box.name = "Terrain"
	box.add_theme_constant_override("separation", 4)
	tabs.add_child(box)

	box.add_child(_section_title("Concentric GPU clipmap"))
	box.add_child(_note("Current renderer: procedural spherical L0-L14 clipmap"))

	for row in TERRAIN_ROWS:
		var key: String = row[0]
		var button := CheckButton.new()
		button.text = row[1]
		button.button_pressed = row[3]
		button.add_theme_font_size_override("font_size", 13)
		button.toggled.connect(_on_toggled.bind(key))
		box.add_child(button)
		_buttons[key] = button
		box.add_child(_note(row[2]))

	_sink_label = Label.new()
	_sink_label.text = "Sink depth: %.2f × LOD spacing" % DEFAULT_SINK_SCALE
	_sink_label.add_theme_font_size_override("font_size", 13)
	box.add_child(_sink_label)
	box.add_child(_note("Default is 2×. Increase only while inspecting overlap."))

	_sink_slider = HSlider.new()
	_sink_slider.min_value = 0.0
	_sink_slider.max_value = 12.0
	_sink_slider.step = 0.25
	_sink_slider.value = DEFAULT_SINK_SCALE
	_sink_slider.custom_minimum_size = Vector2(0, 22)
	_sink_slider.value_changed.connect(_on_sink_scale_changed)
	box.add_child(_sink_slider)

	var reset := Button.new()
	reset.text = "Reset terrain inspection"
	reset.add_theme_font_size_override("font_size", 13)
	reset.pressed.connect(_on_reset_inspection)
	box.add_child(reset)
	box.add_child(_note("Clears ring labels, unfreezes, closes the cut and restores the normal 2× sink depth"))


func _build_sky_tab(tabs: TabContainer) -> void:
	var box := VBoxContainer.new()
	box.name = "Sky"
	box.add_theme_constant_override("separation", 4)
	tabs.add_child(box)

	box.add_child(_section_title("Procedural stellar rendering"))
	box.add_child(_note("Defaults are the physically calibrated path. Overrides stay constant day and night."))
	box.add_child(_note("Brightness still emerges through atmosphere + exposure; no night visibility boost exists."))

	_add_sky_slider(
		box,
		"radiance",
		"Stellar radiance calibration",
		"Global fixed photometric calibration. 1.00 = default; never driven by sun altitude.",
		0.25, 2.0, 0.05, SKY_DEFAULT
	)
	_add_sky_slider(
		box,
		"magnitude",
		"Magnitude flux response",
		"1.00 = physical Pogson law: 5 magnitudes = 100× flux, 8 magnitudes ≈ 1585×.",
		0.5, 1.5, 0.05, SKY_DEFAULT
	)
	_add_sky_slider(
		box,
		"seeing",
		"Atmospheric seeing / scintillation",
		"Scales turbulence only. Near-horizon stars still scintillate more strongly by geometry.",
		0.0, 2.0, 0.05, SKY_DEFAULT
	)
	_add_sky_slider(
		box,
		"colour",
		"Naked-eye stellar colour response",
		"1.00 = temperature-derived physical colour; 0.00 = luminance-preserving monochrome diagnostic.",
		0.0, 1.0, 0.05, SKY_DEFAULT
	)
	_add_sky_slider(
		box,
		"chromatic",
		"Chromatic scintillation",
		"Low-altitude red/blue turbulence flashes. Luminance is preserved exactly.",
		0.0, 2.0, 0.05, SKY_DEFAULT
	)

	var reset := Button.new()
	reset.text = "Reset sky calibration to physical defaults"
	reset.add_theme_font_size_override("font_size", 13)
	reset.pressed.connect(_on_reset_sky)
	box.add_child(reset)
	box.add_child(_note("Catalogue density is magnitude-limited: lower graphics tiers remove the faintest stars first."))


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

	# Unicode 38 is '&'. Using the produced character makes this work correctly on
	# AZERTY (where '&' is the unshifted 1 key) as well as other keyboard layouts.
	if key_event.unicode == 38:
		toggle()
		get_viewport().set_input_as_handled()


func toggle() -> void:
	visible = not visible
	if visible:
		_resolve_procedural_sky()
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
	if key == "ring_indicator":
		if GroundGeometryClipmap.has_method("set_debug_ring_indicator"):
			GroundGeometryClipmap.call("set_debug_ring_indicator", pressed)
		return
	if debug != null:
		debug.set(key, pressed)


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
	if GroundGeometryClipmap.has_method("set_debug_ring_indicator"):
		GroundGeometryClipmap.call("set_debug_ring_indicator", false)
	if _sink_slider != null:
		_sink_slider.set_value_no_signal(DEFAULT_SINK_SCALE)
	if _sink_label != null:
		_sink_label.text = "Sink depth: %.2f × LOD spacing" % DEFAULT_SINK_SCALE
	if debug != null:
		debug.reset_inspection()


func _on_sky_slider_changed(value: float, key: String) -> void:
	_update_sky_label(key, value)
	_resolve_procedural_sky()
	if procedural_sky == null:
		return
	match key:
		"radiance":
			procedural_sky.set_star_radiance_scale(value)
		"magnitude":
			procedural_sky.set_magnitude_flux_exponent(value)
		"seeing":
			procedural_sky.set_seeing_strength(value)
		"colour":
			procedural_sky.set_colour_strength(value)
		"chromatic":
			procedural_sky.set_chromatic_scintillation_strength(value)


func _update_sky_label(key: String, value: float, explicit_title: String = "") -> void:
	var label_value: Variant = _sky_labels.get(key)
	if not (label_value is Label):
		return
	var label := label_value as Label
	var title := explicit_title
	if title.is_empty():
		match key:
			"radiance": title = "Stellar radiance calibration"
			"magnitude": title = "Magnitude flux response"
			"seeing": title = "Atmospheric seeing / scintillation"
			"colour": title = "Naked-eye stellar colour response"
			"chromatic": title = "Chromatic scintillation"
	label.text = "%s: %.2f ×" % [title, value]


func _on_reset_sky() -> void:
	_resolve_procedural_sky()
	if procedural_sky != null:
		procedural_sky.reset_debug_calibration()
	for key_value: Variant in ["radiance", "magnitude", "seeing", "colour", "chromatic"]:
		var key := String(key_value)
		var slider_value: Variant = _sky_sliders.get(key)
		if slider_value is HSlider:
			(slider_value as HSlider).set_value_no_signal(SKY_DEFAULT)
		_update_sky_label(key, SKY_DEFAULT)


func _resolve_procedural_sky() -> void:
	if procedural_sky != null and is_instance_valid(procedural_sky):
		return
	var root := get_parent()
	if root == null:
		return
	var candidate := root.get_node_or_null("ProceduralSky")
	if candidate is ProceduralSky:
		procedural_sky = candidate as ProceduralSky


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
