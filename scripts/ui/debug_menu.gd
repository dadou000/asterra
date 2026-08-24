class_name DebugMenu
extends CanvasLayer
## Runtime debug menu for terrain, procedural sky, exposure and world-generation tools.
##
## The menu is intentionally bound to '&' rather than Escape. Escape remains free
## for the normal game/pause flow and never opens or closes this debug interface.

signal opened
signal closed
signal rebake_requested
signal coast_profile_requested

const HISTOGRAM_EFFECT_SCRIPT = preload("res://scripts/rendering/screen_histogram_compositor_effect.gd")
const HISTOGRAM_PLOT_SCRIPT = preload("res://scripts/ui/screen_histogram_plot.gd")

## [property, label, note, default]
const TERRAIN_ROWS := [
	["wireframe", "Wireframe", "Shows the actual concentric clipmap triangles", false],
	["ring_indicator", "LOD ring labels", "Shows L0, L1, L2... directly on each active concentric ring", false],
	["freeze_terrain", "Freeze terrain clipmap", "Locks centre + active LODs in planet space while you fly around", false],
	["side_cut", "Side cut / sinking view", "Cuts away half the terrain and colours each LOD so overlap is visible", false],
]

const DEFAULT_SINK_SCALE := 2.0
const SKY_DEFAULT := 1.0
const EXPOSURE_DEBUG_REFRESH_SECONDS := 0.10

var debug: TerrainDebug
var procedural_sky: ProceduralSky
var sky_material: ShaderMaterial
var sky_resource: Sky
var eye_exposure: HumanEyeExposure

var _panel: PanelContainer
var _tabs: TabContainer
var _buttons := {}
var _rebake_button: Button
var _sink_label: Label
var _sink_slider: HSlider
var _sky_labels := {}
var _sky_sliders := {}
var _sky_target_label: Label

var _exposure_status_label: Label
var _histogram_stats_label: Label
var _histogram_plot: Control
var _histogram_capture_button: CheckButton
var _exposure_rate_label: Label
var _exposure_rate_slider: HSlider
var _exposure_comp_label: Label
var _exposure_comp_slider: HSlider
var _histogram_effect
var _histogram_user_enabled := true
var _exposure_refresh_accumulator := 0.0


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

	_tabs = TabContainer.new()
	_tabs.custom_minimum_size = Vector2(520, maxf(340.0, panel_h - 80.0))
	_tabs.size_flags_vertical = Control.SIZE_EXPAND_FILL
	root_box.add_child(_tabs)

	_build_terrain_tab(_tabs)
	_build_sky_tab(_tabs)
	_build_exposure_tab(_tabs)
	_build_world_tab(_tabs)
	_tabs.tab_changed.connect(_on_tab_changed)

	var hint := Label.new()
	hint.text = "& toggles debug  •  Esc is not bound to this menu"
	hint.add_theme_font_size_override("font_size", 11)
	hint.add_theme_color_override("font_color", Color(0.55, 0.62, 0.72))
	root_box.add_child(hint)


func _process(delta: float) -> void:
	if not visible or not _is_exposure_tab_active():
		return
	_exposure_refresh_accumulator += delta
	if _exposure_refresh_accumulator < EXPOSURE_DEBUG_REFRESH_SECONDS:
		return
	_exposure_refresh_accumulator = fmod(_exposure_refresh_accumulator,
		EXPOSURE_DEBUG_REFRESH_SECONDS)
	_resolve_exposure_targets()
	_refresh_exposure_debug()


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
	box.add_child(_note("Current renderer: procedural spherical L0-L14 clipmap"))

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
	box.add_child(_note("Clears ring labels, unfreezes, closes the cut and restores the normal 2× sink depth"))


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


func _build_exposure_tab(tabs: TabContainer) -> void:
	var scroll := ScrollContainer.new()
	scroll.name = "Exposure"
	scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	scroll.vertical_scroll_mode = ScrollContainer.SCROLL_MODE_AUTO
	scroll.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	tabs.add_child(scroll)

	var box := VBoxContainer.new()
	box.name = "ExposureControls"
	box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.add_theme_constant_override("separation", 5)
	scroll.add_child(box)

	box.add_child(_section_title("Human eye adaptation"))
	box.add_child(_note("The controller proxy selects asymmetric cone/rod timing. The histogram below is measured from the actual rendered HDR frame."))

	_exposure_status_label = Label.new()
	_exposure_status_label.text = "Exposure controller: waiting for player observer…"
	_exposure_status_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_exposure_status_label.add_theme_font_size_override("font_size", 12)
	_exposure_status_label.add_theme_color_override("font_color", Color(0.80, 0.86, 0.96))
	box.add_child(_exposure_status_label)

	var enabled_toggle := CheckButton.new()
	enabled_toggle.text = "Enable renderer auto exposure"
	enabled_toggle.button_pressed = true
	enabled_toggle.toggled.connect(_on_auto_exposure_toggled)
	box.add_child(enabled_toggle)

	var freeze_toggle := CheckButton.new()
	freeze_toggle.text = "Freeze adaptation"
	freeze_toggle.button_pressed = false
	freeze_toggle.toggled.connect(_on_freeze_adaptation_toggled)
	box.add_child(freeze_toggle)
	box.add_child(_note("Freezes both the physiology timer and renderer adaptation speed at the current state."))

	_exposure_comp_label = Label.new()
	_exposure_comp_label.text = "Exposure compensation: +0.0 EV"
	_exposure_comp_label.add_theme_font_size_override("font_size", 13)
	box.add_child(_exposure_comp_label)
	_exposure_comp_slider = HSlider.new()
	_exposure_comp_slider.min_value = -4.0
	_exposure_comp_slider.max_value = 4.0
	_exposure_comp_slider.step = 0.1
	_exposure_comp_slider.value = 0.0
	_exposure_comp_slider.value_changed.connect(_on_exposure_compensation_changed)
	box.add_child(_exposure_comp_slider)

	_exposure_rate_label = Label.new()
	_exposure_rate_label.text = "Adaptation rate multiplier: 1.00 ×"
	_exposure_rate_label.add_theme_font_size_override("font_size", 13)
	box.add_child(_exposure_rate_label)
	_exposure_rate_slider = HSlider.new()
	_exposure_rate_slider.min_value = 0.05
	_exposure_rate_slider.max_value = 4.0
	_exposure_rate_slider.step = 0.05
	_exposure_rate_slider.value = 1.0
	_exposure_rate_slider.value_changed.connect(_on_exposure_rate_changed)
	box.add_child(_exposure_rate_slider)

	var reset_state := Button.new()
	reset_state.text = "Reset adaptation state"
	reset_state.pressed.connect(_on_reset_adaptation_state)
	box.add_child(reset_state)
	box.add_child(_note("Restarts the cone/rod controller from the current view; Godot's internal exposure then converges at the selected rate."))

	var reset_debug := Button.new()
	reset_debug.text = "Reset eye debug controls"
	reset_debug.pressed.connect(_on_reset_eye_debug)
	box.add_child(reset_debug)

	box.add_child(HSeparator.new())
	box.add_child(_section_title("Screen HDR luminance histogram"))
	_histogram_capture_button = CheckButton.new()
	_histogram_capture_button.text = "Capture GPU screen histogram"
	_histogram_capture_button.button_pressed = true
	_histogram_capture_button.toggled.connect(_on_histogram_capture_toggled)
	box.add_child(_histogram_capture_button)
	box.add_child(_note("64 logarithmic bins, -12 to +8 EV around 18% grey. Sampled before tonemapping and auto exposure; capture runs only while this tab is visible."))

	_histogram_plot = HISTOGRAM_PLOT_SCRIPT.new()
	_histogram_plot.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.add_child(_histogram_plot)
	box.add_child(_note("Yellow = 18% grey  •  Green = median (P50)  •  Red = P95  •  bar height is logarithmic count"))

	_histogram_stats_label = Label.new()
	_histogram_stats_label.text = "GPU histogram: waiting…"
	_histogram_stats_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_histogram_stats_label.add_theme_font_size_override("font_size", 11)
	_histogram_stats_label.add_theme_color_override("font_color", Color(0.67, 0.75, 0.87))
	box.add_child(_histogram_stats_label)


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
		_resolve_exposure_targets()
		_update_histogram_capture_state()
		_refresh_exposure_debug()
		opened.emit()
	else:
		_update_histogram_capture_state()
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

	# Resolve the material through the actual WorldEnvironment -> Environment -> Sky
	# chain instead of trusting a script-side cached variable. This guarantees that
	# the sliders modify the material Godot is currently drawing.
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
		# REALTIME makes the radiance/reflection cubemap follow the same-frame uniform
		# edits too. Visible sky pixels already read the live material every frame.
		sky_resource.radiance_size = Sky.RADIANCE_SIZE_256
		sky_resource.process_mode = Sky.PROCESS_MODE_REALTIME
		var live_material := sky_resource.sky_material
		if live_material is ShaderMaterial:
			sky_material = live_material as ShaderMaterial
			break

	# Fallback for unusual harnesses without a WorldEnvironment child.
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


func _resolve_exposure_targets() -> void:
	var root := get_parent()
	if root == null:
		return

	if eye_exposure == null or not is_instance_valid(eye_exposure):
		for child in root.get_children():
			if child is HumanEyeExposure:
				eye_exposure = child as HumanEyeExposure
				break

	var world_environment: WorldEnvironment = null
	for child in root.get_children():
		if child is WorldEnvironment:
			world_environment = child as WorldEnvironment
			break
	if world_environment == null:
		return

	if _histogram_effect == null:
		_histogram_effect = HISTOGRAM_EFFECT_SCRIPT.new()

	var compositor: Compositor = world_environment.compositor
	if compositor == null:
		compositor = Compositor.new()
	var effects: Array[CompositorEffect] = compositor.compositor_effects
	if not effects.has(_histogram_effect):
		# Append after existing effects (notably depth-aware clouds) so the histogram
		# measures the final scene-linear 3D frame before built-in post processing.
		effects.append(_histogram_effect)
		compositor.compositor_effects = effects
	world_environment.compositor = compositor
	_update_histogram_capture_state()


func _on_tab_changed(_tab: int) -> void:
	if _is_exposure_tab_active():
		_resolve_exposure_targets()
		_exposure_refresh_accumulator = EXPOSURE_DEBUG_REFRESH_SECONDS
	_update_histogram_capture_state()


func _is_exposure_tab_active() -> bool:
	if _tabs == null or _tabs.get_tab_count() <= 0:
		return false
	return _tabs.get_tab_title(_tabs.current_tab) == "Exposure"


func _update_histogram_capture_state() -> void:
	if _histogram_effect == null:
		return
	var should_capture := visible and _histogram_user_enabled and _is_exposure_tab_active()
	_histogram_effect.set_capture_enabled(should_capture)


func _refresh_exposure_debug() -> void:
	if _exposure_status_label != null:
		if eye_exposure == null or not is_instance_valid(eye_exposure):
			_exposure_status_label.text = "Exposure controller: NOT FOUND"
		else:
			var state := eye_exposure.debug_snapshot()
			_exposure_status_label.text = (
				"Mode: %s  |  phase: %s  |  %s\n"
				+ "Controller meter proxy: %.5f  |  adapted proxy: %.5f  |  nominal adaptation: %+.2f EV\n"
				+ "Dark timer: %.1f / %.0f s  |  rate: %.5f s⁻¹  |  meter floor: %.5f\n"
				+ "Highlight floor: %.5f  |  darkness floor: %.5f  |  sensitivity bounds: %.2f … %.2f"
			) % [
				"AUTO" if bool(state.get("auto_exposure_enabled", false)) else "MANUAL/OFF",
				String(state.get("stage", "?")),
				String(state.get("direction", "?")),
				float(state.get("metered_proxy", 0.0)),
				float(state.get("adapted_luminance", 0.0)),
				float(state.get("nominal_adaptation_ev", 0.0)),
				float(state.get("dark_seconds", 0.0)),
				float(state.get("rod_onset_seconds", 0.0)),
				float(state.get("adaptation_rate", 0.0)),
				float(state.get("meter_floor", 0.0)),
				float(state.get("highlight_floor", 0.0)),
				float(state.get("darkness_floor", 0.0)),
				float(state.get("min_sensitivity", 0.0)),
				float(state.get("max_sensitivity", 0.0)),
			]

	if _histogram_effect == null:
		if _histogram_stats_label != null:
			_histogram_stats_label.text = "GPU histogram: effect not installed"
		return
	if not _histogram_effect.is_ready():
		if _histogram_stats_label != null:
			_histogram_stats_label.text = "GPU histogram unavailable: RenderingDevice compute pipeline did not initialize"
		return

	var histogram: Dictionary = _histogram_effect.get_snapshot()
	if histogram.is_empty():
		if _histogram_stats_label != null:
			_histogram_stats_label.text = "GPU histogram: waiting for asynchronous readback…"
		return

	if _histogram_plot != null:
		_histogram_plot.set_snapshot(histogram)
	if _histogram_stats_label != null:
		_histogram_stats_label.text = (
			"HDR geometric mean: %.6f (%+.2f EV)  |  arithmetic mean: %.6f\n"
			+ "P01 %+.2f EV  •  P50 %+.2f EV  •  P95 %+.2f EV  •  P99 %+.2f EV\n"
			+ "Samples: %d (1/%d px per axis)  |  below range: %.3f%%  |  above range: %.3f%%  |  invalid: %d"
		) % [
			float(histogram.get("geometric_mean_luminance", 0.0)),
			float(histogram.get("mean_ev", 0.0)),
			float(histogram.get("arithmetic_mean_luminance", 0.0)),
			float(histogram.get("p01_ev", 0.0)),
			float(histogram.get("p50_ev", 0.0)),
			float(histogram.get("p95_ev", 0.0)),
			float(histogram.get("p99_ev", 0.0)),
			int(histogram.get("total_count", 0)),
			int(histogram.get("sample_stride", 4)),
			float(histogram.get("below_fraction", 0.0)) * 100.0,
			float(histogram.get("above_fraction", 0.0)) * 100.0,
			int(histogram.get("invalid_count", 0)),
		]


func _on_auto_exposure_toggled(enabled_value: bool) -> void:
	_resolve_exposure_targets()
	if eye_exposure != null:
		eye_exposure.set_debug_auto_exposure_enabled(enabled_value)
	_refresh_exposure_debug()


func _on_freeze_adaptation_toggled(frozen: bool) -> void:
	_resolve_exposure_targets()
	if eye_exposure != null:
		eye_exposure.set_debug_freeze_adaptation(frozen)
	_refresh_exposure_debug()


func _on_exposure_compensation_changed(value: float) -> void:
	if _exposure_comp_label != null:
		_exposure_comp_label.text = "Exposure compensation: %+.1f EV" % value
	_resolve_exposure_targets()
	if eye_exposure != null:
		eye_exposure.set_debug_exposure_compensation_ev(value)


func _on_exposure_rate_changed(value: float) -> void:
	if _exposure_rate_label != null:
		_exposure_rate_label.text = "Adaptation rate multiplier: %.2f ×" % value
	_resolve_exposure_targets()
	if eye_exposure != null:
		eye_exposure.set_debug_rate_scale(value)


func _on_reset_adaptation_state() -> void:
	_resolve_exposure_targets()
	if eye_exposure != null:
		eye_exposure.reset_adaptation_state()
	_refresh_exposure_debug()


func _on_reset_eye_debug() -> void:
	_resolve_exposure_targets()
	if eye_exposure != null:
		eye_exposure.reset_debug_controls()
	if _exposure_comp_slider != null:
		_exposure_comp_slider.set_value_no_signal(0.0)
	if _exposure_comp_label != null:
		_exposure_comp_label.text = "Exposure compensation: +0.0 EV"
	if _exposure_rate_slider != null:
		_exposure_rate_slider.set_value_no_signal(1.0)
	if _exposure_rate_label != null:
		_exposure_rate_label.text = "Adaptation rate multiplier: 1.00 ×"
	_refresh_exposure_debug()


func _on_histogram_capture_toggled(enabled_value: bool) -> void:
	_histogram_user_enabled = enabled_value
	_update_histogram_capture_state()
	if not enabled_value and _histogram_stats_label != null:
		_histogram_stats_label.text = "GPU histogram capture paused"


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
