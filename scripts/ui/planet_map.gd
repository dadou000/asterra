class_name PlanetMap
extends CanvasLayer
## Whole-planet layer inspector.
##
## Renders any baked field as an equirectangular map. This is the fastest way to
## confirm that the passes are actually coupled: rain shadows should sit behind
## mountain belts, rivers should thread the valleys, deserts should sit under the
## subtropical highs, and the transport corridors should follow the valleys the
## erosion pass cut.

enum Layer {
	ELEVATION, PLATES, GEOLOGY, RESOURCES, EROSION, DRAINAGE, WATERSHEDS,
	TEMPERATURE, PRECIPITATION, WIND, STORMS, SOIL, BIOMES, SUITABILITY, CORRIDORS,
}
const LAYER_NAMES := [
	"Elevation", "Tectonic plates", "Bedrock geology", "Resources",
	"Sediment & erosion", "Drainage network", "Watersheds",
	"Temperature", "Precipitation", "Prevailing winds", "Severe weather",
	"Soil", "Biomes", "Buildability", "Transport corridors",
]

const W := 960
const H := 480

var layer_index: int = Layer.ELEVATION
var texture_rect: TextureRect
var title: Label
var marker: Control
var _player_dir: Vector3 = Vector3(1, 0, 0)
var _cache: Dictionary = {}

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

func toggle() -> void:
	visible = not visible
	if visible:
		refresh()

func cycle(step: int) -> void:
	layer_index = wrapi(layer_index + step, 0, LAYER_NAMES.size())
	refresh()

func set_player_dir(d: Vector3) -> void:
	_player_dir = d
	if visible:
		marker.queue_redraw()

func refresh() -> void:
	if not Planet.ready_state:
		return
	title.text = "%s   ( , / . to change layer, M to close )" % LAYER_NAMES[layer_index]
	if not _cache.has(layer_index):
		_cache[layer_index] = ImageTexture.create_from_image(_render(layer_index))
	texture_rect.texture = _cache[layer_index]
	marker.queue_redraw()

func invalidate() -> void:
	_cache.clear()

func _render(which: int) -> Image:
	var img := Image.create(W, H, false, Image.FORMAT_RGB8)
	var f := Planet.fields
	var g := Planet.grid
	for y in H:
		var lat := (0.5 - float(y) / float(H)) * PI
		for x in W:
			var lon := (float(x) / float(W) - 0.5) * TAU
			var d := CubeSphere.latlon_to_dir(lat, lon)
			var c := g.dir_to_index(d)
			img.set_pixel(x, y, _color_for(which, f, c))
	return img

func _color_for(which: int, f: PlanetFields, c: int) -> Color:
	var h: float = f.elev[c]
	var water := h < 0.0
	match which:
		Layer.ELEVATION:
			if water:
				var t := clampf(-h / 5000.0, 0.0, 1.0)
				return Color(0.02, 0.10, 0.30).lerp(Color(0.25, 0.55, 0.78), 1.0 - t)
			var t2 := clampf(h / 5200.0, 0.0, 1.0)
			return Color(0.24, 0.44, 0.20).lerp(Color(0.55, 0.44, 0.30), t2).lerp(
				Color(0.98, 0.98, 1.0), NoiseKit.smoothstepf(0.62, 1.0, t2))
		Layer.PLATES:
			var p: int = f.plate[c]
			var col := Color.from_hsv(fmod(float(p) * 0.137, 1.0), 0.55, 0.85)
			return col.darkened(0.45) if water else col
		Layer.GEOLOGY:
			if water:
				return Color(0.06, 0.10, 0.16)
			return Color.from_hsv(fmod(float(f.rock[c]) * 0.0771 + 0.08, 1.0), 0.52,
				0.45 + 0.4 * float(f.rock[c] % 3) / 2.0)
		Layer.RESOURCES:
			if water:
				return Color(0.05, 0.08, 0.12)
			return Color(clampf(f.ore_iron[c] * 1.6 + f.ore_copper[c] * 0.4, 0, 1),
				clampf(f.quartz[c] * 1.8, 0, 1),
				clampf(f.petroleum[c] * 1.4 + f.coal[c] * 0.9, 0, 1))
		Layer.EROSION:
			if water:
				return Color(0.05, 0.08, 0.14)
			var d1 := clampf(f.sediment[c] / 60.0, 0.0, 1.0)
			var up := clampf(f.uplift[c] * 2.0, 0.0, 1.0)
			return Color(0.15 + up * 0.8, 0.25 + d1 * 0.5, 0.2)
		Layer.DRAINAGE:
			if water:
				return Color(0.04, 0.07, 0.14)
			var q: float = f.discharge[c]
			var t3 := clampf(log(maxf(q, 0.01)) / 9.0 + 0.35, 0.0, 1.0)
			var land := Color(0.16, 0.17, 0.16).lerp(Color(0.42, 0.42, 0.40), clampf(h / 3000.0, 0, 1))
			return land.lerp(Color(0.25, 0.62, 1.0), NoiseKit.smoothstepf(0.42, 0.95, t3))
		Layer.WATERSHEDS:
			if water:
				return Color(0.05, 0.07, 0.12)
			var ws: int = f.watershed[c]
			return Color.from_hsv(fmod(float(absi(HashRNG.hash2(7, ws)) % 997) / 997.0, 1.0), 0.5, 0.85)
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
			if water:
				return Color(0.05, 0.08, 0.13)
			return Color(clampf(f.soil_sand[c], 0, 1), clampf(f.soil_organic[c] * 2.2, 0, 1),
				clampf(f.soil_clay[c], 0, 1)) * clampf(0.25 + f.soil_depth[c] / 3.0, 0.25, 1.0)
		Layer.BIOMES:
			return PlanetFields.BIOME_COLORS[f.biome[c]]
		Layer.SUITABILITY:
			if water:
				return Color(0.05, 0.08, 0.13)
			return Color(0.10, 0.10, 0.12).lerp(Color(0.35, 1.0, 0.45), f.suitability[c])
		Layer.CORRIDORS:
			if water:
				return Color(0.05, 0.08, 0.13)
			return Color(0.10, 0.10, 0.12).lerp(Color(1.0, 0.75, 0.25), f.corridor[c])
	return Color.MAGENTA

func _draw_marker() -> void:
	var latlon := CubeSphere.dir_to_latlon(_player_dir)
	var x := (latlon.y / TAU + 0.5) * float(W) - float(W) * 0.5
	var y := (0.5 - latlon.x / PI) * float(H) - float(H) * 0.5
	marker.draw_circle(Vector2(x, y), 5.0, Color(1, 0.2, 0.2, 0.9))
	marker.draw_circle(Vector2(x, y), 9.0, Color(1, 1, 1, 0.35), false, 1.5)
