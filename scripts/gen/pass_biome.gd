class_name PassBiome
extends RefCounted
## Biomes and vegetation.
##
## Biome is an ecological/climate classification. Hydrology is orthogonal: a
## river can cross tundra, forest or desert, and a lake can occupy a depression
## inside any of those regions. River/lake state therefore never overwrites the
## underlying biome here.

const B := PlanetFields.Biome
const FOREST_CLASSES := [
	B.TAIGA, B.TEMPERATE_FOREST, B.TEMPERATE_RAINFOREST,
	B.TROPICAL_SEASONAL_FOREST, B.TROPICAL_RAINFOREST,
]

var cfg: GenConfig
var grid: PlanetGrid
var fields: PlanetFields

func _init(p_fields: PlanetFields) -> void:
	fields = p_fields
	cfg = p_fields.cfg
	grid = p_fields.grid

func run(progress: Callable = Callable()) -> void:
	var n := grid.cell_count
	for c in n:
		if progress.is_valid() and (c & 0x3FFF) == 0:
			progress.call("Biomes", float(c) / float(n))
		var h: float = fields.elev[c]
		var t: float = fields.temp_mean[c]
		var p: float = fields.precip[c]

		# Marine biomes remain true base surface classes. Inland surface water is
		# an attribute and does not replace the terrestrial biome underneath it.
		if h < 0.0:
			fields.biome[c] = B.ICE_CAP if t < -2.0 else (
				B.SHELF_SEA if h > cfg.shelf_depth - 40.0 else B.OCEAN)
			fields.vegetation[c] = 0.0
			continue

		var summer := t + fields.temp_range[c] * 0.5
		var b: int
		if summer < -1.0:
			b = B.ICE_CAP
		elif fields.wetland[c] > 0.62 and t > -4.0:
			b = B.WETLAND
		elif summer < 7.0:
			b = B.TUNDRA
		elif fields.soil_depth[c] < 0.10:
			b = B.BARE_ROCK
		elif t < 2.0:
			b = B.TAIGA if p > 300.0 else B.COLD_DESERT
		elif t < 8.0:
			if p < 260.0:
				b = B.COLD_DESERT
			elif p < 550.0:
				b = B.STEPPE
			else:
				b = B.TAIGA
		elif t < 17.0:
			if p < 280.0:
				b = B.COLD_DESERT
			elif p < 600.0:
				b = B.TEMPERATE_GRASSLAND
			elif p < 1500.0:
				b = B.MEDITERRANEAN if fields.temp_range[c] > 19.0 and p < 850.0 else B.TEMPERATE_FOREST
			else:
				b = B.TEMPERATE_RAINFOREST
		else:
			if p < 320.0:
				b = B.HOT_DESERT
			elif p < 850.0:
				b = B.SAVANNA
			elif p < 1700.0:
				b = B.TROPICAL_SEASONAL_FOREST
			else:
				b = B.TROPICAL_RAINFOREST

		if h > 900.0 and summer < 10.5 and b in [
			B.TAIGA, B.TEMPERATE_FOREST, B.TEMPERATE_RAINFOREST]:
			b = B.ALPINE

		if b in FOREST_CLASSES and fields.soil_depth[c] < 0.32:
			if summer < 11.0:
				b = B.TUNDRA
			elif t < 17.0:
				b = B.TEMPERATE_GRASSLAND
			else:
				b = B.SAVANNA
		fields.biome[c] = b

		var water_lim := clampf(p / 1600.0, 0.0, 1.0)
		var temp_lim := clampf((t + 6.0) / 26.0, 0.0, 1.0)
		var soil_lim := clampf(fields.soil_depth[c] / 1.4, 0.05, 1.0)
		var veg := water_lim * temp_lim * soil_lim
		if b in [B.HOT_DESERT, B.COLD_DESERT, B.BARE_ROCK, B.ICE_CAP]:
			veg *= 0.12

		# Surface-water coverage affects biomass without changing biome identity.
		if fields.is_lake(c):
			veg = 0.0
		elif fields.river_width[c] > 0.0:
			var river_cover := clampf(fields.river_width[c] / maxf(grid.cell_size[c], 1.0), 0.0, 0.95)
			veg *= 1.0 - river_cover
		fields.vegetation[c] = clampf(veg, 0.0, 1.0)
