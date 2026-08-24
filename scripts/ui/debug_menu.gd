class_name DebugMenu
extends CanvasLayer
## Escape menu for the active spherical procedural terrain renderer.
##
## Obsolete quadtree/tile/UV/forced-depth tools have been removed. The remaining
## controls operate directly on the concentric GPU clipmap.

signal opened
signal closed
signal rebake_requested
signal coast_profile_requested

## [property, label, note, default]
const ROWS := [
	["wireframe", "Wireframe", "Shows the actual concentric clipmap triangles", false],
	["freeze_terrain", "Freeze terrain clipmap", "Locks centre + active LODs in planet space while you fly around", false],
	["side_cut", "Side cut / sinking view", "Cuts away half the terrain and colours each LOD so overlap is visible", false],
]

const DEFAULT_SINK_SCALE := 2.0

var debug: TerrainDebug

var _panel: PanelContainer
var _buttons := {}
var _rebake_button: Button
var _sink_label: Label
var _sink_slider: HSlider


func _ready() -> void:
	layer = 20
	visible = false
	process_mode = Node.PROCESS_MODE_ALWAYS

	_panel = PanelContainer.new()
	_panel.set_anchors_preset(Control.PRESET_CENTER_LEFT)
	_panel.position = Vector2(40, -190)
	_panel.custom_minimum_size = Vector2(455, 0)
	var style := StyleBoxFlat.new()
	style.bg_color = Color(0.05, 0.06, 0.09, 0.90)
	style.border_color = Color(0.35, 0.45, 0.60, 0.9)
	style.set_border_width_all(1)
	style.set_corner_radius_all(4)
	style.set_content_margin_all(14)
	_panel.add_theme_stylebox_override("panel", style)
	add_child(_panel)

	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 3)
	_panel.add_child(box)

	var title := Label.new()
	title.text = "TERRAIN DEBUG"
	title.add_theme_font_size_override("font_size", 15)
	title.add_theme_color_override("font_color", Color(0.95, 0.97, 1.0))
	box.add_child(title)

	box.add_child(_note("Current renderer: procedural GPU L0-L14 spherical clipmap"))

	for row in ROWS:
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
	box.add_child(_note("Default is 2×. Raise temporarily to make the overlap easier to inspect."))

	_sink_slider = HSlider.new()
	_sink_slider.min_value = 0.0
	_sink_slider.max_value = 12.0
	_sink_slider.step = 0.25
	_sink_slider.value = DEFAULT_SINK_SCALE
	_sink_slider.custom_minimum_size = Vector2(0, 20)
	_sink_slider.value_changed.connect(_on_sink_scale_changed)
	box.add_child(_sink_slider)

	var reset := Button.new()
	reset.text = "Reset terrain inspection"
	reset.add_theme_font_size_override("font_size", 13)
	reset.pressed.connect(_on_reset_inspection)
	box.add_child(reset)
	box.add_child(_note("Unfreezes, closes the cut and restores the normal 2× sink depth"))

	var separator := HSeparator.new()
	separator.add_theme_constant_override("separation", 8)
	box.add_child(separator)

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
	box.add_child(_note("Regenerates the macro planet fields from the seed"))

	var hint := Label.new()
	hint.text = "\nRecommended inspection: Freeze + Side cut + Wireframe, then fly to the cut edge.\nEsc to close"
	hint.add_theme_font_size_override("font_size", 11)
	hint.add_theme_color_override("font_color", Color(0.55, 0.62, 0.72))
	box.add_child(hint)


func _input(event: InputEvent) -> void:
	if event is InputEventKey and event.pressed and not event.echo \
			and event.keycode == KEY_ESCAPE:
		if not visible and debug == null:
			get_viewport().set_input_as_handled()
			return
		toggle()
		get_viewport().set_input_as_handled()


func toggle() -> void:
	visible = not visible
	if visible:
		opened.emit()
	else:
		closed.emit()


func _note(text: String) -> Label:
	var note := Label.new()
	note.text = "     %s" % text
	note.add_theme_font_size_override("font_size", 11)
	note.add_theme_color_override("font_color", Color(0.58, 0.65, 0.76))
	return note


func _on_toggled(pressed: bool, key: String) -> void:
	if debug != null:
		debug.set(key, pressed)


func _on_sink_scale_changed(value: float) -> void:
	if _sink_label != null:
		_sink_label.text = "Sink depth: %.2f × LOD spacing" % value
	if debug != null:
		debug.sink_scale = value


func _on_reset_inspection() -> void:
	for key_value: Variant in ["freeze_terrain", "side_cut"]:
		var key := String(key_value)
		var button_value: Variant = _buttons.get(key)
		if button_value is CheckButton:
			(button_value as CheckButton).set_pressed_no_signal(false)
	if _sink_slider != null:
		_sink_slider.set_value_no_signal(DEFAULT_SINK_SCALE)
	if _sink_label != null:
		_sink_label.text = "Sink depth: %.2f × LOD spacing" % DEFAULT_SINK_SCALE
	if debug != null:
		debug.reset_inspection()


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
