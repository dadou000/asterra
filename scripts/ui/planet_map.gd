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
var _player_dir := Vector3(1, 0, 0)
var _cache: Dictionary = {}
var _render_image: Image
var _render_layer := -1
var _render_y := 0
var _render_fields: Variant
var _render_grid: Variant

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
	if _render_layer < 0 or _render_image == null: return
	var started := Time.get_ticks_usec()
	while _render_y < H:
		_render_row(_render_y)
		_render_y += 1
		if Time.get_ticks_usec() - started >= RENDER_BUDGET_USEC: break
	if _render_y < H: return
	var completed_layer := _render_layer
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
	if visible: refresh()

func cycle(step: int) -> void:
	layer_index = wrapi(layer_index + step, 0, LAYER_NAMES.size())
	if visible: refresh()

func set_player_dir(d: Vector3) -> void:
	_player_dir = d
	if visible: marker.queue_redraw()

func refresh() -> void:
	if not Planet.ready_state: return
	if _cache.has(layer_index):
		texture_rect.texture = _cache[layer_index]
		_update_title(false)
	else:
		_begin_render(layer_index)
		_update_title(true)
	marker.queue_redraw()

func invalidate() -> void:
	_cache.clear(); _render_layer = -1; _render_y = 0; _render_image = null
	_render_fields = null; _render_grid = null
	if texture_rect != null: texture_rect.texture = null

func _begin_render(which: int) -> void:
	if _render_layer == which: return
	_render_layer = which; _render_y = 0
	_render_image = Image.create(W, H, false, Image.FORMAT_RGB8)
	_render_fields = Planet.fields
	_render_grid = Planet.grid

func _render_row(y: int) -> void:
	if _render_fields == null or _render_grid == null:
		_render_layer = -1; return
	var lat := (0.5 - float(y) / float(H)) * PI
	for x in W:
		var lon := (float(x) / float(W) - 0.5) * TAU
		var d := CubeSphere.latlon_to_dir(lat, lon)
		var c := int(_render_grid.dir_to_index(d))
		_render_image.set_pixel(x, y, _color_for(_render_layer, _render_fields, c))

func _render(which: int) -> Image:
	if not Planet.ready_state or Planet.fields == null or Planet.grid == null:
		return Image.create(1, 1, false, Image.FORMAT_RGB8)
	var image := Image.create(W, H, false, Image.FORMAT_RGB8)
	var fields: Variant = Planet.fields
	var grid: Variant = Planet.grid
	for y in H:
		var lat := (0.5 - float(y) / float(H)) * PI
		for x in W:
			var lon := (float(x) / float(W) - 0.5) * TAU
			var d := CubeSphere.latlon_to_dir(lat, lon)
			var c := int(grid.dir_to_index(d))
			image.set_pixel(x, y, _color_for(which, fields, c))
	return image

func _update_title(rendering: bool) -> void:
	var suffix := "  — rendering…" if rendering else ""
	title.text = "%s%s   ( , / . to change layer, M to close )" % [LAYER_NAMES[layer_index], suffix]

func _color_for(which: int, f: Variant, c: int) -> Color:
	var h := float(f.elev[c]); var sea := h < 0.0
	match which:
		Layer.ELEVATION:
			if sea: return Color(0.02,0.10,0.30).lerp(Color(0.25,0.55,0.78), 1.0-clampf(-h/5000.0,0.0,1.0))
			var t2 := clampf(h/5200.0,0.0,1.0)
			var land := Color(0.24,0.44,0.20).lerp(Color(0.55,0.44,0.30),t2).lerp(Color(0.98,0.98,1.0),NoiseKit.smoothstepf(0.62,1.0,t2))
			if f.is_lake(c): land = land.lerp(Color(0.18,0.48,0.78),0.82)
			return land
		Layer.PLATES:
			var p := int(f.plate[c]); var col := Color.from_hsv(fmod(float(p)*0.137,1.0),0.55,0.85)
			if sea: col = col.darkened(0.45)
			var bnd := float(f.plate_boundary[c])
			if bnd > 0.08: col = col.lerp(PlanetFields.TECTONIC_BOUNDARY_COLORS[int(f.plate_boundary_type[c])],clampf(bnd*0.90,0.0,0.90))
			return col
		Layer.GEOLOGY:
			if sea: return Color(0.06,0.10,0.16)
			return Color.from_hsv(fmod(float(f.rock[c])*0.0771+0.08,1.0),0.52,0.45+0.4*float(int(f.rock[c])%3)/2.0)
		Layer.LANDMARKS:
			var background := Color(0.025,0.04,0.065) if sea else Color(0.10,0.105,0.10)
			if float(f.plate_boundary[c]) > 0.20: background = background.lerp(PlanetFields.TECTONIC_BOUNDARY_COLORS[int(f.plate_boundary_type[c])],float(f.plate_boundary[c])*0.16)
			var lm := int(f.landmark[c])
			if lm == PlanetFields.Landmark.NONE: return background
			return background.lerp(PlanetFields.LANDMARK_COLORS[lm],0.32+clampf(float(f.landmark_strength[c]),0.0,1.0)*0.68)
		Layer.RESOURCES:
			if sea: return Color(0.05,0.08,0.12)
			return Color(clampf(float(f.ore_iron[c])*1.6+float(f.ore_copper[c])*0.4,0,1),clampf(float(f.quartz[c])*1.8,0,1),clampf(float(f.petroleum[c])*1.4+float(f.coal[c])*0.9,0,1))
		Layer.EROSION:
			if sea: return Color(0.05,0.08,0.14)
			return Color(0.15+clampf(float(f.uplift[c])*2.0,0,1)*0.8,0.25+clampf(float(f.sediment[c])/60.0,0,1)*0.5,0.2)
		Layer.DRAINAGE:
			if sea: return Color(0.025,0.065,0.16)
			if f.is_lake(c): return Color(0.10,0.42,0.88)
			var lh := Color(0.17,0.17,0.15).lerp(Color(0.28,0.30,0.23),clampf(float(f.wetland[c]),0,1)*0.75)
			lh = lh.lerp(Color(0.30,0.42,0.24),clampf(float(f.floodplain[c]),0,1)*0.55)
			if float(f.river_width[c]) <= 0.0: return lh
			var river := clampf(log(1.0+float(f.river_width[c]))/log(2601.0),0,1)
			return lh.lerp(Color(0.18,0.58,1.0),0.45+river*0.55)
		Layer.WATERSHEDS:
			if sea: return Color(0.05,0.07,0.12)
			return Color.from_hsv(fmod(float(absi(HashRNG.hash2(7,int(f.watershed[c])))%997)/997.0,1.0),0.5,0.85)
		Layer.TEMPERATURE:
			var t := clampf((float(f.temp_mean[c])+40.0)/75.0,0,1); return Color(0.15,0.30,0.85).lerp(Color(0.95,0.85,0.25),t).lerp(Color(0.9,0.2,0.15),NoiseKit.smoothstepf(0.68,1.0,t))
		Layer.PRECIPITATION: return Color(0.70,0.60,0.35).lerp(Color(0.05,0.35,0.75),clampf(float(f.precip[c])/2600.0,0,1))
		Layer.WIND: return Color(clampf(float(f.wind_u[c])/14.0*0.5+0.5,0,1),0.35,clampf(float(f.wind_v[c])/8.0*0.5+0.5,0,1))
		Layer.STORMS:
			var s := float(f.storm_risk[c]); return Color(0.06,0.07,0.10).lerp(Color(1.0,0.85,0.25),s).lerp(Color(1.0,0.2,0.1),NoiseKit.smoothstepf(0.55,1.0,s))
		Layer.SOIL:
			if sea: return Color(0.05,0.08,0.13)
			return Color(clampf(float(f.soil_sand[c]),0,1),clampf(float(f.soil_organic[c])*2.2,0,1),clampf(float(f.soil_clay[c]),0,1))*clampf(0.25+float(f.soil_depth[c])/3.0,0.25,1)
		Layer.BIOMES: return PlanetFields.BIOME_COLORS[int(f.biome[c])]
		Layer.SUITABILITY:
			if f.is_water(c): return Color(0.05,0.08,0.13)
			return Color(0.10,0.10,0.12).lerp(Color(0.35,1.0,0.45),float(f.suitability[c]))
		Layer.CORRIDORS:
			if f.is_water(c): return Color(0.05,0.08,0.13)
			return Color(0.10,0.10,0.12).lerp(Color(1.0,0.75,0.25),float(f.corridor[c]))
	return Color.MAGENTA

func _draw_marker() -> void:
	var latlon := CubeSphere.dir_to_latlon(_player_dir)
	var x := (latlon.y/TAU+0.5)*float(W)-float(W)*0.5
	var y := (0.5-latlon.x/PI)*float(H)-float(H)*0.5
	marker.draw_circle(Vector2(x,y),5.0,Color(1,0.2,0.2,0.9))
	marker.draw_circle(Vector2(x,y),9.0,Color(1,1,1,0.35),false,1.5)
