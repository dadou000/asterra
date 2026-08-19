class_name PlanetFields
extends RefCounted
## The baked macro state of Asterra. One flat array per physical quantity over the
## PlanetGrid cell space. This is the canonical world data the roadmap asks for:
## it is *not* a scene graph, and nothing here needs to be rendered to exist.

const FORMAT_VERSION := 3

## Bedrock families (1.3 "geology first").
enum Rock {
	GRANITE, BASALT, GABBRO, GNEISS, SCHIST, SANDSTONE, SHALE,
	LIMESTONE, DOLOMITE, CONGLOMERATE, QUARTZITE, RHYOLITE_TUFF, SERPENTINITE,
}
const ROCK_NAMES := [
	"Granite", "Basalt", "Gabbro", "Gneiss", "Schist", "Sandstone", "Shale",
	"Limestone", "Dolomite", "Conglomerate", "Quartzite", "Rhyolitic tuff", "Serpentinite",
]
## Erodibility multiplier per family (soft sediments strip fast, quartzite resists).
const ROCK_ERODIBILITY := [
	0.65, 0.80, 0.60, 0.55, 0.90, 1.45, 1.85, 1.30, 1.05, 1.25, 0.35, 1.60, 1.10,
]
## Fraction of the rock that weathers to clay (vs sand) -- drives soil texture.
const ROCK_CLAY_YIELD := [
	0.22, 0.34, 0.30, 0.26, 0.52, 0.14, 0.72, 0.40, 0.36, 0.20, 0.06, 0.55, 0.48,
]

enum Biome {
	OCEAN, SHELF_SEA, LAKE, ICE_CAP, TUNDRA, TAIGA, COLD_DESERT,
	TEMPERATE_GRASSLAND, TEMPERATE_FOREST, TEMPERATE_RAINFOREST, MEDITERRANEAN,
	STEPPE, HOT_DESERT, SAVANNA, TROPICAL_SEASONAL_FOREST, TROPICAL_RAINFOREST,
	WETLAND, ALPINE, BARE_ROCK, RIVER,
}
const BIOME_NAMES := [
	"Ocean", "Shelf sea", "Lake", "Ice cap", "Tundra", "Taiga", "Cold desert",
	"Temperate grassland", "Temperate forest", "Temperate rainforest", "Mediterranean",
	"Steppe", "Hot desert", "Savanna", "Tropical seasonal forest", "Tropical rainforest",
	"Wetland", "Alpine", "Bare rock", "River",
]
const BIOME_COLORS := [
	Color(0.05, 0.13, 0.33), Color(0.10, 0.30, 0.50), Color(0.16, 0.38, 0.62),
	Color(0.93, 0.96, 1.00), Color(0.62, 0.66, 0.58), Color(0.16, 0.33, 0.22),
	Color(0.62, 0.60, 0.52), Color(0.62, 0.68, 0.34), Color(0.20, 0.44, 0.20),
	Color(0.12, 0.40, 0.26), Color(0.55, 0.58, 0.26), Color(0.68, 0.63, 0.36),
	Color(0.83, 0.73, 0.47), Color(0.72, 0.66, 0.28), Color(0.32, 0.50, 0.20),
	Color(0.10, 0.36, 0.16), Color(0.28, 0.44, 0.34), Color(0.55, 0.55, 0.58),
	Color(0.44, 0.42, 0.40), Color(0.20, 0.42, 0.66),
]

var cfg: GenConfig
var grid: PlanetGrid

# --- macro geography ---
var plate := PackedByteArray()
var plate_boundary := PackedFloat32Array()   ## 0..1 proximity to a plate margin
var uplift := PackedFloat32Array()           ## m/Myr orogenic forcing
var base_elev := PackedFloat32Array()        ## pre-erosion elevation, m rel. sea level
var elev := PackedFloat32Array()             ## post-erosion elevation, m rel. sea level

# --- geology ---
var rock := PackedByteArray()                ## surface bedrock family
var strata_phase := PackedFloat32Array()     ## vertical offset of the layer stack, m
var strata_dip := PackedFloat32Array()       ## radians of bedding dip
var erodibility := PackedFloat32Array()      ## continuous rock erodibility (bilinear-safe copy of ROCK_ERODIBILITY)
var fault := PackedFloat32Array()            ## 0..1 fault/fold intensity
var basin := PackedFloat32Array()            ## 0..1 sedimentary basin membership
var ore_iron := PackedFloat32Array()
var ore_copper := PackedFloat32Array()
var coal := PackedFloat32Array()
var petroleum := PackedFloat32Array()
var gas_fraction := PackedFloat32Array()
var quartz := PackedFloat32Array()           ## high-purity silica (Axiom)
var aquifer := PackedFloat32Array()          ## groundwater-bearing potential

# --- erosion / hydrology ---
var sediment := PackedFloat32Array()         ## unconsolidated deposit thickness, m
var flow_dir := PackedByteArray()            ## 0..7 neighbour index, 255 = terminal
var flow_accum := PackedFloat32Array()       ## upstream drainage area, m^2
var discharge := PackedFloat32Array()        ## mean annual discharge, m^3/s
var watershed := PackedInt32Array()          ## outlet cell index
var stream_order := PackedByteArray()        ## Strahler order
var lake_level := PackedFloat32Array()       ## water surface elevation, or -1e9
var river_width := PackedFloat32Array()      ## m
var floodplain := PackedFloat32Array()       ## 0..1
var wetland := PackedFloat32Array()          ## 0..1

# --- climate ---
var temp_mean := PackedFloat32Array()        ## deg C
var temp_range := PackedFloat32Array()       ## seasonal amplitude, deg C
var precip := PackedFloat32Array()           ## mm/yr
var humidity := PackedFloat32Array()         ## 0..1
var wind_u := PackedFloat32Array()           ## east component m/s
var wind_v := PackedFloat32Array()           ## north component m/s
var storm_risk := PackedFloat32Array()       ## 0..1 severe weather likelihood

# --- soil ---
var soil_depth := PackedFloat32Array()       ## m of soil profile above bedrock
var soil_sand := PackedFloat32Array()
var soil_silt := PackedFloat32Array()
var soil_clay := PackedFloat32Array()
var soil_organic := PackedFloat32Array()
var soil_moisture := PackedFloat32Array()

# --- derived ---
var biome := PackedByteArray()
var vegetation := PackedFloat32Array()       ## 0..1 biomass density
var relief := PackedFloat32Array()           ## local elevation range across the 8-neighbourhood, m
var suitability := PackedFloat32Array()      ## 0..1 general buildability
var corridor := PackedFloat32Array()         ## 0..1 natural transport corridor score

var sea_level: float = 0.0                   ## always 0 by construction; kept explicit

const FLOAT_FIELDS := [
	"plate_boundary", "uplift", "base_elev", "elev",
	"strata_phase", "strata_dip", "erodibility", "fault", "basin",
	"ore_iron", "ore_copper", "coal", "petroleum", "gas_fraction", "quartz", "aquifer",
	"sediment", "flow_accum", "discharge", "lake_level", "river_width", "floodplain", "wetland",
	"temp_mean", "temp_range", "precip", "humidity", "wind_u", "wind_v", "storm_risk",
	"soil_depth", "soil_sand", "soil_silt", "soil_clay", "soil_organic", "soil_moisture",
	"vegetation", "relief", "suitability", "corridor",
]
const BYTE_FIELDS := ["plate", "rock", "flow_dir", "stream_order", "biome"]
const INT_FIELDS := ["watershed"]

func _init(p_cfg: GenConfig, p_grid: PlanetGrid) -> void:
	cfg = p_cfg
	grid = p_grid
	var n := grid.cell_count
	for f in FLOAT_FIELDS:
		var a := PackedFloat32Array()
		a.resize(n)
		set(f, a)
	for f in BYTE_FIELDS:
		var a := PackedByteArray()
		a.resize(n)
		set(f, a)
	for f in INT_FIELDS:
		var a := PackedInt32Array()
		a.resize(n)
		set(f, a)

func save_to(path: String) -> Error:
	DirAccess.make_dir_recursive_absolute(path.get_base_dir())
	var f := FileAccess.open_compressed(path, FileAccess.WRITE, FileAccess.COMPRESSION_ZSTD)
	if f == null:
		return FileAccess.get_open_error()
	f.store_32(FORMAT_VERSION)
	f.store_32(grid.res)
	f.store_double(grid.radius)
	f.store_string(cfg.cache_key())
	f.store_8(0)
	for name in FLOAT_FIELDS:
		f.store_var(get(name))
	for name in BYTE_FIELDS:
		f.store_var(get(name))
	for name in INT_FIELDS:
		f.store_var(get(name))
	f.close()
	return OK

static func load_from(path: String, p_cfg: GenConfig) -> PlanetFields:
	if not FileAccess.file_exists(path):
		return null
	var f := FileAccess.open_compressed(path, FileAccess.READ, FileAccess.COMPRESSION_ZSTD)
	if f == null:
		return null
	if f.get_32() != FORMAT_VERSION:
		return null
	var res := f.get_32()
	var radius := f.get_double()
	var key := f.get_line()
	if res != p_cfg.face_res or absf(radius - p_cfg.planet_radius) > 0.5 or key != p_cfg.cache_key():
		return null
	var grid := PlanetGrid.new(res, radius)
	var fields := PlanetFields.new(p_cfg, grid)
	for name in FLOAT_FIELDS:
		fields.set(name, f.get_var())
	for name in BYTE_FIELDS:
		fields.set(name, f.get_var())
	for name in INT_FIELDS:
		fields.set(name, f.get_var())
	f.close()
	return fields

func is_water(c: int) -> bool:
	return elev[c] < 0.0 or lake_level[c] > -1e8

func surface_level(c: int) -> float:
	if lake_level[c] > -1e8:
		return lake_level[c]
	return maxf(elev[c], 0.0) if elev[c] < 0.0 else elev[c]
