class_name WeatherSystem
extends Node
## Authoritative runtime weather potential for Asterra.
##
## This does not draw clouds. It converts the baked climate + the actual terrain
## into two seamless cube-sphere texture arrays consumed by every weather renderer.
## The important rule is the same one used by terrain: one spherical state, many
## visual LODs. Low clouds, orbit clouds, shadows and precipitation all sample the
## same fields instead of inventing unrelated weather.
##
## Weather map RGBA:
##   R cloud coverage potential
##   G convective potential
##   B high-cloud / anvil potential
##   A low-cloud base altitude / 6000 m
## Wind map RGBA (half float):
##   R eastward wind m/s
##   G northward wind m/s
##   B precipitation potential
##   A humidity

signal rebuilt

var weather_map: Texture2DArray
var wind_map: Texture2DArray
var face_res: int = 0

var _cfg: GenConfig
var _grid: PlanetGrid
var _fields: PlanetFields
var _synoptic: NoiseKit
var _fronts: NoiseKit
var _cells: NoiseKit

func rebuild() -> void:
	if not Planet.ready_state:
		return
	_cfg = Planet.cfg
	_grid = Planet.grid
	_fields = Planet.fields
	face_res = _grid.res
	_synoptic = NoiseKit.new(_cfg.stream_seed("weather_synoptic"), 3.4, 4)
	_fronts = NoiseKit.ridged(_cfg.stream_seed("weather_fronts"), 6.0, 4)
	_cells = NoiseKit.new(_cfg.stream_seed("weather_cells"), 10.0, 3)
	_build_textures()
	rebuilt.emit()

func _build_textures() -> void:
	weather_map = null
	wind_map = null
	var res: int = face_res
	var tex_res: int = res + 2
	var cell_step: float = 2.0 / float(res)
	var weather_images: Array[Image] = []
	var wind_images: Array[Image] = []

	for face in 6:
		var wimg := Image.create(tex_res, tex_res, false, Image.FORMAT_RGBAH)
		var fimg := Image.create(tex_res, tex_res, false, Image.FORMAT_RGBAH)
		for y in tex_res:
			var j: int = y - 1
			var v: float = (float(j) + 0.5) * cell_step - 1.0
			for x in tex_res:
				var i: int = x - 1
				var u: float = (float(i) + 0.5) * cell_step - 1.0
				var d := CubeSphere.face_uv_to_dir(face, u, v)
				var sample := _sample_weather(d)
				wimg.set_pixel(x, y, Color(sample["coverage"], sample["convective"],
					sample["high"], sample["base"] / 6000.0))
				fimg.set_pixel(x, y, Color(sample["wind_u"], sample["wind_v"],
					sample["precip"], sample["humidity"]))
		weather_images.append(wimg)
		wind_images.append(fimg)

	var weather_tex := Texture2DArray.new()
	var err := weather_tex.create_from_images(weather_images)
	if err != OK:
		push_error("Failed to build weather texture array (%d)" % err)
		return
	var wind_tex := Texture2DArray.new()
	err = wind_tex.create_from_images(wind_images)
	if err != OK:
		push_error("Failed to build wind texture array (%d)" % err)
		return
	weather_map = weather_tex
	wind_map = wind_tex

func _sample_weather(d: Vector3) -> Dictionary:
	var hum: float = _grid.sample_bilinear(_fields.humidity, d)
	var rain_mm: float = _grid.sample_bilinear(_fields.precip, d)
	var storm: float = _grid.sample_bilinear(_fields.storm_risk, d)
	var temp: float = _grid.sample_bilinear(_fields.temp_mean, d)
	var wu: float = _grid.sample_bilinear(_fields.wind_u, d)
	var wv: float = _grid.sample_bilinear(_fields.wind_v, d)
	var h: float = Planet.macro_height(d)
	var wet: float = clampf(rain_mm / 1800.0, 0.0, 1.2)
	var ocean := h < 0.0

	# Large weather organisation is deliberately independent from the source
	# cloud package. These are deterministic planetary-scale pressure/frontal
	# surrogates whose probability is modulated by Asterra's own climate.
	var syn: float = _synoptic.u(d)
	var frontal: float = clampf((_fronts.s(d) + 1.0) * 0.5, 0.0, 1.0)
	var cellular: float = _cells.u(d)
	var oro: float = _orographic_lift(d, wu, wv, h)

	var moist_score := hum * 0.66 + wet * 0.22 + syn * 0.12
	moist_score += oro * 0.16
	if ocean:
		moist_score += 0.055
	var coverage := NoiseKit.smoothstepf(0.43, 0.88, moist_score)
	coverage *= 0.78 + syn * 0.28
	coverage = clampf(coverage, 0.0, 1.0)

	# Warm humid air + mapped severe-weather risk + terrain lift make vertical
	# clouds. This is how thunderstorms prefer the places the climate generator
	# says should actually produce them instead of being painted at random.
	var warm := NoiseKit.smoothstepf(12.0, 29.0, temp)
	var convective := storm * 0.72 + hum * warm * 0.22 + oro * 0.26
	convective *= 0.72 + cellular * 0.40
	convective = clampf(convective, 0.0, 1.0)

	# Thin upper decks favour storm outflow and broad frontal regions. Mid-latitude
	# wet climates therefore naturally acquire long cirrus shields while tropical
	# convective cells grow anvils.
	var high := storm * 0.48 + frontal * wet * 0.34 + syn * hum * 0.18
	high += convective * 0.22
	high = clampf(high, 0.0, 1.0)

	var precip := clampf(hum * 0.42 + wet * 0.34 + convective * 0.44 - 0.42, 0.0, 1.0)

	# Approximate lifting-condensation level from temperature and RH (Magnus
	# dewpoint followed by the standard ~125 m/K rule). Then keep the cloud base
	# above terrain, which makes the renderer react correctly to mountain ranges.
	var dew := _dewpoint_c(temp, hum)
	var lcl := clampf((temp - dew) * 125.0, 350.0, 3200.0)
	var terrain_floor := maxf(h, 0.0) + lerpf(260.0, 520.0, 1.0 - hum)
	var base := clampf(maxf(lcl, terrain_floor), 450.0, 5000.0)

	return {
		"coverage": coverage,
		"convective": convective,
		"high": high,
		"base": base,
		"wind_u": wu,
		"wind_v": wv,
		"precip": precip,
		"humidity": hum,
	}

func _orographic_lift(d: Vector3, wu: float, wv: float, h: float) -> float:
	var tb := CubeSphere.tangent_basis(d)
	var east: Vector3 = tb[0]
	var north: Vector3 = tb[1]
	var wind := east * wu + north * wv
	if wind.length_squared() < 0.04:
		return 0.0
	var c := _grid.dir_to_index(d)
	var step_angle := _grid.cell_size[c] / maxf(_cfg.planet_radius, 1.0)
	var upwind := (d - wind.normalized() * step_angle).normalized()
	var upwind_h := Planet.macro_height(upwind)
	return clampf((h - upwind_h) / 900.0, 0.0, 1.0)

func _dewpoint_c(temp_c: float, rh: float) -> float:
	var a := 17.625
	var b := 243.04
	var safe_rh := clampf(rh, 0.02, 1.0)
	var gamma := log(safe_rh) + a * temp_c / (b + temp_c)
	return b * gamma / (a - gamma)
