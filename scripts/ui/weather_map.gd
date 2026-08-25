extends CanvasLayer
## Live whole-planet meteorological map driven by WeatherSystem.
## `é` toggles it on French AZERTY layouts. Global and local products are sampled
## from the same six-layer AVX2 atmosphere used by the cloud renderer.

enum Product {
	COMPOSITE,
	SYNOPTIC,
	CLOUD_COVER,
	PRECIPITATION,
	CONVECTION,
	PRESSURE,
	AIR_TEMPERATURE,
	SEA_TEMPERATURE,
	CAPE,
	IRRADIANCE,
	VORTICITY,
	DIVERGENCE,
	POTENTIAL_VORTICITY,
	WIND_SHEAR,
}

const PRODUCT_NAMES := [
	"Composite weather",
	"Synoptic analysis",
	"Cloud cover",
	"Precipitation",
	"Organised storm intensity",
	"Surface pressure anomaly",
	"Near-surface air temperature",
	"Sea-surface temperature",
	"Convective available potential energy (CAPE)",
	"Absorbed solar irradiance",
	"Relative vorticity",
	"Low-level divergence",
	"Potential-vorticity proxy",
	"Vertical wind shear",
]
const PRODUCT_NOTES := [
	"Cloud shield + precipitation + organised convective cores",
	"Pressure systems with automatic H/L and tropical/extratropical storm classification",
	"Vertically integrated liquid/ice cloud condensate",
	"Current model precipitation intensity",
	"Vertical mass flux + instability + rotation + shear",
	"Boundary-layer pressure/geopotential perturbation",
	"Lowest model-layer air temperature",
	"Live ocean mixed-layer skin temperature; land is left uncoloured",
	"Parcel-buoyancy proxy from low-level moisture and model-layer instability",
	"Solar power absorbed by the live land/ocean surface after clouds and terrain shading",
	"Strongest signed relative vorticity across low/mid layers",
	"Signed divergence in the low storm-inflow layer; negative = convergence",
	"Upper-level absolute vorticity × potential-temperature stratification",
	"Maximum adjacent-layer horizontal wind-vector difference",
]
const LEGEND_LEFT := [
	"Clear", "Deep low", "0 %", "None", "Stable", "-60 hPa",
	"−53 °C", "−13 °C", "0 J/kg", "0 W/m²",
	"Anticyclonic", "Convergence", "Negative PV", "0 m/s",
]
const LEGEND_RIGHT := [
	"Severe", "Strong high", "100 %", "Intense", "Organised", "+60 hPa",
	"+47 °C", "+32 °C", "4000+ J/kg", "1200+ W/m²",
	"Cyclonic", "Divergence", "Positive PV", "55+ m/s",
]

const W := 960
const H := 480
const BASE_RENDER_BUDGET_USEC := 2400
const TUNING_LABELS := {
	&"circulation": "Circulation",
	&"temperature": "Radiative heat",
	&"humidity": "Humidity supply",
	&"cloud_microphysics": "Cloud physics",
	&"convection": "Convection",
	&"precipitation": "Precipitation",
}
const TUNING_TOOLTIPS := {
	&"circulation": "Large-scale wind and pressure-pattern restoration.",
	&"temperature": "Radiative and surface temperature relaxation rate.",
	&"humidity": "Moisture restoration toward the saturation-limited climate.",
	&"cloud_microphysics": "Condensation, evaporation, freezing, melting, and cloud decay.",
	&"convection": "Resolved vertical transport from instability and convergence.",
	&"precipitation": "Liquid/ice autoconversion and fallout rate.",
}
const LAYER_LABELS := [
	"L1  0.45 km", "L2  1.7 km", "L3  3.3 km",
	"L4  5.6 km", "L5  8.5 km", "L6  12.8 km",
]

var product_index: int = Product.COMPOSITE
var texture_rect: TextureRect
var title: Label
var note: Label
var product_select: OptionButton
var legend_texture: TextureRect
var legend_left: Label
var legend_right: Label
var marker: Control
var simulation_weight_label: Label
var simulation_weight_slider: HSlider
var simulation_speed_label: Label
var simulation_speed_slider: HSlider
var warp_indicator_label: Label
var tuning_panel: PanelContainer
var tuning_button: Button
var _tuning_sliders := {}
var _tuning_value_labels := {}
var _layer_sliders: Array[HSlider] = []
var _layer_value_labels: Array[Label] = []
var _material: ShaderMaterial
var _player: AsterraPlayer
var _player_dir := Vector3(1, 0, 0)
var _systems: Array[Dictionary] = []
var _system_scan_accum := 0.0

var _base_image: Image
var _base_texture: ImageTexture
var _base_render_y := -1
var _base_fields: PlanetFields
var _base_grid: PlanetGrid


func _ready() -> void:
	layer = 30
	visible = false
	process_mode = Node.PROCESS_MODE_ALWAYS

	var bg := ColorRect.new()
	bg.color = Color(0.012, 0.018, 0.030, 0.965)
	bg.set_anchors_preset(Control.PRESET_FULL_RECT)
	add_child(bg)

	texture_rect = TextureRect.new()
	texture_rect.set_anchors_preset(Control.PRESET_CENTER)
	texture_rect.position = Vector2(-float(W) * 0.5, -float(H) * 0.5)
	texture_rect.custom_minimum_size = Vector2(W, H)
	texture_rect.stretch_mode = TextureRect.STRETCH_SCALE
	texture_rect.texture = _placeholder_texture()
	_material = ShaderMaterial.new()
	_material.shader = load("res://shaders/weather_map.gdshader")
	texture_rect.material = _material
	add_child(texture_rect)

	title = Label.new()
	title.set_anchors_preset(Control.PRESET_CENTER_TOP)
	title.position = Vector2(-float(W) * 0.5, 24)
	title.custom_minimum_size = Vector2(W, 28)
	title.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	title.add_theme_font_size_override("font_size", 19)
	title.add_theme_color_override("font_color", Color(0.94, 0.97, 1.0))
	add_child(title)

	product_select = OptionButton.new()
	product_select.set_anchors_preset(Control.PRESET_CENTER_TOP)
	product_select.position = Vector2(-190, 56)
	product_select.custom_minimum_size = Vector2(380, 36)
	for product_name in PRODUCT_NAMES:
		product_select.add_item(product_name)
	product_select.item_selected.connect(_on_product_selected)
	add_child(product_select)

	simulation_weight_label = Label.new()
	simulation_weight_label.set_anchors_preset(Control.PRESET_CENTER_TOP)
	simulation_weight_label.position = Vector2(-370, 99)
	simulation_weight_label.custom_minimum_size = Vector2(260, 28)
	simulation_weight_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	simulation_weight_label.add_theme_font_size_override("font_size", 13)
	add_child(simulation_weight_label)

	simulation_weight_slider = HSlider.new()
	simulation_weight_slider.set_anchors_preset(Control.PRESET_CENTER_TOP)
	simulation_weight_slider.position = Vector2(-92, 98)
	simulation_weight_slider.custom_minimum_size = Vector2(300, 28)
	simulation_weight_slider.min_value = WeatherSystem.SIMULATION_WEIGHT_MIN
	simulation_weight_slider.max_value = WeatherSystem.SIMULATION_WEIGHT_MAX
	simulation_weight_slider.step = 0.05
	simulation_weight_slider.value = WeatherSystem.simulation_weight
	simulation_weight_slider.value_changed.connect(_on_simulation_weight_changed)
	add_child(simulation_weight_slider)

	var reset_weight := Button.new()
	reset_weight.set_anchors_preset(Control.PRESET_CENTER_TOP)
	reset_weight.position = Vector2(225, 96)
	reset_weight.custom_minimum_size = Vector2(145, 32)
	reset_weight.text = "Calibrated 1×"
	reset_weight.pressed.connect(WeatherSystem.reset_simulation_weight)
	add_child(reset_weight)
	WeatherSystem.simulation_weight_changed.connect(_on_external_simulation_weight_changed)
	_update_simulation_weight_label(WeatherSystem.simulation_weight)

	simulation_speed_label = Label.new()
	simulation_speed_label.set_anchors_preset(Control.PRESET_CENTER_TOP)
	simulation_speed_label.position = Vector2(-370, 140)
	simulation_speed_label.custom_minimum_size = Vector2(260, 28)
	simulation_speed_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	simulation_speed_label.add_theme_font_size_override("font_size", 13)
	add_child(simulation_speed_label)

	simulation_speed_slider = HSlider.new()
	simulation_speed_slider.set_anchors_preset(Control.PRESET_CENTER_TOP)
	simulation_speed_slider.position = Vector2(-92, 139)
	simulation_speed_slider.custom_minimum_size = Vector2(300, 28)
	simulation_speed_slider.min_value = 0.0
	simulation_speed_slider.max_value = 14.0
	simulation_speed_slider.step = 1.0
	simulation_speed_slider.value = _speed_to_slider(WeatherSystem.simulation_speed)
	simulation_speed_slider.tooltip_text = "Logarithmic weather warp: paused, then powers of two from 1× to 8192×. Above 256× the global model spins up and the local nest resynchronises afterward."
	simulation_speed_slider.value_changed.connect(_on_simulation_speed_slider_changed)
	add_child(simulation_speed_slider)

	var reset_speed := Button.new()
	reset_speed.set_anchors_preset(Control.PRESET_CENTER_TOP)
	reset_speed.position = Vector2(225, 137)
	reset_speed.custom_minimum_size = Vector2(145, 32)
	reset_speed.text = "Normal 1×"
	reset_speed.pressed.connect(WeatherSystem.reset_simulation_speed)
	add_child(reset_speed)
	WeatherSystem.simulation_speed_changed.connect(_on_external_simulation_speed_changed)
	_update_simulation_speed_label(WeatherSystem.simulation_speed)

	warp_indicator_label = Label.new()
	warp_indicator_label.set_anchors_preset(Control.PRESET_CENTER_TOP)
	warp_indicator_label.position = Vector2(375, 132)
	warp_indicator_label.custom_minimum_size = Vector2(104, 42)
	warp_indicator_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	warp_indicator_label.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	warp_indicator_label.add_theme_font_size_override("font_size", 10)
	warp_indicator_label.add_theme_color_override("font_color", Color(0.48, 0.86, 0.94))
	warp_indicator_label.tooltip_text = "Top: simulated duration advanced per real second. Bottom: cumulative simulated time gained beyond wall-clock time this session."
	add_child(warp_indicator_label)
	_update_warp_indicator()

	_create_tuning_panel()
	WeatherSystem.tuning_weight_changed.connect(_on_external_tuning_weight_changed)
	WeatherSystem.layer_weight_changed.connect(_on_external_layer_weight_changed)
	WeatherSystem.physics_tuning_reset.connect(_refresh_tuning_controls)

	note = Label.new()
	note.set_anchors_preset(Control.PRESET_CENTER_BOTTOM)
	note.position = Vector2(-float(W) * 0.5, -84)
	note.custom_minimum_size = Vector2(W, 22)
	note.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	note.add_theme_font_size_override("font_size", 12)
	note.add_theme_color_override("font_color", Color(0.68, 0.75, 0.84))
	add_child(note)

	legend_texture = TextureRect.new()
	legend_texture.set_anchors_preset(Control.PRESET_CENTER_BOTTOM)
	legend_texture.position = Vector2(-210, -52)
	legend_texture.custom_minimum_size = Vector2(420, 12)
	legend_texture.texture = _make_legend_texture(product_index)
	legend_texture.stretch_mode = TextureRect.STRETCH_SCALE
	add_child(legend_texture)

	legend_left = Label.new()
	legend_left.set_anchors_preset(Control.PRESET_CENTER_BOTTOM)
	legend_left.position = Vector2(-210, -38)
	legend_left.custom_minimum_size = Vector2(180, 20)
	legend_left.add_theme_font_size_override("font_size", 11)
	add_child(legend_left)

	legend_right = Label.new()
	legend_right.set_anchors_preset(Control.PRESET_CENTER_BOTTOM)
	legend_right.position = Vector2(30, -38)
	legend_right.custom_minimum_size = Vector2(180, 20)
	legend_right.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	legend_right.add_theme_font_size_override("font_size", 11)
	add_child(legend_right)

	var hint := Label.new()
	hint.set_anchors_preset(Control.PRESET_CENTER_BOTTOM)
	hint.position = Vector2(-float(W) * 0.5, -18)
	hint.custom_minimum_size = Vector2(W, 18)
	hint.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	hint.text = "é close  •  ← / → product  •  1–9 direct  •  P physics tuning  •  6 sigma layers"
	hint.add_theme_font_size_override("font_size", 11)
	hint.add_theme_color_override("font_color", Color(0.52, 0.59, 0.69))
	add_child(hint)

	marker = Control.new()
	marker.set_anchors_preset(Control.PRESET_CENTER)
	marker.draw.connect(_draw_marker)
	add_child(marker)

	_apply_product()


func _create_tuning_panel() -> void:
	tuning_button = Button.new()
	tuning_button.set_anchors_preset(Control.PRESET_CENTER_TOP)
	tuning_button.position = Vector2(390, 56)
	tuning_button.custom_minimum_size = Vector2(155, 36)
	tuning_button.text = "Physics tuning  [P]"
	tuning_button.tooltip_text = "Tune the native atmospheric solver while it is running."
	tuning_button.pressed.connect(_toggle_tuning_panel)
	add_child(tuning_button)

	tuning_panel = PanelContainer.new()
	tuning_panel.set_anchor(SIDE_LEFT, 1.0)
	tuning_panel.set_anchor(SIDE_RIGHT, 1.0)
	tuning_panel.set_anchor(SIDE_TOP, 0.5)
	tuning_panel.set_anchor(SIDE_BOTTOM, 0.5)
	tuning_panel.offset_left = -350.0
	tuning_panel.offset_right = -18.0
	tuning_panel.offset_top = -345.0
	tuning_panel.offset_bottom = 345.0
	var panel_style := StyleBoxFlat.new()
	panel_style.bg_color = Color(0.018, 0.029, 0.047, 0.97)
	panel_style.border_color = Color(0.24, 0.39, 0.52, 0.85)
	panel_style.set_border_width_all(1)
	panel_style.corner_radius_top_left = 7
	panel_style.corner_radius_top_right = 7
	panel_style.corner_radius_bottom_left = 7
	panel_style.corner_radius_bottom_right = 7
	tuning_panel.add_theme_stylebox_override("panel", panel_style)
	add_child(tuning_panel)

	var margin := MarginContainer.new()
	margin.add_theme_constant_override("margin_left", 14)
	margin.add_theme_constant_override("margin_right", 14)
	margin.add_theme_constant_override("margin_top", 12)
	margin.add_theme_constant_override("margin_bottom", 12)
	tuning_panel.add_child(margin)
	var column := VBoxContainer.new()
	column.add_theme_constant_override("separation", 5)
	margin.add_child(column)

	var heading := Label.new()
	heading.text = "LIVE PHYSICS CALIBRATION"
	heading.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	heading.add_theme_font_size_override("font_size", 16)
	column.add_child(heading)
	var explanation := Label.new()
	explanation.text = "0× disables a parameterized process; 1× is the Earth-like baseline.\nChanges affect subsequent native solver steps."
	explanation.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	explanation.add_theme_font_size_override("font_size", 11)
	explanation.add_theme_color_override("font_color", Color(0.66, 0.74, 0.82))
	column.add_child(explanation)

	for tuning_key: StringName in WeatherSystem.TUNING_KEYS:
		var row := HBoxContainer.new()
		var label := Label.new()
		label.custom_minimum_size = Vector2(118, 22)
		label.text = TUNING_LABELS[tuning_key]
		label.tooltip_text = TUNING_TOOLTIPS[tuning_key]
		label.add_theme_font_size_override("font_size", 11)
		row.add_child(label)
		var slider := HSlider.new()
		slider.custom_minimum_size = Vector2(130, 22)
		slider.min_value = WeatherSystem.TUNING_WEIGHT_MIN
		slider.max_value = WeatherSystem.TUNING_WEIGHT_MAX
		slider.step = 0.05
		slider.value = float(WeatherSystem.tuning_weights[tuning_key])
		slider.tooltip_text = TUNING_TOOLTIPS[tuning_key]
		slider.value_changed.connect(_on_tuning_slider_changed.bind(tuning_key))
		row.add_child(slider)
		var value_label := Label.new()
		value_label.custom_minimum_size = Vector2(44, 22)
		value_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
		value_label.text = "%.2f×" % slider.value
		value_label.add_theme_font_size_override("font_size", 11)
		row.add_child(value_label)
		_tuning_sliders[tuning_key] = slider
		_tuning_value_labels[tuning_key] = value_label
		column.add_child(row)

	column.add_child(HSeparator.new())
	var layer_heading := Label.new()
	layer_heading.text = "VERTICAL COLUMN CONTRIBUTION"
	layer_heading.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	layer_heading.tooltip_text = "Pressure-thickness weights used to integrate cloud condensate through the six levels."
	layer_heading.add_theme_font_size_override("font_size", 12)
	column.add_child(layer_heading)

	for layer in WeatherSystem.LAYERS:
		var row := HBoxContainer.new()
		var label := Label.new()
		label.custom_minimum_size = Vector2(118, 20)
		label.text = LAYER_LABELS[layer]
		label.add_theme_font_size_override("font_size", 11)
		row.add_child(label)
		var slider := HSlider.new()
		slider.custom_minimum_size = Vector2(130, 20)
		slider.min_value = WeatherSystem.TUNING_WEIGHT_MIN
		slider.max_value = WeatherSystem.TUNING_WEIGHT_MAX
		slider.step = 0.05
		slider.value = WeatherSystem.layer_weights[layer]
		slider.value_changed.connect(_on_layer_slider_changed.bind(layer))
		row.add_child(slider)
		var value_label := Label.new()
		value_label.custom_minimum_size = Vector2(44, 20)
		value_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
		value_label.text = "%.2f×" % slider.value
		value_label.add_theme_font_size_override("font_size", 11)
		row.add_child(value_label)
		_layer_sliders.append(slider)
		_layer_value_labels.append(value_label)
		column.add_child(row)

	var buttons := HBoxContainer.new()
	buttons.alignment = BoxContainer.ALIGNMENT_CENTER
	var reset := Button.new()
	reset.text = "Reset Earth-like defaults"
	reset.pressed.connect(WeatherSystem.reset_physics_tuning)
	buttons.add_child(reset)
	var close_button := Button.new()
	close_button.text = "Close"
	close_button.pressed.connect(_toggle_tuning_panel)
	buttons.add_child(close_button)
	column.add_child(buttons)
	tuning_panel.visible = false


func _input(event: InputEvent) -> void:
	if not (event is InputEventKey):
		return
	var key := event as InputEventKey
	if not key.pressed or key.echo:
		return

	if key.unicode == 233 or key.unicode == 201:
		toggle()
		get_viewport().set_input_as_handled()
		return
	if not visible:
		return

	match key.keycode:
		KEY_ESCAPE:
			if tuning_panel != null and tuning_panel.visible:
				tuning_panel.visible = false
			else:
				close()
		KEY_P:
			_toggle_tuning_panel()
		KEY_LEFT, KEY_COMMA:
			cycle(-1)
		KEY_RIGHT, KEY_PERIOD:
			cycle(1)
		KEY_1:
			set_product(Product.COMPOSITE)
		KEY_2:
			set_product(Product.SYNOPTIC)
		KEY_3:
			set_product(Product.CLOUD_COVER)
		KEY_4:
			set_product(Product.PRECIPITATION)
		KEY_5:
			set_product(Product.CONVECTION)
		KEY_6:
			set_product(Product.PRESSURE)
		KEY_7:
			set_product(Product.AIR_TEMPERATURE)
		KEY_8:
			set_product(Product.SEA_TEMPERATURE)
		KEY_9:
			set_product(Product.CAPE)
		_:
			return
	get_viewport().set_input_as_handled()


func _process(delta: float) -> void:
	if not visible:
		return
	_update_warp_indicator()
	_update_system_analysis(delta)
	_try_bind_player()
	if _player != null and is_instance_valid(_player):
		_player_dir = _player.up_dir()
		marker.queue_redraw()
	_bind_weather_texture()
	_update_base_map()


func toggle() -> void:
	if visible:
		close()
	else:
		open()


func open() -> void:
	visible = true
	_try_bind_player()
	_close_planet_map_if_needed()
	_set_player_locked(true)
	_bind_weather_texture()
	_apply_product()
	_update_base_map()
	marker.queue_redraw()


func close() -> void:
	visible = false
	_set_player_locked(false)


func cycle(step: int) -> void:
	set_product(wrapi(product_index + step, 0, PRODUCT_NAMES.size()))


func set_product(which: int) -> void:
	product_index = clampi(which, 0, PRODUCT_NAMES.size() - 1)
	if product_select != null:
		product_select.select(product_index)
	_apply_product()


func _on_product_selected(index: int) -> void:
	set_product(index)


func _on_simulation_weight_changed(value: float) -> void:
	WeatherSystem.set_simulation_weight(value)
	_update_simulation_weight_label(WeatherSystem.simulation_weight)


func _on_external_simulation_weight_changed(value: float) -> void:
	if simulation_weight_slider != null:
		simulation_weight_slider.set_value_no_signal(value)
	_update_simulation_weight_label(value)


func _update_simulation_weight_label(value: float) -> void:
	if simulation_weight_label == null:
		return
	var mode := "calibrated"
	if value < 0.01:
		mode = "neutral"
	elif value < 0.95:
		mode = "reduced"
	elif value > 1.05:
		mode = "amplified"
	simulation_weight_label.text = "Simulation influence: %.2f×  (%s)" % [value, mode]


func _on_simulation_speed_slider_changed(slider_value: float) -> void:
	WeatherSystem.set_simulation_speed(_slider_to_speed(slider_value))
	_update_simulation_speed_label(WeatherSystem.simulation_speed)
	_update_product_note()


func _on_external_simulation_speed_changed(value: float) -> void:
	if simulation_speed_slider != null:
		simulation_speed_slider.set_value_no_signal(_speed_to_slider(value))
	_update_simulation_speed_label(value)
	_update_warp_indicator()
	_update_product_note()


func _update_simulation_speed_label(value: float) -> void:
	if simulation_speed_label == null:
		return
	var mode := "normal"
	if value < 0.01:
		mode = "paused"
	elif value < 0.95:
		mode = "slow"
	elif value >= 16.0:
		mode = "spin-up"
	elif value > 1.05:
		mode = "fast"
	if value > WeatherSystem.HIGH_WARP_LOCAL_THRESHOLD:
		mode = "global spin-up"
	var speed_text := "%.2f" % value if value < 10.0 else "%.0f" % value
	simulation_speed_label.text = "Weather speed: %s×  (%s)" % [speed_text, mode]


func _slider_to_speed(slider_value: float) -> float:
	if slider_value < 0.5:
		return 0.0
	return pow(2.0, slider_value - 1.0)


func _speed_to_slider(speed: float) -> float:
	if speed < 0.5:
		return 0.0
	return clampf(log(speed) / log(2.0) + 1.0, 1.0, 14.0)


func _compact_duration(seconds: float) -> String:
	var whole := maxi(int(round(seconds)), 0)
	if whole < 60:
		return "%ds" % whole
	if whole < 3600:
		return "%dm %02ds" % [whole / 60, whole % 60]
	if whole < 86400:
		return "%dh %02dm" % [whole / 3600, (whole % 3600) / 60]
	return "%dd %02dh" % [whole / 86400, (whole % 86400) / 3600]


func _update_warp_indicator() -> void:
	if warp_indicator_label == null:
		return
	var speed := WeatherSystem.simulation_speed
	var rate := _compact_duration(speed)
	var total := _compact_duration(WeatherSystem.warped_ahead_seconds)
	warp_indicator_label.text = "1s → %s\nΣ +%s" % [rate, total]
	if WeatherSystem.solver_backlog_seconds() > 360.0:
		warp_indicator_label.add_theme_color_override("font_color", Color(1.0, 0.68, 0.22))
		warp_indicator_label.tooltip_text = "The global solver is catching up: %s queued." % _compact_duration(
			WeatherSystem.solver_backlog_seconds())
	else:
		warp_indicator_label.add_theme_color_override("font_color", Color(0.48, 0.86, 0.94))
		warp_indicator_label.tooltip_text = "Top: simulated duration advanced per real second. Bottom: cumulative simulated time gained beyond wall-clock time this session."


func _toggle_tuning_panel() -> void:
	if tuning_panel == null:
		return
	tuning_panel.visible = not tuning_panel.visible
	if tuning_panel.visible:
		_refresh_tuning_controls()


func _on_tuning_slider_changed(value: float, tuning_key: StringName) -> void:
	WeatherSystem.set_tuning_weight(tuning_key, value)
	_on_external_tuning_weight_changed(tuning_key, float(WeatherSystem.tuning_weights[tuning_key]))


func _on_layer_slider_changed(value: float, layer: int) -> void:
	WeatherSystem.set_layer_weight(layer, value)
	_on_external_layer_weight_changed(layer, WeatherSystem.layer_weights[layer])


func _on_external_tuning_weight_changed(tuning_key: StringName, value: float) -> void:
	var slider: HSlider = _tuning_sliders.get(tuning_key)
	if slider != null:
		slider.set_value_no_signal(value)
	var value_label: Label = _tuning_value_labels.get(tuning_key)
	if value_label != null:
		value_label.text = "%.2f×" % value


func _on_external_layer_weight_changed(layer: int, value: float) -> void:
	if layer < 0 or layer >= _layer_sliders.size():
		return
	_layer_sliders[layer].set_value_no_signal(value)
	_layer_value_labels[layer].text = "%.2f×" % value


func _refresh_tuning_controls() -> void:
	for tuning_key: StringName in WeatherSystem.TUNING_KEYS:
		_on_external_tuning_weight_changed(tuning_key, float(WeatherSystem.tuning_weights[tuning_key]))
	for layer in WeatherSystem.layer_weights.size():
		_on_external_layer_weight_changed(layer, WeatherSystem.layer_weights[layer])


func _apply_product() -> void:
	if _material != null:
		_material.set_shader_parameter("u_product", product_index)
	if title != null:
		title.text = "ASTERRA METEOROLOGY — %s" % PRODUCT_NAMES[product_index]
	_update_product_note()
	if legend_texture != null:
		legend_texture.texture = _make_legend_texture(product_index)
	if legend_left != null:
		legend_left.text = LEGEND_LEFT[product_index]
	if legend_right != null:
		legend_right.text = LEGEND_RIGHT[product_index]
	_system_scan_accum = 1.0
	if marker != null:
		marker.queue_redraw()


func _update_product_note() -> void:
	if note != null:
		var resolution_note := "global spin-up" if WeatherSystem.simulation_speed \
			> WeatherSystem.HIGH_WARP_LOCAL_THRESHOLD else "local nest %.0f km" % \
			(WeatherSystem.local_span_m / 1000.0)
		note.text = "%s  •  %d layers  •  %s" % [
			PRODUCT_NOTES[product_index], WeatherSystem.layer_count, resolution_note]


func _bind_weather_texture() -> void:
	if _material == null or WeatherSystem.global_weather_texture == null:
		return
	_material.set_shader_parameter("u_weather", WeatherSystem.global_weather_texture)
	if WeatherSystem.local_weather_texture != null:
		_material.set_shader_parameter("u_local_weather", WeatherSystem.local_weather_texture)
	if WeatherSystem.global_diagnostics_texture != null:
		_material.set_shader_parameter("u_diagnostics", WeatherSystem.global_diagnostics_texture)
	if WeatherSystem.local_diagnostics_texture != null:
		_material.set_shader_parameter("u_local_diagnostics", WeatherSystem.local_diagnostics_texture)
	if WeatherSystem.global_products_texture != null:
		_material.set_shader_parameter("u_products", WeatherSystem.global_products_texture)
	if WeatherSystem.local_products_texture != null:
		_material.set_shader_parameter("u_local_products", WeatherSystem.local_products_texture)
	_material.set_shader_parameter("u_local_center", WeatherSystem.local_center)
	_material.set_shader_parameter("u_local_east", WeatherSystem.local_east)
	_material.set_shader_parameter("u_local_north", WeatherSystem.local_north)
	_material.set_shader_parameter(
		"u_local_span_m", 0.0 if WeatherSystem.simulation_speed \
		> WeatherSystem.HIGH_WARP_LOCAL_THRESHOLD else WeatherSystem.local_span_m)
	if Planet.cfg != null:
		_material.set_shader_parameter("u_planet_radius", Planet.cfg.planet_radius)


func _placeholder_texture() -> ImageTexture:
	var image := Image.create(1, 1, false, Image.FORMAT_RGB8)
	image.set_pixel(0, 0, Color(0.025, 0.045, 0.075))
	return ImageTexture.create_from_image(image)


func _update_base_map() -> void:
	if not Planet.ready_state or Planet.fields == null or Planet.grid == null:
		return
	if _base_fields != Planet.fields:
		_begin_base_render()
	if _base_render_y < 0:
		return

	var started := Time.get_ticks_usec()
	while _base_render_y < H:
		_render_base_row(_base_render_y)
		_base_render_y += 1
		if Time.get_ticks_usec() - started >= BASE_RENDER_BUDGET_USEC:
			break

	if _base_render_y >= H:
		_base_texture = ImageTexture.create_from_image(_base_image)
		texture_rect.texture = _base_texture
		_base_render_y = -1


func _begin_base_render() -> void:
	_base_fields = Planet.fields
	_base_grid = Planet.grid
	_base_image = Image.create(W, H, false, Image.FORMAT_RGB8)
	_base_render_y = 0


func _render_base_row(y: int) -> void:
	if _base_fields == null or _base_grid == null:
		_base_render_y = -1
		return
	var lat := (0.5 - float(y) / float(H)) * PI
	for x in W:
		var lon := (float(x) / float(W) - 0.5) * TAU
		var d := CubeSphere.latlon_to_dir(lat, lon)
		var c := _base_grid.dir_to_index(d)
		var h := _base_fields.elev[c]
		if h < 0.0:
			var depth := clampf(-h / 5000.0, 0.0, 1.0)
			_base_image.set_pixel(x, y, Color(0.025, 0.075, 0.135).lerp(
				Color(0.07, 0.20, 0.31), 1.0 - depth))
		else:
			var biome_color: Color = PlanetFields.BIOME_COLORS[_base_fields.biome[c]]
			var mountain := clampf(h / 4500.0, 0.0, 1.0)
			var land := biome_color.darkened(0.52).lerp(Color(0.46, 0.43, 0.39), mountain * 0.50)
			_base_image.set_pixel(x, y, land)


func _try_bind_player() -> void:
	if _player != null and is_instance_valid(_player):
		return
	var root := get_tree().current_scene
	if root == null:
		return
	for child in root.get_children():
		if child is AsterraPlayer:
			_player = child as AsterraPlayer
			return


func _set_player_locked(locked: bool) -> void:
	_try_bind_player()
	if _player == null or not is_instance_valid(_player):
		return
	if locked:
		_player.set_mouse_captured(false)
		_player.input_enabled = false
		return

	var root := get_tree().current_scene
	if root != null:
		for child in root.get_children():
			if child is DebugMenu and child.visible:
				return
			if child is PlanetMap and child.visible:
				return
	_player.set_mouse_captured(true)
	_player.input_enabled = true


func _close_planet_map_if_needed() -> void:
	var root := get_tree().current_scene
	if root == null:
		return
	for child in root.get_children():
		if child is PlanetMap and child.visible:
			child.toggle()
			return


func _update_system_analysis(delta: float) -> void:
	if product_index != Product.SYNOPTIC:
		if not _systems.is_empty():
			_systems.clear()
			marker.queue_redraw()
		return
	_system_scan_accum += delta
	if _system_scan_accum < 0.75:
		return
	_system_scan_accum = 0.0
	var weather := WeatherSystem.global_weather_values
	var diagnostics := WeatherSystem.global_diagnostics_values
	var products := WeatherSystem.global_products_values
	var expected := WeatherSystem.GLOBAL_W * WeatherSystem.GLOBAL_H * 4
	if weather.size() != expected or diagnostics.size() != expected or products.size() != expected:
		return

	var candidates: Array[Dictionary] = []
	for y in range(6, WeatherSystem.GLOBAL_H - 6, 6):
		for x in range(0, WeatherSystem.GLOBAL_W, 6):
			var cell := x + y * WeatherSystem.GLOBAL_W
			var offset := cell * 4
			var pressure := weather[offset + 3]
			var is_low := pressure < 0.475
			var is_high := pressure > 0.525
			if not is_low and not is_high:
				continue
			var is_extreme := true
			for oy in range(-6, 7, 3):
				for ox in range(-6, 7, 3):
					if ox == 0 and oy == 0:
						continue
					var nx := wrapi(x + ox, 0, WeatherSystem.GLOBAL_W)
					var ny := clampi(y + oy, 0, WeatherSystem.GLOBAL_H - 1)
					var neighbour := weather[(nx + ny * WeatherSystem.GLOBAL_W) * 4 + 3]
					if (is_low and neighbour < pressure) or (is_high and neighbour > pressure):
						is_extreme = false
						break
				if not is_extreme:
					break
			if not is_extreme:
				continue

			var latitude_deg := 90.0 - 180.0 * (float(y) + 0.5) / float(WeatherSystem.GLOBAL_H)
			var storm := weather[offset + 1]
			var rain := weather[offset + 2]
			var rotation := absf(diagnostics[offset] - 0.5) * 2.0
			var sea_temperature := products[offset + 1]
			var cape := products[offset + 2]
			var system_name := "ANTICYCLONE" if is_high else "DEPRESSION"
			if is_low and absf(latitude_deg) < 35.0 and sea_temperature > 0.35 \
					and cape > 0.15 and storm > 0.10 and rain > 0.015 and rotation > 0.12:
				system_name = "HURRICANE / TYPHOON"
			elif is_low and storm > 0.11 and rotation > 0.10:
				system_name = "STORM LOW"
			var strength := absf(pressure - 0.5) * 5.0 + storm + cape * 0.08
			candidates.append({
				"x": x, "y": y, "low": is_low, "name": system_name,
				"pressure": pressure, "strength": strength,
			})

	candidates.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		return float(a.strength) > float(b.strength))
	_systems.clear()
	for candidate in candidates:
		var map_x := fposmod(float(candidate.x) / float(WeatherSystem.GLOBAL_W) + 0.5, 1.0) * W
		var map_y := (float(candidate.y) + 0.5) / float(WeatherSystem.GLOBAL_H) * H
		var separated := true
		for accepted in _systems:
			var accepted_x := float(accepted.map_x)
			var dx := absf(map_x - accepted_x)
			dx = minf(dx, float(W) - dx)
			if Vector2(dx, map_y - float(accepted.map_y)).length() < 82.0:
				separated = false
				break
		if not separated:
			continue
		candidate["map_x"] = map_x
		candidate["map_y"] = map_y
		_systems.append(candidate)
		if _systems.size() >= 10:
			break
	marker.queue_redraw()


func _draw_marker() -> void:
	if product_index == Product.SYNOPTIC:
		var font := marker.get_theme_default_font()
		for system in _systems:
			var position := Vector2(
				float(system.map_x) - float(W) * 0.5,
				float(system.map_y) - float(H) * 0.5)
			var low := bool(system.low)
			var color := Color(0.96, 0.24, 0.16, 0.96) if low \
				else Color(0.18, 0.48, 1.0, 0.96)
			marker.draw_circle(position, 14.0, Color(0.015, 0.025, 0.045, 0.82))
			marker.draw_circle(position, 14.0, color, false, 2.2, true)
			marker.draw_string(font, position + Vector2(-12, 7), "L" if low else "H",
				HORIZONTAL_ALIGNMENT_CENTER, 24.0, 18, color)
			var name := String(system.name)
			marker.draw_string(font, position + Vector2(-66, -19), name,
				HORIZONTAL_ALIGNMENT_CENTER, 132.0, 10, Color(0.96, 0.98, 1.0, 0.96))
			var anomaly_hpa := (float(system.pressure) - 0.5) * 120.0
			marker.draw_string(font, position + Vector2(-40, 27), "%+.1f hPa" % anomaly_hpa,
				HORIZONTAL_ALIGNMENT_CENTER, 80.0, 9, Color(0.78, 0.84, 0.90, 0.90))

	var latlon := CubeSphere.dir_to_latlon(_player_dir)
	var x := (latlon.y / TAU + 0.5) * float(W) - float(W) * 0.5
	var y := (0.5 - latlon.x / PI) * float(H) - float(H) * 0.5

	if Planet.cfg != null:
		var half_angle := (WeatherSystem.local_span_m * 0.5) / maxf(Planet.cfg.planet_radius, 1.0)
		var cos_lat := maxf(absf(cos(latlon.x)), 0.18)
		var rx := minf(half_angle / (TAU * cos_lat) * float(W), float(W) * 0.42)
		var ry := half_angle / PI * float(H)
		var points := PackedVector2Array()
		for i in 49:
			var a := TAU * float(i) / 48.0
			points.append(Vector2(x + cos(a) * rx, y + sin(a) * ry))
		marker.draw_polyline(points, Color(0.18, 0.82, 0.92, 0.72), 1.3, true)

	marker.draw_circle(Vector2(x, y), 5.0, Color(1.0, 0.22, 0.12, 0.95))
	marker.draw_circle(Vector2(x, y), 9.0, Color(1.0, 1.0, 1.0, 0.42), false, 1.5)


func _make_legend_texture(which: int) -> GradientTexture1D:
	var gradient := Gradient.new()
	match which:
		Product.SYNOPTIC:
			gradient.offsets = PackedFloat32Array([0.0, 0.5, 1.0])
			gradient.colors = PackedColorArray([
				Color(0.10, 0.28, 0.88), Color(0.92, 0.92, 0.88), Color(0.88, 0.16, 0.10)])
		Product.CLOUD_COVER:
			gradient.offsets = PackedFloat32Array([0.0, 0.55, 1.0])
			gradient.colors = PackedColorArray([
				Color(0.035, 0.10, 0.19), Color(0.48, 0.59, 0.68), Color(0.96, 0.98, 1.0)])
		Product.PRECIPITATION:
			gradient.offsets = PackedFloat32Array([0.0, 0.10, 0.35, 0.62, 0.82, 1.0])
			gradient.colors = PackedColorArray([
				Color(0.02, 0.08, 0.13), Color(0.05, 0.42, 0.72), Color(0.08, 0.78, 0.36),
				Color(0.98, 0.86, 0.16), Color(0.92, 0.18, 0.09), Color(0.78, 0.10, 0.82)])
		Product.CONVECTION:
			gradient.offsets = PackedFloat32Array([0.0, 0.25, 0.55, 0.78, 1.0])
			gradient.colors = PackedColorArray([
				Color(0.035, 0.06, 0.12), Color(0.12, 0.34, 0.72), Color(1.0, 0.82, 0.12),
				Color(0.98, 0.18, 0.06), Color(0.88, 0.08, 0.78)])
		Product.PRESSURE:
			gradient.offsets = PackedFloat32Array([0.0, 0.5, 1.0])
			gradient.colors = PackedColorArray([
				Color(0.10, 0.28, 0.88), Color(0.92, 0.92, 0.88), Color(0.88, 0.16, 0.10)])
		Product.AIR_TEMPERATURE, Product.SEA_TEMPERATURE:
			gradient.offsets = PackedFloat32Array([0.0, 0.25, 0.50, 0.72, 1.0])
			gradient.colors = PackedColorArray([
				Color(0.22, 0.08, 0.55), Color(0.12, 0.48, 0.92), Color(0.18, 0.82, 0.62),
				Color(1.0, 0.82, 0.16), Color(0.82, 0.06, 0.08)])
		Product.CAPE:
			gradient.offsets = PackedFloat32Array([0.0, 0.25, 0.60, 1.0])
			gradient.colors = PackedColorArray([
				Color(0.04, 0.11, 0.20), Color(0.12, 0.62, 0.48),
				Color(0.98, 0.80, 0.12), Color(0.88, 0.04, 0.30)])
		Product.IRRADIANCE:
			gradient.offsets = PackedFloat32Array([0.0, 0.45, 1.0])
			gradient.colors = PackedColorArray([
				Color(0.015, 0.025, 0.09), Color(0.18, 0.42, 0.82), Color(1.0, 0.90, 0.30)])
		Product.VORTICITY, Product.DIVERGENCE:
			gradient.offsets = PackedFloat32Array([0.0, 0.5, 1.0])
			gradient.colors = PackedColorArray([
				Color(0.10, 0.36, 0.92), Color(0.88, 0.90, 0.90), Color(0.92, 0.16, 0.12)])
		Product.POTENTIAL_VORTICITY:
			gradient.offsets = PackedFloat32Array([0.0, 0.5, 1.0])
			gradient.colors = PackedColorArray([
				Color(0.20, 0.20, 0.70), Color(0.90, 0.90, 0.86), Color(0.72, 0.08, 0.72)])
		Product.WIND_SHEAR:
			gradient.offsets = PackedFloat32Array([0.0, 0.35, 0.68, 1.0])
			gradient.colors = PackedColorArray([
				Color(0.03, 0.10, 0.16), Color(0.10, 0.62, 0.54), Color(0.96, 0.82, 0.14), Color(0.90, 0.10, 0.12)])
		_:
			gradient.offsets = PackedFloat32Array([0.0, 0.32, 0.62, 0.82, 1.0])
			gradient.colors = PackedColorArray([
				Color(0.035, 0.10, 0.19), Color(0.70, 0.76, 0.78), Color(0.08, 0.78, 0.36),
				Color(0.98, 0.40, 0.08), Color(0.88, 0.08, 0.78)])
	var tex := GradientTexture1D.new()
	tex.gradient = gradient
	tex.width = 256
	return tex
