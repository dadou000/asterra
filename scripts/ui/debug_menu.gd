class_name DebugMenu
extends CanvasLayer
## Escape menu: the visual debug switches.
##
## Everything in here answers a question about the renderer that cannot be
## answered by looking at a finished frame -- where one tile ends and the next
## begins, which level of the quadtree drew it, whether its UVs agree with its
## place on the cube sphere. Each row is a toggle and a one-line note on what
## seeing it would mean.

signal opened
signal closed
signal rebake_requested
signal coast_profile_requested

## [property, label, note, default]
const ROWS := [
	["heightmap", "Terrain heightmap", "Off meshes the bare cube sphere -- costs a re-mesh", true],
	["surface_texture", "Surface texture", "Off paints plain grey: shape without colour", true],
	["tile_axes", "Tile axes (3D)", "Arrow along +v, tick along +u, built from the quadtree", false],
	["uv_glyphs", "Tile UV glyph", "The same glyph drawn from the mesh UVs -- the two must agree", false],
	["lod_tint", "Tint by LOD depth", "One colour per quadtree level", false],
	["face_seams", "Cube-face seams", "The twelve cube edges, where they land on the sphere", false],
	["hide_water", "Hide water", "Leaves the sea bed, to see what the ocean is covering", false],
	["wireframe", "Wireframe", "Triangles as meshed, skirts included", false],
	["freeze_stream", "Freeze streaming", "Stops the quadtree restructuring while you look at it", false],
]

var debug: TerrainDebug

var _panel: PanelContainer
var _buttons := {}
var _lod_button: CheckButton
var _rebake_button: Button
var _lod_depth := 5

func _ready() -> void:
	layer = 20
	visible = false
	process_mode = Node.PROCESS_MODE_ALWAYS

	_panel = PanelContainer.new()
	_panel.set_anchors_preset(Control.PRESET_CENTER_LEFT)
	_panel.position = Vector2(40, -230)
	_panel.custom_minimum_size = Vector2(430, 0)
	var style := StyleBoxFlat.new()
	style.bg_color = Color(0.05, 0.06, 0.09, 0.90)
	style.border_color = Color(0.35, 0.45, 0.60, 0.9)
	style.set_border_width_all(1)
	style.set_corner_radius_all(4)
	style.set_content_margin_all(14)
	_panel.add_theme_stylebox_override("panel", style)
	add_child(_panel)

	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 2)
	_panel.add_child(box)

	var title := Label.new()
	title.text = "DEBUG"
	title.add_theme_font_size_override("font_size", 15)
	title.add_theme_color_override("font_color", Color(0.95, 0.97, 1.0))
	box.add_child(title)

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

	# Forced LOD gets a value as well as a switch, so it is its own little block.
	_lod_button = CheckButton.new()
	_lod_button.text = "Force LOD depth: %d" % _lod_depth
	_lod_button.add_theme_font_size_override("font_size", 13)
	_lod_button.toggled.connect(_on_force_lod_toggled)
	box.add_child(_lod_button)
	box.add_child(_note("Every tile at one depth, distance ignored (budget-capped)"))
	var slider := HSlider.new()
	slider.min_value = 0
	slider.max_value = 8
	slider.step = 1
	slider.value = _lod_depth
	slider.custom_minimum_size = Vector2(0, 18)
	slider.value_changed.connect(_on_lod_depth_changed)
	box.add_child(slider)

	var separator := HSeparator.new()
	separator.add_theme_constant_override("separation", 8)
	box.add_child(separator)

	var coast_profile := Button.new()
	coast_profile.text = "Edit coastline terrain profile..."
	coast_profile.add_theme_font_size_override("font_size", 13)
	coast_profile.pressed.connect(func(): coast_profile_requested.emit())
	box.add_child(coast_profile)
	box.add_child(_note("Edits terrain height by geodesic distance toward the sea only"))

	_rebake_button = Button.new()
	_rebake_button.text = "Rebake planet (ignore cache)"
	_rebake_button.add_theme_font_size_override("font_size", 13)
	_rebake_button.pressed.connect(_on_rebake_pressed)
	box.add_child(_rebake_button)
	box.add_child(_note("Regenerates all planet fields from the seed and bypasses the saved .bake cache"))

	var hint := Label.new()
	hint.text = "\nEsc to close"
	hint.add_theme_font_size_override("font_size", 11)
	hint.add_theme_color_override("font_color", Color(0.55, 0.62, 0.72))
	box.add_child(hint)

## Handled in _input rather than _unhandled_input so that a focused check button
## can never swallow the key that closes the menu. Before the first planet bake
## finishes `debug` is still null and there is no player/terrain to control, so
## ignore attempts to open the menu during that short startup window.
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

func _on_force_lod_toggled(pressed: bool) -> void:
	if debug != null:
		debug.force_lod = pressed

func _on_lod_depth_changed(value: float) -> void:
	_lod_depth = int(value)
	_lod_button.text = "Force LOD depth: %d" % _lod_depth
	if debug != null:
		debug.force_lod_depth = _lod_depth

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
