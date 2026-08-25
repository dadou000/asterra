extends CanvasLayer
## Live whole-planet meteorological map driven by WeatherSystem.
##
## `é` toggles the map on French AZERTY layouts. The display consumes the same
## 256x128 AVX2 weather field that drives the volumetric cloud renderer, so the
## synoptic map and the sky cannot disagree about storm placement.

enum Product {
	COMPOSITE,
	CLOUD_COVER,
	PRECIPITATION,
	CONVECTION,
	PRESSURE,
}

const PRODUCT_NAMES := [
	"Composite weather",
	"Cloud cover",
	"Precipitation",
	"Convective storm intensity",
	"Surface pressure anomaly",
]
const PRODUCT_NOTES := [
	"Cloud shield + precipitation + severe convective cores",
	"Column cloud/coverage potential used by the global cloud field",
	"Current model precipitation intensity",
	"CAPE + rotation/convergence storm signal",
	"Pressure perturbation around the planetary mean sea-level state",
]
const LEGEND_LEFT := ["Clear", "0 %", "None", "Stable", "-55 hPa"]
const LEGEND_RIGHT := ["Severe", "100 %", "Intense", "Severe", "+55 hPa"]

const W := 960
const H := 480
const BASE_RENDER_BUDGET_USEC := 2400

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
var _material: ShaderMaterial
var _player: AsterraPlayer
var _player_dir := Vector3(1, 0, 0)

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
	product_select.position = Vector2(-170, 56)
	product_select.custom_minimum_size = Vector2(340, 36)
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
	legend_left.custom_minimum_size = Vector2(160, 20)
	legend_left.add_theme_font_size_override("font_size", 11)
	add_child(legend_left)

	legend_right = Label.new()
	legend_right.set_anchors_preset(Control.PRESET_CENTER_BOTTOM)
	legend_right.position = Vector2(50, -38)
	legend_right.custom_minimum_size = Vector2(160, 20)
	legend_right.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	legend_right.add_theme_font_size_override("font_size", 11)
	add_child(legend_right)

	var hint := Label.new()
	hint.set_anchors_preset(Control.PRESET_CENTER_BOTTOM)
	hint.position = Vector2(-float(W) * 0.5, -18)
	hint.custom_minimum_size = Vector2(W, 18)
	hint.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	hint.text = "é close  •  ← / → change product  •  1–5 direct  •  weight affects map, clouds and shadows"
	hint.add_theme_font_size_override("font_size", 11)
	hint.add_theme_color_override("font_color", Color(0.52, 0.59, 0.69))
	add_child(hint)

	marker = Control.new()
	marker.set_anchors_preset(Control.PRESET_CENTER)
	marker.draw.connect(_draw_marker)
	add_child(marker)

	_apply_product()


func _input(event: InputEvent) -> void:
	if not (event is InputEventKey):
		return
	var key := event as InputEventKey
	if not key.pressed or key.echo:
		return

	# Printable-key matching is layout-correct: on French AZERTY this is the `é`
	# key rather than the physical US keyboard position.
	if key.unicode == 233 or key.unicode == 201:
		toggle()
		get_viewport().set_input_as_handled()
		return

	if not visible:
		return

	match key.keycode:
		KEY_ESCAPE:
			close()
		KEY_LEFT, KEY_COMMA:
			cycle(-1)
		KEY_RIGHT, KEY_PERIOD:
			cycle(1)
		KEY_1:
			set_product(Product.COMPOSITE)
		KEY_2:
			set_product(Product.CLOUD_COVER)
		KEY_3:
			set_product(Product.PRECIPITATION)
		KEY_4:
			set_product(Product.CONVECTION)
		KEY_5:
			set_product(Product.PRESSURE)
		_:
			return
	get_viewport().set_input_as_handled()


func _process(_delta: float) -> void:
	if not visible:
		return
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


func _apply_product() -> void:
	if _material != null:
		_material.set_shader_parameter("u_product", product_index)
	if title != null:
		title.text = "ASTERRA METEOROLOGY — %s" % PRODUCT_NAMES[product_index]
	if note != null:
		note.text = "%s  •  local nest %.0f km wide" % [
			PRODUCT_NOTES[product_index], WeatherSystem.local_span_m / 1000.0]
	if legend_texture != null:
		legend_texture.texture = _make_legend_texture(product_index)
	if legend_left != null:
		legend_left.text = LEGEND_LEFT[product_index]
	if legend_right != null:
		legend_right.text = LEGEND_RIGHT[product_index]


func _bind_weather_texture() -> void:
	if _material == null or WeatherSystem.global_weather_texture == null:
		return
	_material.set_shader_parameter("u_weather", WeatherSystem.global_weather_texture)


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

	# Do not steal control back from another modal overlay that is still open.
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


func _draw_marker() -> void:
	var latlon := CubeSphere.dir_to_latlon(_player_dir)
	var x := (latlon.y / TAU + 0.5) * float(W) - float(W) * 0.5
	var y := (0.5 - latlon.x / PI) * float(H) - float(H) * 0.5

	# The cyan ellipse is the current mesoscale refinement footprint. It is an
	# equirectangular approximation of the 422 km tangent-plane nest.
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
		_:
			gradient.offsets = PackedFloat32Array([0.0, 0.32, 0.62, 0.82, 1.0])
			gradient.colors = PackedColorArray([
				Color(0.035, 0.10, 0.19), Color(0.70, 0.76, 0.78), Color(0.08, 0.78, 0.36),
				Color(0.98, 0.40, 0.08), Color(0.88, 0.08, 0.78)])
	var tex := GradientTexture1D.new()
	tex.gradient = gradient
	tex.width = 256
	return tex
