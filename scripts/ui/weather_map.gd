extends CanvasLayer
## Interactive whole-planet weather globe.
##
## `é` toggles the viewer. Drag the globe to rotate it and use the wheel to zoom.
## Weather scalar products come directly from WeatherSystem. The wind overlay uses
## the native per-layer vector field when the backend exposes it; until an older
## DLL is rebuilt it falls back to the baked prevailing-wind field so the map
## remains usable instead of failing to open.

enum Product {
	WIND,
	COMPOSITE,
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
	"Wind",
	"Composite weather",
	"Cloud cover",
	"Precipitation",
	"Organised storm intensity",
	"Surface pressure anomaly",
	"Near-surface air temperature",
	"Sea-surface temperature",
	"CAPE",
	"Absorbed solar irradiance",
	"Relative vorticity",
	"Low-level divergence",
	"Potential-vorticity proxy",
	"Vertical wind shear",
]
const PRODUCT_NOTES := [
	"Horizontal wind speed and animated flow",
	"Clouds, precipitation and organised convection",
	"Vertically integrated liquid/ice condensate",
	"Current model precipitation intensity",
	"Resolved ascent + instability + rotation + shear",
	"Boundary-layer pressure perturbation",
	"Lowest model-layer air temperature",
	"Live ocean mixed-layer temperature",
	"Convective available potential-energy proxy",
	"Surface short-wave power after clouds and terrain shading",
	"Strongest signed low/mid-level relative vorticity",
	"Signed low-level divergence; negative means convergence",
	"Upper-level potential-vorticity proxy",
	"Maximum adjacent-layer vector wind shear",
]
const LEGEND_LEFT := [
	"0 km/h", "Clear", "0 %", "None", "Stable", "Low",
	"Cold", "Cold", "0 J/kg", "0 W/m²", "Anticyclonic", "Convergence", "Negative", "0 m/s",
]
const LEGEND_RIGHT := [
	"235+ km/h", "Severe", "100 %", "Intense", "Organised", "High",
	"Hot", "Hot", "4000+ J/kg", "1200+ W/m²", "Cyclonic", "Divergence", "Positive", "55+ m/s",
]
const LAYER_NAMES := [
	"L1  ~0.45 km",
	"L2  ~1.7 km",
	"L3  ~3.3 km",
	"L4  ~5.6 km",
	"L5  ~8.5 km",
	"L6  ~12.8 km",
]
const FALLBACK_LAYER_SPEED := [1.0, 1.22, 1.48, 1.78, 2.08, 2.34]

const GLOBAL_W := 256
const GLOBAL_H := 128
const PARTICLE_COUNT := 32768
const PLANET_RADIUS_M := 3500000.0
const FIELD_BUILD_BUDGET_USEC := 2600
const LIVE_WIND_REFRESH_S := 0.50

var product_index := Product.WIND
var wind_layer := 0

var _viewport_container: SubViewportContainer
var _subviewport: SubViewport
var _world_root: Node3D
var _camera: Camera3D
var _globe: MeshInstance3D
var _globe_material: ShaderMaterial
var _wind_particles: MultiMeshInstance3D
var _wind_material: ShaderMaterial

var _panel: PanelContainer
var _product_select: OptionButton
var _layer_select: OptionButton
var _flow_check: CheckButton
var _source_label: Label
var _note_label: Label
var _cursor_label: Label
var _legend: TextureRect
var _legend_left: Label
var _legend_right: Label

var _camera_yaw := 0.35
var _camera_pitch := 0.16
var _camera_distance := 2.72
var _angular_velocity := Vector2.ZERO
var _dragging := false

var _base_image: Image
var _base_texture: ImageTexture
var _wind_image: Image
var _wind_texture: ImageTexture
var _wind_values := PackedFloat32Array()
var _field_build_y := -1
var _base_ready := false
var _wind_fallback_ready := false
var _live_wind_available := false
var _live_wind_accum := 999.0
var _last_viewport_size := Vector2i.ZERO


func _ready() -> void:
	layer = 30
	visible = false
	process_mode = Node.PROCESS_MODE_ALWAYS
	_create_placeholder_textures()
	_build_viewer()
	_build_ui()
	_sync_weather_textures()
	_update_product()
	_update_camera()


func _build_viewer() -> void:
	var background := ColorRect.new()
	background.color = Color(0.0015, 0.0025, 0.0055, 1.0)
	background.set_anchors_preset(Control.PRESET_FULL_RECT)
	background.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(background)

	_viewport_container = SubViewportContainer.new()
	_viewport_container.set_anchors_preset(Control.PRESET_FULL_RECT)
	_viewport_container.stretch = true
	_viewport_container.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(_viewport_container)

	_subviewport = SubViewport.new()
	_subviewport.size = Vector2i(1280, 720)
	_subviewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	_subviewport.transparent_bg = false
	_subviewport.msaa_3d = Viewport.MSAA_2X
	_viewport_container.add_child(_subviewport)

	_world_root = Node3D.new()
	_subviewport.add_child(_world_root)

	var world_environment := WorldEnvironment.new()
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color(0.001, 0.0015, 0.0035)
	environment.background_energy_multiplier = 0.05
	world_environment.environment = environment
	_world_root.add_child(world_environment)

	_camera = Camera3D.new()
	_camera.current = true
	_camera.fov = 42.0
	_camera.near = 0.015
	_camera.far = 20.0
	_world_root.add_child(_camera)

	_globe = MeshInstance3D.new()
	var sphere := SphereMesh.new()
	sphere.radius = 1.0
	sphere.height = 2.0
	sphere.radial_segments = 160
	sphere.rings = 96
	_globe.mesh = sphere
	_globe_material = ShaderMaterial.new()
	_globe_material.shader = load("res://shaders/weather_globe_product.gdshader")
	_globe.material_override = _globe_material
	_world_root.add_child(_globe)


func _build_ui() -> void:
	_cursor_label = Label.new()
	_cursor_label.position = Vector2(16, 16)
	_cursor_label.custom_minimum_size = Vector2(430, 54)
	_cursor_label.add_theme_font_size_override("font_size", 13)
	_cursor_label.add_theme_color_override("font_color", Color(0.92, 0.96, 1.0))
	_cursor_label.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(_cursor_label)

	var hint := Label.new()
	hint.set_anchors_preset(Control.PRESET_TOP_RIGHT)
	hint.position = Vector2(-420, 16)
	hint.custom_minimum_size = Vector2(400, 28)
	hint.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	hint.text = "LMB drag  •  wheel zoom  •  R reset  •  é close"
	hint.add_theme_font_size_override("font_size", 12)
	hint.add_theme_color_override("font_color", Color(0.62, 0.70, 0.79))
	hint.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(hint)

	_panel = PanelContainer.new()
	_panel.set_anchor(SIDE_LEFT, 0.0)
	_panel.set_anchor(SIDE_RIGHT, 0.0)
	_panel.set_anchor(SIDE_TOP, 1.0)
	_panel.set_anchor(SIDE_BOTTOM, 1.0)
	_panel.offset_left = 14.0
	_panel.offset_right = 374.0
	_panel.offset_top = -246.0
	_panel.offset_bottom = -14.0
	_panel.add_theme_stylebox_override("panel", _panel_style())
	add_child(_panel)

	var margin := MarginContainer.new()
	margin.add_theme_constant_override("margin_left", 12)
	margin.add_theme_constant_override("margin_right", 12)
	margin.add_theme_constant_override("margin_top", 10)
	margin.add_theme_constant_override("margin_bottom", 10)
	_panel.add_child(margin)

	var column := VBoxContainer.new()
	column.add_theme_constant_override("separation", 5)
	margin.add_child(column)

	var heading := Label.new()
	heading.text = "ASTERRA  •  GLOBAL WEATHER"
	heading.add_theme_font_size_override("font_size", 14)
	heading.add_theme_color_override("font_color", Color(0.91, 0.95, 1.0))
	column.add_child(heading)

	_source_label = Label.new()
	_source_label.text = "Wind source: preparing field…"
	_source_label.add_theme_font_size_override("font_size", 10)
	_source_label.add_theme_color_override("font_color", Color(0.53, 0.74, 0.83))
	column.add_child(_source_label)

	var product_row := HBoxContainer.new()
	var product_tag := Label.new()
	product_tag.text = "Product"
	product_tag.custom_minimum_size = Vector2(64, 30)
	product_row.add_child(product_tag)
	_product_select = OptionButton.new()
	_product_select.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	for name in PRODUCT_NAMES:
		_product_select.add_item(name)
	_product_select.selected = product_index
	_product_select.item_selected.connect(_on_product_selected)
	product_row.add_child(_product_select)
	column.add_child(product_row)

	var layer_row := HBoxContainer.new()
	var layer_tag := Label.new()
	layer_tag.text = "Height"
	layer_tag.custom_minimum_size = Vector2(64, 30)
	layer_row.add_child(layer_tag)
	_layer_select = OptionButton.new()
	_layer_select.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	for name in LAYER_NAMES:
		_layer_select.add_item(name)
	_layer_select.selected = wind_layer
	_layer_select.item_selected.connect(_on_layer_selected)
	layer_row.add_child(_layer_select)
	column.add_child(layer_row)

	_flow_check = CheckButton.new()
	_flow_check.text = "Animated GPU wind flow"
	_flow_check.button_pressed = true
	_flow_check.toggled.connect(_on_flow_toggled)
	column.add_child(_flow_check)

	_note_label = Label.new()
	_note_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_note_label.custom_minimum_size = Vector2(330, 34)
	_note_label.add_theme_font_size_override("font_size", 10)
	_note_label.add_theme_color_override("font_color", Color(0.67, 0.72, 0.80))
	column.add_child(_note_label)

	_legend = TextureRect.new()
	_legend.custom_minimum_size = Vector2(330, 8)
	_legend.stretch_mode = TextureRect.STRETCH_SCALE
	column.add_child(_legend)

	var legend_row := HBoxContainer.new()
	_legend_left = Label.new()
	_legend_left.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_legend_left.add_theme_font_size_override("font_size", 9)
	legend_row.add_child(_legend_left)
	_legend_right = Label.new()
	_legend_right.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_legend_right.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	_legend_right.add_theme_font_size_override("font_size", 9)
	legend_row.add_child(_legend_right)
	column.add_child(legend_row)


func _panel_style() -> StyleBoxFlat:
	var style := StyleBoxFlat.new()
	style.bg_color = Color(0.012, 0.018, 0.040, 0.94)
	style.border_color = Color(0.28, 0.38, 0.58, 0.85)
	style.set_border_width_all(1)
	style.corner_radius_top_left = 6
	style.corner_radius_top_right = 6
	style.corner_radius_bottom_left = 6
	style.corner_radius_bottom_right = 6
	return style


func _create_placeholder_textures() -> void:
	var base := Image.create(2, 1, false, Image.FORMAT_RGB8)
	base.set_pixel(0, 0, Color(0.018, 0.045, 0.10))
	base.set_pixel(1, 0, Color(0.035, 0.085, 0.09))
	_base_texture = ImageTexture.create_from_image(base)

	_wind_image = Image.create(GLOBAL_W, GLOBAL_H, false, Image.FORMAT_RGBAF)
	_wind_image.fill(Color(0.0, 0.0, 0.0, 1.0))
	_wind_texture = ImageTexture.create_from_image(_wind_image)
	_wind_values = _wind_image.get_data().to_float32_array()


func _sync_weather_textures() -> void:
	if _globe_material == null:
		return
	_globe_material.set_shader_parameter("u_base", _base_texture)
	_globe_material.set_shader_parameter("u_weather", WeatherSystem.global_weather_texture)
	_globe_material.set_shader_parameter("u_diagnostics", WeatherSystem.global_diagnostics_texture)
	_globe_material.set_shader_parameter("u_products", WeatherSystem.global_products_texture)
	_globe_material.set_shader_parameter("u_wind", _wind_texture)
	if _wind_material != null:
		_wind_material.set_shader_parameter("u_wind", _wind_texture)


func _ensure_particles() -> void:
	if _wind_particles != null:
		return

	_wind_particles = MultiMeshInstance3D.new()
	var quad := ArrayMesh.new()
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = PackedVector3Array([
		Vector3(-0.5, -0.5, 0.0), Vector3(0.5, -0.5, 0.0),
		Vector3(0.5, 0.5, 0.0), Vector3(-0.5, 0.5, 0.0),
	])
	arrays[Mesh.ARRAY_TEX_UV] = PackedVector2Array([
		Vector2(0.0, 0.0), Vector2(1.0, 0.0),
		Vector2(1.0, 1.0), Vector2(0.0, 1.0),
	])
	arrays[Mesh.ARRAY_INDEX] = PackedInt32Array([0, 1, 2, 0, 2, 3])
	quad.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)

	_wind_material = ShaderMaterial.new()
	_wind_material.shader = load("res://shaders/weather_wind_particles.gdshader")
	_wind_material.set_shader_parameter("u_wind", _wind_texture)
	_wind_material.set_shader_parameter("u_planet_radius_m", PLANET_RADIUS_M)
	quad.surface_set_material(0, _wind_material)

	var multimesh := MultiMesh.new()
	multimesh.transform_format = MultiMesh.TRANSFORM_3D
	multimesh.use_custom_data = true
	multimesh.mesh = quad
	multimesh.instance_count = PARTICLE_COUNT
	multimesh.custom_aabb = AABB(Vector3(-1.12, -1.12, -1.12), Vector3(2.24, 2.24, 2.24))
	var rng := RandomNumberGenerator.new()
	rng.seed = 0x41535445525241
	for i in PARTICLE_COUNT:
		multimesh.set_instance_transform(i, Transform3D.IDENTITY)
		multimesh.set_instance_custom_data(i, Color(
			rng.randf(), rng.randf_range(0.004, 0.996), rng.randf(), rng.randf()))
	_wind_particles.multimesh = multimesh
	_wind_particles.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	_wind_particles.visible = _flow_check == null or _flow_check.button_pressed
	_world_root.add_child(_wind_particles)


func _process(delta: float) -> void:
	if not visible:
		return
	_update_viewport_size()
	_update_camera_inertia(delta)
	_build_field_budgeted()
	_live_wind_accum += delta
	if _live_wind_accum >= LIVE_WIND_REFRESH_S:
		_live_wind_accum = 0.0
		_refresh_live_wind()
	_update_cursor_readout()


func _update_viewport_size() -> void:
	var size := Vector2i(get_viewport().get_visible_rect().size)
	if size.x < 16 or size.y < 16 or size == _last_viewport_size:
		return
	_last_viewport_size = size
	_subviewport.size = size


func _update_camera_inertia(delta: float) -> void:
	if not _dragging and _angular_velocity.length_squared() > 1e-8:
		_camera_yaw += _angular_velocity.x * delta * 60.0
		_camera_pitch = clampf(_camera_pitch + _angular_velocity.y * delta * 60.0, -1.48, 1.48)
		_angular_velocity *= exp(-delta * 7.0)
		_update_camera()


func _update_camera() -> void:
	if _camera == null:
		return
	var cp := cos(_camera_pitch)
	var direction := Vector3(cp * cos(_camera_yaw), sin(_camera_pitch), cp * sin(_camera_yaw))
	_camera.position = direction * _camera_distance
	var up := Vector3.UP
	if absf(direction.dot(up)) > 0.985:
		up = Vector3.FORWARD
	_camera.look_at(Vector3.ZERO, up)


func _unhandled_key_input(event: InputEvent) -> void:
	if not (event is InputEventKey) or not event.pressed or event.echo:
		return
	if event.unicode == 233 or event.unicode == 201:
		_set_open(not visible)
		get_viewport().set_input_as_handled()
		return
	if not visible:
		return
	if event.keycode == KEY_ESCAPE:
		_set_open(false)
	elif event.keycode == KEY_R:
		_camera_yaw = 0.35
		_camera_pitch = 0.16
		_camera_distance = 2.72
		_angular_velocity = Vector2.ZERO
		_update_camera()
	elif event.keycode == KEY_LEFT:
		_product_select.select((product_index + PRODUCT_NAMES.size() - 1) % PRODUCT_NAMES.size())
		_on_product_selected(_product_select.selected)
	elif event.keycode == KEY_RIGHT:
		_product_select.select((product_index + 1) % PRODUCT_NAMES.size())
		_on_product_selected(_product_select.selected)


func _input(event: InputEvent) -> void:
	if not visible:
		return
	if event is InputEventMouseButton:
		var over_panel := _panel != null and _panel.get_global_rect().has_point(event.position)
		if event.button_index == MOUSE_BUTTON_WHEEL_UP and event.pressed and not over_panel:
			_camera_distance = maxf(1.10, _camera_distance * 0.86)
			_update_camera()
			get_viewport().set_input_as_handled()
		elif event.button_index == MOUSE_BUTTON_WHEEL_DOWN and event.pressed and not over_panel:
			_camera_distance = minf(7.5, _camera_distance / 0.86)
			_update_camera()
			get_viewport().set_input_as_handled()
		elif event.button_index == MOUSE_BUTTON_LEFT:
			if event.pressed and not over_panel:
				_dragging = true
				_angular_velocity = Vector2.ZERO
				get_viewport().set_input_as_handled()
			elif not event.pressed:
				_dragging = false
	elif event is InputEventMouseMotion and _dragging:
		var sensitivity := 0.0060 * clampf(_camera_distance / 2.7, 0.34, 1.25)
		_camera_yaw -= event.relative.x * sensitivity
		_camera_pitch = clampf(_camera_pitch + event.relative.y * sensitivity, -1.48, 1.48)
		_angular_velocity = Vector2(-event.relative.x, event.relative.y) * sensitivity * 0.045
		_update_camera()
		get_viewport().set_input_as_handled()
	elif event is InputEventMagnifyGesture:
		_camera_distance = clampf(_camera_distance / maxf(event.factor, 0.05), 1.10, 7.5)
		_update_camera()


func _set_open(opened: bool) -> void:
	visible = opened
	_dragging = false
	if opened:
		_ensure_particles()
		_sync_weather_textures()
		_update_viewport_size()
		_begin_field_build_if_needed()
		_live_wind_accum = 999.0
		_update_product()


func _begin_field_build_if_needed(force_wind: bool = false) -> void:
	if Planet.fields == null or Planet.grid == null:
		return
	if not _base_ready:
		_base_image = Image.create(GLOBAL_W, GLOBAL_H, false, Image.FORMAT_RGB8)
	if force_wind or not _wind_fallback_ready:
		_wind_image = Image.create(GLOBAL_W, GLOBAL_H, false, Image.FORMAT_RGBAF)
	_field_build_y = 0


func _build_field_budgeted() -> void:
	if _field_build_y < 0:
		if not _base_ready and Planet.fields != null:
			_begin_field_build_if_needed()
		return
	if Planet.fields == null or Planet.grid == null:
		_field_build_y = -1
		return

	var started := Time.get_ticks_usec()
	while _field_build_y < GLOBAL_H:
		_build_field_row(_field_build_y)
		_field_build_y += 1
		if Time.get_ticks_usec() - started >= FIELD_BUILD_BUDGET_USEC:
			break
	if _field_build_y < GLOBAL_H:
		return

	_field_build_y = -1
	if not _base_ready and _base_image != null:
		_base_texture = ImageTexture.create_from_image(_base_image)
		_base_ready = true
		_globe_material.set_shader_parameter("u_base", _base_texture)
	if not _live_wind_available and _wind_image != null:
		_publish_wind_image(_wind_image)
		_wind_fallback_ready = true
		_source_label.text = "Wind source: baked prevailing field (rebuild native DLL for live layer vectors)"


func _build_field_row(y: int) -> void:
	var lat := PI * (0.5 - (float(y) + 0.5) / float(GLOBAL_H))
	var cos_lat := cos(lat)
	var fallback_scale: float = FALLBACK_LAYER_SPEED[wind_layer]
	for x in GLOBAL_W:
		# Base map uses conventional -π..π display longitude.
		if not _base_ready and _base_image != null:
			var display_lon := TAU * (float(x) + 0.5) / float(GLOBAL_W) - PI
			var display_dir := Vector3(cos_lat * cos(display_lon), sin(lat), cos_lat * sin(display_lon))
			var base_color := Color(0.025, 0.07, 0.16)
			if Planet.has_method("surface_color"):
				base_color = Planet.surface_color(display_dir)
			elif Planet.has_method("water_coverage"):
				var water: float = clampf(Planet.water_coverage(display_dir), 0.0, 1.0)
				base_color = Color(0.055, 0.20, 0.08).lerp(Color(0.018, 0.07, 0.19), water)
			_base_image.set_pixel(x, y, base_color)

		# Wind textures use the native atmosphere's 0..2π longitude convention.
		if not _live_wind_available and _wind_image != null:
			var lon := TAU * (float(x) + 0.5) / float(GLOBAL_W)
			var direction := Vector3(cos_lat * cos(lon), sin(lat), cos_lat * sin(lon))
			var u := float(Planet.grid.sample_bilinear(Planet.fields.wind_u, direction)) * fallback_scale
			var v := float(Planet.grid.sample_bilinear(Planet.fields.wind_v, direction)) * fallback_scale
			_wind_image.set_pixel(x, y, Color(u, v, sqrt(u * u + v * v), 1.0))


func _refresh_live_wind() -> void:
	var native: Variant = WeatherSystem.get("_native")
	if native == null or not (native is Object) or not native.has_method(&"get_global_wind_rgba"):
		_live_wind_available = false
		if not _wind_fallback_ready and _field_build_y < 0:
			_begin_field_build_if_needed(true)
		return
	var result: Variant = native.call(&"get_global_wind_rgba", wind_layer)
	if not (result is PackedFloat32Array):
		return
	var values: PackedFloat32Array = result
	if values.size() != GLOBAL_W * GLOBAL_H * 4:
		return
	var image := Image.create_from_data(
		GLOBAL_W, GLOBAL_H, false, Image.FORMAT_RGBAF, values.to_byte_array())
	_live_wind_available = true
	_wind_values = values
	_wind_texture.update(image)
	_sync_weather_textures()
	_source_label.text = "Wind source: LIVE six-layer AVX2 atmosphere  •  %s" % LAYER_NAMES[wind_layer]


func _publish_wind_image(image: Image) -> void:
	_wind_values = image.get_data().to_float32_array()
	if _wind_texture == null:
		_wind_texture = ImageTexture.create_from_image(image)
	else:
		_wind_texture.update(image)
	_sync_weather_textures()


func _on_product_selected(index: int) -> void:
	product_index = clampi(index, 0, PRODUCT_NAMES.size() - 1)
	_update_product()


func _on_layer_selected(index: int) -> void:
	wind_layer = clampi(index, 0, LAYER_NAMES.size() - 1)
	_live_wind_accum = 999.0
	if not _live_wind_available:
		_wind_fallback_ready = false
		_begin_field_build_if_needed(true)


func _on_flow_toggled(enabled: bool) -> void:
	if _wind_particles != null:
		_wind_particles.visible = enabled


func _update_product() -> void:
	if _globe_material != null:
		_globe_material.set_shader_parameter("u_product", product_index)
	if _note_label != null:
		_note_label.text = PRODUCT_NOTES[product_index]
	if _legend_left != null:
		_legend_left.text = LEGEND_LEFT[product_index]
		_legend_right.text = LEGEND_RIGHT[product_index]
		_legend.texture = _make_legend_texture(product_index)


func _make_legend_texture(product: int) -> GradientTexture1D:
	var gradient := Gradient.new()
	gradient.offsets = PackedFloat32Array([0.0, 0.28, 0.55, 0.78, 1.0])
	match product:
		Product.WIND:
			gradient.colors = PackedColorArray([
				Color(0.035, 0.075, 0.30), Color(0.025, 0.31, 0.54),
				Color(0.045, 0.62, 0.43), Color(0.65, 0.92, 0.13), Color(0.95, 0.16, 0.12)])
		Product.PRECIPITATION:
			gradient.colors = PackedColorArray([
				Color(0.02, 0.09, 0.20), Color(0.04, 0.56, 0.88),
				Color(0.16, 0.86, 0.34), Color(1.0, 0.82, 0.13), Color(0.90, 0.06, 0.25)])
		Product.PRESSURE, Product.VORTICITY, Product.DIVERGENCE, Product.POTENTIAL_VORTICITY:
			gradient.colors = PackedColorArray([
				Color(0.08, 0.24, 0.82), Color(0.40, 0.58, 0.88),
				Color(0.86, 0.88, 0.90), Color(0.93, 0.52, 0.40), Color(0.90, 0.15, 0.08)])
		_:
			gradient.colors = PackedColorArray([
				Color(0.04, 0.08, 0.18), Color(0.08, 0.42, 0.75),
				Color(0.10, 0.75, 0.45), Color(1.0, 0.80, 0.12), Color(0.92, 0.06, 0.12)])
	var texture := GradientTexture1D.new()
	texture.gradient = gradient
	texture.width = 330
	return texture


func _update_cursor_readout() -> void:
	if _camera == null or _viewport_container == null:
		return
	var mouse := _viewport_container.get_local_mouse_position()
	var size := Vector2(_subviewport.size)
	if mouse.x < 0.0 or mouse.y < 0.0 or mouse.x >= size.x or mouse.y >= size.y:
		_cursor_label.text = ""
		return
	var origin := _camera.project_ray_origin(mouse)
	var direction := _camera.project_ray_normal(mouse).normalized()
	var b := origin.dot(direction)
	var c := origin.length_squared() - 1.0
	var discriminant := b * b - c
	if discriminant < 0.0:
		_cursor_label.text = ""
		return
	var t := -b - sqrt(discriminant)
	if t <= 0.0:
		_cursor_label.text = ""
		return
	var hit := (origin + direction * t).normalized()
	var lat := asin(clampf(hit.y, -1.0, 1.0))
	var lon := atan2(hit.z, hit.x)
	var lat_text := "%.2f°%s" % [absf(rad_to_deg(lat)), "N" if lat >= 0.0 else "S"]
	var lon_text := "%.2f°%s" % [absf(rad_to_deg(lon)), "E" if lon >= 0.0 else "W"]
	var wind := _sample_wind(lon, lat)
	var speed_kmh := wind.z * 3.6
	var toward := rad_to_deg(atan2(wind.x, wind.y))
	var from_degrees := fmod(toward + 540.0, 360.0)
	_cursor_label.text = "%s, %s\nWind | %03d° @ %.1f km/h  •  %s" % [
		lat_text, lon_text, int(round(from_degrees)), speed_kmh, LAYER_NAMES[wind_layer]]


func _sample_wind(lon: float, lat: float) -> Vector3:
	if _wind_values.size() != GLOBAL_W * GLOBAL_H * 4:
		return Vector3.ZERO
	var ucoord := fposmod(lon / TAU, 1.0)
	var vcoord := clampf(0.5 - lat / PI, 0.0, 0.999999)
	var x := wrapi(int(floor(ucoord * GLOBAL_W)), 0, GLOBAL_W)
	var y := clampi(int(floor(vcoord * GLOBAL_H)), 0, GLOBAL_H - 1)
	var offset := (x + y * GLOBAL_W) * 4
	return Vector3(
		_wind_values[offset], _wind_values[offset + 1], _wind_values[offset + 2])
