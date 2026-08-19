class_name PassSoil
extends RefCounted
## 1.5 (second third) -- soil.
##
## Soil is derived from bedrock + climate + water + deposition + organics, with a
## continuous composition rather than a soil-type enum. The excavation system
## reads exactly these numbers, so digging a floodplain gives you silt and digging
## a granite hillside gives you thin gritty sand over rock -- without any
## authoring.

const R := PlanetFields.Rock

var cfg: GenConfig
var grid: PlanetGrid
var fields: PlanetFields

func _init(p_fields: PlanetFields) -> void:
	fields = p_fields
	cfg = p_fields.cfg
	grid = p_fields.grid

func run(progress: Callable = Callable()) -> void:
	var n := grid.cell_count
	var s := cfg.stream_seed("soil")
	var patch := NoiseKit.new(s + 1, 9.0, 4)

	for c in n:
		if progress.is_valid() and (c & 0x3FFF) == 0:
			progress.call("Soil", float(c) / float(n))
		var h: float = fields.elev[c]
		var d := grid.cell_dir(c)
		if h < 0.0:
			# Marine sediment still has a composition -- dredging and coastal
			# works will need it.
			fields.soil_depth[c] = clampf(fields.sediment[c] * 0.35, 0.0, 40.0)
			fields.soil_sand[c] = 0.30
			fields.soil_silt[c] = 0.44
			fields.soil_clay[c] = 0.26
			fields.soil_organic[c] = 0.02
			fields.soil_moisture[c] = 1.0
			continue

		var rock: int = fields.rock[c]
		var t: float = fields.temp_mean[c]
		var p: float = fields.precip[c]

		# Chemical weathering intensity: warm and wet weathers fast, cold or dry
		# barely at all.
		var w_temp := clampf((t + 4.0) / 30.0, 0.0, 1.35)
		var w_wet := clampf(p / 1300.0, 0.05, 1.6)
		var weathering := clampf(w_temp * w_wet, 0.0, 1.6)

		# Slope strips soil; flats and floodplains collect it.
		var base := c * 8
		var relief := 0.0
		for k in 8:
			relief = maxf(relief, absf(h - fields.elev[grid.nbr[base + k]]))
		var slope := relief / grid.cell_size[c]
		var retention := clampf(1.0 - slope * 26.0, 0.03, 1.0)

		var depth := (0.18 + 2.6 * weathering) * retention
		depth += fields.floodplain[c] * 3.4
		depth += clampf(fields.sediment[c], 0.0, 60.0) * 0.05 * retention
		depth += fields.wetland[c] * 0.8
		depth *= 0.75 + 0.5 * patch.u(d)
		fields.soil_depth[c] = clampf(depth, 0.0, 14.0)

		# Texture. Clay comes from chemical weathering of the parent rock; sand
		# from quartz-rich or barely-weathered rock; silt from water/wind
		# transport, so floodplains and loess belts are silty.
		var clay_yield: float = PlanetFields.ROCK_CLAY_YIELD[rock]
		var clay := clay_yield * clampf(weathering, 0.1, 1.4)
		var sand := (1.0 - clay_yield) * (1.2 - clampf(weathering, 0.0, 1.0) * 0.55)
		if rock in [R.SANDSTONE, R.QUARTZITE, R.CONGLOMERATE, R.GRANITE]:
			sand *= 1.35
		var silt := 0.24 + fields.floodplain[c] * 0.9 + fields.wetland[c] * 0.25
		silt += clampf(fields.sediment[c] / 90.0, 0.0, 0.5)
		var tot := maxf(1e-4, clay + sand + silt)
		fields.soil_clay[c] = clay / tot
		fields.soil_sand[c] = sand / tot
		fields.soil_silt[c] = silt / tot

		# Organic matter: production rises with warmth and water, decomposition
		# rises with warmth alone -- so peat forms in cold wet ground.
		var production := clampf(w_wet, 0.0, 1.3) * clampf((t + 8.0) / 26.0, 0.0, 1.25)
		var decay := clampf((t + 2.0) / 24.0, 0.06, 1.4)
		var org := production / (0.55 + decay * 1.4)
		org *= 1.0 + fields.wetland[c] * 1.9
		fields.soil_organic[c] = clampf(org * 0.16, 0.0, 0.85)

		# Plant-available water.
		var texture_hold := fields.soil_clay[c] * 0.9 + fields.soil_silt[c] * 0.7 + fields.soil_sand[c] * 0.25
		var m := clampf(p / 1200.0, 0.0, 1.4) * (0.45 + 0.75 * texture_hold) * retention
		m += fields.wetland[c] * 0.6 + fields.aquifer[c] * 0.18
		m -= clampf((t - 22.0) / 30.0, 0.0, 0.35)
		fields.soil_moisture[c] = clampf(m, 0.0, 1.0)

## Vertical horizons at a cell, as (thickness, material id) pairs from the surface
## down. Material ids: 0 = organic O, 1 = topsoil A, 2 = subsoil B, 3 = parent C,
## 4 = bedrock. Consumed by the excavation system.
func horizons(c: int) -> Array:
	var depth: float = fields.soil_depth[c]
	if depth <= 0.02:
		return [[0.0, 4]]
	var o := clampf(fields.soil_organic[c] * 0.55, 0.0, 0.45) * clampf(depth, 0.0, 1.0)
	var a := depth * 0.28
	var b := depth * 0.42
	var cz := maxf(0.0, depth - o - a - b)
	return [[o, 0], [a, 1], [b, 2], [cz, 3], [INF, 4]]
