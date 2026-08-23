class_name PassBiome
extends RefCounted
## 1.5 (final third) -- biomes and vegetation.
##
## Derived from climate, soil, elevation and water availability rather than
## painted biome masks. The classification is a Whittaker-style temperature /
## precipitation space, then overridden by the things that physically dominate a
## location: standing water, permanent ice, wetland, treeline, bare rock.

const B := PlanetFields.Biome
## Biomes whose defining feature is a closed tree canopy, and which therefore
## cannot exist without enough soil to root one.
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

		if h < 0.0:
			fields.biome[c] = B.ICE_CAP if t < -2.0 else (B.SHELF_SEA if h > cfg.shelf_depth - 40.0 else B.OCEAN)
			fields.vegetation[c] = 0.0
			continue
		if fields.lake_level[c] > -1e8:
			fields.biome[c] = B.LAKE
			fields.vegetation[c] = 0.0
			continue
		if fields.river_width[c] >= 60.0:
			fields.biome[c] = B.RIVER
			fields.vegetation[c] = 0.05
			continue

		# Summer warmth matters more than the annual mean for ice and treeline.
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

		# Treeline: above it, forests become alpine regardless of the mean.
		if h > 900.0 and summer < 10.5 and b in [B.TAIGA, B.TEMPERATE_FOREST, B.TEMPERATE_RAINFOREST]:
			b = B.ALPINE

		# Substrate limit. Climate says what could grow here; the ground says what
		# can. A closed canopy needs a rooting depth that a scoured ridge does not
		# have, and the Whittaker scheme -- being a function of temperature and
		# rainfall alone -- will happily put rainforest on bare rock. Where the
		# soil is too thin to root trees, the community that actually occupies
		# that ground is the open one for the same climate.
		if b in FOREST_CLASSES and fields.soil_depth[c] < 0.32:
			if summer < 11.0:
				b = B.TUNDRA
			elif t < 17.0:
				b = B.TEMPERATE_GRASSLAND
			else:
				b = B.SAVANNA
		fields.biome[c] = b

		# Vegetation biomass: water-limited and temperature-limited productivity,
		# scaled by how much soil there is to root in.
		var water_lim := clampf(p / 1600.0, 0.0, 1.0)
		var temp_lim := clampf((t + 6.0) / 26.0, 0.0, 1.0)
		var soil_lim := clampf(fields.soil_depth[c] / 1.4, 0.05, 1.0)
		var veg := water_lim * temp_lim * soil_lim
		if b in [B.HOT_DESERT, B.COLD_DESERT, B.BARE_ROCK, B.ICE_CAP]:
			veg *= 0.12
		fields.vegetation[c] = clampf(veg, 0.0, 1.0)
