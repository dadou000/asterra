class_name PlanetMap
extends CanvasLayer
## Whole-planet layer inspector. Base environmental classifications, hydrology
## attributes and exceptional landmarks are intentionally separate map layers.

enum Layer {
	ELEVATION, PLATES, GEOLOGY, LANDMARKS, RESOURCES, EROSION, DRAINAGE, WATERSHEDS,
	TEMPERATURE, PRECIPITATION, WIND, STORMS, SOIL, BIOMES, SUITABILITY, CORRIDORS,
}
const LAYER_NAMES := [
	"Elevation", "Tectonic plates / margins", "Bedrock geology", "Landmarks",
	"Resources", "Sediment & erosion", "Hydrology / drainage", "Watersheds",
	"Temperature", "Precipitation", "Prevailing winds", "Severe weather",
	"Soil", "Biomes", "Buildability", "Transport corridors",
]

const W := 960
const H := 480
const RENDER_BUDGET_USEC := 3000

var layer_index: int = Layer.ELEVATION
var texture_rect: TextureRect
var title: Label
var marker: Control
var _player_dir: Vector3 = Vector3(1, 0, 0)
var _cache: Dictionary = {}

var _render_image: Image
var _render_layer: int = -1
var _render_y: int = 0
var _render_fields: PlanetFields
var _render_grid: PlanetGrid

func _ready() -> void:
	layer = 20
	visible = false
	var bg := ColorRect.new()
	bg.color = Color(0.02, 0.03, 0.05, 0.94)
	bg.set_anchors_preset(Control.PRESET_FULL_RECT)
	add_child(bg)
	texture_rect = TextureRect.new()
	texture_rect.set_anchors_preset(Control.PRESET_CENTER)
	texture_rect.position = Vector2(-float(W) * 0.5, -float(H) * 0.5)
	texture_rect.custom_minimum_size = Vector2(W, H)
	texture_rect.stretch_mode = TextureRect.STRETCH_SCALE
	add_child(texture_rect)
	title = Label.new()
	title.set_anchors_preset(Control.PRESET_CENTER_TOP)
	title.position = Vector2(-float(W) * 0.5, 40)
	title.custom_minimum_size = Vector2(W, 24)
	title.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	title.add_theme_font_size_override("font_size", 17)
	add_child(title)
	marker = Control.new()
	marker.set_anchors_preset(Control.PRESET_CENTER)
	marker.draw.connect(_draw_marker)
	add_child(marker)

func _process(_dt: float) -> void:
	if _render_layer < 0 or _render_image == null:
		return
	var started: int = Time.get_ticks_usec()
	while _render_y < H:
		_render_row(_render_y)
		_render_y += 1
		if Time.get_ticks_usec() - started >= RENDER_BUDGET_USEC:
			break
	if _render_y < H:
		return

	var completed_layer: int = _render_layer
	var texture := ImageTexture.create_from_image(_render_image)
	_cache[completed_layer] = texture
	_render_layer = -1
	_render_y = 0
	_render_image = null
	_render_fields = null
	_render_grid = null
	if visible and completed_layer == layer_index:
		texture_rect.texture = texture
		_update_title(false)

func toggle() -> void:
	visible = not visible
	if visible:
		refresh()

func cycle(step: int) -> void:
	layer_index = wrapi(layer_index + step, 0, LAYER_NAMES.size())
	if visible:
		refresh()

func set_player_dir(d: Vector3) -> void:
	_player_dir = d
	if visible:
		marker.queue_redraw()

func refresh() -> void:
	if not Planet.ready_state:
		return
	if _cache.has(layer_index):
		texture_rect.texture = _cache[layer_index]
		_update_title(false)
	else:
		_begin_render(layer_index)
		_update_title(true)
	marker.queue_redraw()

func invalidate() -> void:
	_cache.clear()
	_render_layer = -1
	_render_y = 0
	_render_image = null
	_render_fields = null
	_render_grid = null
	if texture_rect != null:
		texture_rect.texture = null

func _begin_render(which: int) -> void:
	if _render_layer == which:
		return
	_render_layer = which
	_render_y = 0
	_render_image = Image.create(W, H, false, Image.FORMAT_RGB8)
	_render_fields = Planet.fields
	_render_grid = Planet.grid

func _render_row(y: int) -> void:
	if _render_fields == null or _render_grid == null:
		_render_layer = -1
		return
	var lat: float = (0.5 - float(y) / float(H)) * PI
	for x in W:
		var lon: float = (float(x) / float(W) - 0.5) * TAU
		var d: Vector3 = CubeSphere.latlon_to_dir(lat, lon)
		var c: int = _render_grid.dir_to_index(d)
		_render_image.set_pixel(x, y, _color_for(_render_layer, _render_fields, c))

func _render(which: int) -> Image:
	if not Planet.ready_state or Planet.fields == null or Planet.grid == null:
		return Image.create(1, 1, false, Image.FORMAT_RGB8)
	var image: Image = Image.create(W, H, false, Image.FORMAT_RGB8)
	var fields: PlanetFields = Planet.fields
	var grid: PlanetGrid = Planet.grid
	for y in H:
		var lat: float = (0.5 - float(y) / float(H)) * PI
		for x in W:
			var lon: float = (float(x) / float(W) - 0.5) * TAU
			var d: Vector3 = CubeSphere.latlon_to_dir(lat, lon)
			var c: int = grid.dir_to_index(d)
			image.set_pixel(x, y, _color_for(which, fields, c))
	return image

func _update_title(rendering: bool) -> void:
	var suffix := "  — rendering…" if rendering else ""
	title.text = "%s%s   ( , / . to change layer, M to close )" % [LAYER_NAMES[layer_index], suffix]

func _color_for(which: int, f: PlanetFields, c: int) -> Color:
	var h: float = f.elev[c]
	var sea := h < 0.0
	match which:
		Layer.ELEVATION:
			if sea:
				var t := clampf(-h / 5000.0, 0.0, 1.0)
				return Color(0.02, 0.10, 0.30).lerp(Color(0.25, 0.55, 0.78), 1.0 - t)
			var t2 := clampf(h / 5200.0, 0.0, 1.0)
			var land := Color(0.24, 0.44, 0.20).lerp(Color(0.55, 0.44, 0.30), t2).lerp(
				Color(0.98, 0.98, 1.0), NoiseKit.smoothstepf(0.62, 1.0, t2))
			if f.is_lake(c):
				land = land.lerp(Color(0.18, 0.48, 0.78), 0.82)
			return land

		Layer.PLATES:
			var p: int = f.plate[c]
			var col := Color.from_hsv(fmod(float(p) * 0.137, 1.0), 0.55, 0.85)
			if sea:
				col = col.darkened(0.45)
			var bnd := f.plate_boundary[c]
			if bnd > 0.08:
				var bt: int = f.plate_boundary_type[c]
				col = col.lerp(PlanetFields.TECTONIC_BOUNDARY_COLORS[bt],
					clampf(bnd * 0.90, 0.0, 0.90))
			return col

		Layer.GEOLOGY:
			if sea:
				return Color(0.06, 0.10, 0.16)
			return Color.from_hsv(
				fmod(float(f.rock[c]) * 0.0771 + 0.08, 1.0), 0.52,
				0.45 + 0.4 * float(f.rock[c] % 3) / 2.0)

		Layer.LANDMARKS:
			var background := Color(0.025, 0.04, 0.065) if sea else Color(0.10, 0.105, 0.10)
			# Low-intensity margin context makes it obvious why chains occur where
			# they do without turning the landmark layer into a second plate map.
			if f.plate_boundary[c] > 0.20:
				var bt2: int = f.plate_boundary_type[c]
				background = background.lerp(
					PlanetFields.TECTONIC_BOUNDARY_COLORS[bt2], f.plate_boundary[c] * 0.16)
			var lm: int = f.landmark[c]
			if lm == PlanetFields.Landmark.NONE:
				return background
			var strength := clampf(f.landmark_strength[c], 0.0, 1.0)
			var feature_col: Color = PlanetFields.LANDMARK_COLORS[lm]
			return background.lerp(feature_col, 0.32 + strength * 0.68)

		Layer.RESOURCES:
			if sea:
				return Color(0.05, 0.08, 0.12)
			return Color(
				clampf(f.ore_iron[c] * 1.6 + f.ore_copper[c] * 0.4, 0, 1),
				clampf(f.quartz[c] * 1.8, 0, 1),
				clampf(f.petroleum[c] * 1.4 + f.coal[c] * 0.9, 0, 1))

		Layer.EROSION:
			if sea:
				return Color(0.05, 0.08, 0.14)
			var d1 := clampf(f.sediment[c] / 60.0, 0.0, 1.0)
			var up := clampf(f.uplift[c] * 2.0, 0.0, 1.0)
			return Color(0.15 + up * 0.8, 0.25 + d1 * 0.5, 0.2)

		Layer.DRAINAGE:
			if sea:
				return Color(0.025, 0.065, 0.16)
			if f.is_lake(c):
				return Color(0.10, 0.42, 0.88)
			var land_hydro := Color(0.17, 0.17, 0.15).lerp(
				Color(0.28, 0.30, 0.23), clampf(f.wetland[c], 0.0, 1.0) * 0.75)
			land_hydro = land_hydro.lerp(Color(0.30, 0.42, 0.24),
				clampf(f.floodplain[c], 0.0, 1.0) * 0.55)
			if f.river_width[c] <= 0.0:
				return land_hydro
			var river := clampf(log(1.0 + f.river_width[c]) / log(2601.0), 0.0, 1.0)
			return land_hydro.lerp(Color(0.18, 0.58, 1.0), 0.45 + river * 0.55)

		Layer.WATERSHEDS:
			if sea:
				return Color(0.05, 0.07, 0.12)
			var ws: int = f.watershed[c]
			return Color.from_hsv(
				fmod(float(absi(HashRNG.hash2(7, ws)) % 997) / 997.0, 1.0), 0.5, 0.85)

		Layer.TEMPERATURE:
			var t4 := clampf((f.temp_mean[c] + 40.0) / 75.0, 0.0, 1.0)
			return Color(0.15, 0.30, 0.85).lerp(Color(0.95, 0.85, 0.25), t4).lerp(
				Color(0.9, 0.2, 0.15), NoiseKit.smoothstepf(0.68, 1.0, t4))

		Layer.PRECIPITATION:
			var p2 := clampf(f.precip[c] / 2600.0, 0.0, 1.0)
			return Color(0.70, 0.60, 0.35).lerp(Color(0.05, 0.35, 0.75), p2)

		Layer.WIND:
			var u := clampf(f.wind_u[c] / 14.0 * 0.5 + 0.5, 0.0, 1.0)
			var v := clampf(f.wind_v[c] / 8.0 * 0.5 + 0.5, 0.0, 1.0)
			return Color(u, 0.35, v)

		Layer.STORMS:
			var s2: float = f.storm_risk[c]
			return Color(0.06, 0.07, 0.10).lerp(Color(1.0, 0.85, 0.25), s2).lerp(
				Color(1.0, 0.2, 0.1), NoiseKit.smoothstepf(0.55, 1.0, s2))

		Layer.SOIL:
			if sea:
				return Color(0.05, 0.08, 0.13)
			return Color(
				clampf(f.soil_sand[c], 0, 1), clampf(f.soil_organic[c] * 2.2, 0, 1),
				clampf(f.soil_clay[c], 0, 1)) * clampf(0.25 + f.soil_depth[c] / 3.0, 0.25, 1.0)

		Layer.BIOMES:
			# Deliberately no river/lake overlay here. This is the ecological base
			# layer; inspect Hydrology to see surface-water attributes.
			return PlanetFields.BIOME_COLORS[f.biome[c]]

		Layer.SUITABILITY:
			if f.is_water(c):
				return Color(0.05, 0.08, 0.13)
			return Color(0.10, 0.10, 0.12).lerp(Color(0.35, 1.0, 0.45), f.suitability[c])

		Layer.CORRIDORS:
			if f.is_water(c):
				return Color(0.05, 0.08, 0.13)
			return Color(0.10, 0.10, 0.12).lerp(Color(1.0, 0.75, 0.25), f.corridor[c])
	return Color.MAGENTA

func _draw_marker() -> void:
	var latlon := CubeSphere.dir_to_latlon(_player_dir)
	var x := (latlon.y / TAU + 0.5) * float(W) - float(W) * 0.5
	var y := (0.5 - latlon.x / PI) * float(H) - float(H) * 0.5
	marker.draw_circle(Vector2(x, y), 5.0, Color(1, 0.2, 0.2, 0.9))
	marker.draw_circle(Vector2(x, y), 9.0, Color(1, 1, 1, 0.35), false, 1.5)
