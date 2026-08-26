class_name PlanetFields
extends RefCounted
## The baked macro state of Asterra. One flat array per physical quantity over the
## PlanetGrid cell space. This is the canonical world data the roadmap asks for:
## it is *not* a scene graph, and nothing here needs to be rendered to exist.

const FORMAT_VERSION := 4

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
## Dry weathered-outcrop colour per family. This is what an orbital image
## actually shows wherever soil is thin: pale granite highlands, near-black
## basalt provinces, red sandstone, white limestone karst.
const ROCK_COLORS := [
	Color(0.335, 0.305, 0.275),
	Color(0.085, 0.082, 0.080),
	Color(0.105, 0.104, 0.098),
	Color(0.250, 0.240, 0.232),
	Color(0.175, 0.170, 0.158),
	Color(0.365, 0.230, 0.140),
	Color(0.155, 0.146, 0.132),
	Color(0.415, 0.400, 0.352),
	Color(0.375, 0.358, 0.318),
	Color(0.265, 0.222, 0.180),
	Color(0.455, 0.442, 0.422),
	Color(0.330, 0.298, 0.260),
	Color(0.120, 0.145, 0.118),
]

## A biome is the ecological/climatic community occupying the substrate. Rivers
## and lakes are deliberately absent: they are hydrology attributes which can
## cross or occupy any terrestrial biome without destroying the biome underneath.
enum Biome {
	OCEAN, SHELF_SEA, ICE_CAP, TUNDRA, TAIGA, COLD_DESERT,
	TEMPERATE_GRASSLAND, TEMPERATE_FOREST, TEMPERATE_RAINFOREST, MEDITERRANEAN,
	STEPPE, HOT_DESERT, SAVANNA, TROPICAL_SEASONAL_FOREST, TROPICAL_RAINFOREST,
	WETLAND, ALPINE, BARE_ROCK,
}
const BIOME_NAMES := [
	"Ocean", "Shelf sea", "Ice cap", "Tundra", "Taiga", "Cold desert",
	"Temperate grassland", "Temperate forest", "Temperate rainforest", "Mediterranean",
	"Steppe", "Hot desert", "Savanna", "Tropical seasonal forest", "Tropical rainforest",
	"Wetland", "Alpine", "Bare rock",
]
## Visible-band RGB reflectance per biome, linear.
const BIOME_ALBEDO := [
	Color(0.012, 0.026, 0.050),   # ocean
	Color(0.020, 0.058, 0.090),   # shelf sea
	Color(0.720, 0.760, 0.800),   # ice cap
	Color(0.105, 0.100, 0.068),   # tundra
	Color(0.028, 0.055, 0.030),   # taiga
	Color(0.260, 0.245, 0.205),   # cold desert
	Color(0.115, 0.145, 0.055),   # temperate grassland
	Color(0.035, 0.095, 0.032),   # temperate forest
	Color(0.026, 0.072, 0.030),   # temperate rainforest
	Color(0.105, 0.110, 0.055),   # mediterranean scrub
	Color(0.185, 0.165, 0.080),   # steppe
	Color(0.380, 0.300, 0.180),   # hot desert
	Color(0.215, 0.185, 0.080),   # savanna
	Color(0.045, 0.105, 0.038),   # tropical seasonal forest
	Color(0.026, 0.080, 0.028),   # tropical rainforest
	Color(0.055, 0.085, 0.048),   # wetland
	Color(0.245, 0.240, 0.230),   # alpine scree
	Color(0.215, 0.205, 0.190),   # bare rock
]
const BIOME_COLORS := [
	Color(0.05, 0.13, 0.33), Color(0.10, 0.30, 0.50),
	Color(0.93, 0.96, 1.00), Color(0.62, 0.66, 0.58), Color(0.16, 0.33, 0.22),
	Color(0.62, 0.60, 0.52), Color(0.62, 0.68, 0.34), Color(0.20, 0.44, 0.20),
	Color(0.12, 0.40, 0.26), Color(0.55, 0.58, 0.26), Color(0.68, 0.63, 0.36),
	Color(0.83, 0.73, 0.47), Color(0.72, 0.66, 0.28), Color(0.32, 0.50, 0.20),
	Color(0.10, 0.36, 0.16), Color(0.28, 0.44, 0.34), Color(0.55, 0.55, 0.58),
	Color(0.44, 0.42, 0.40),
]

## Physical plate-margin kinematics. This is separate from geology/fault
## intensity: a transform margin can have a very strong fault while almost no
## normal uplift.
enum TectonicBoundary {
	INTERIOR, CONVERGENT, DIVERGENT, TRANSFORM,
}
const TECTONIC_BOUNDARY_NAMES := ["Interior", "Convergent", "Divergent", "Transform"]
const TECTONIC_BOUNDARY_COLORS := [
	Color(0.14, 0.14, 0.16), Color(0.95, 0.28, 0.18),
	Color(0.20, 0.55, 0.95), Color(0.95, 0.72, 0.20),
]

## Exceptional persistent landforms. A landmark is metadata layered on top of
## terrain, geology, hydrology and biome rather than another biome category.
enum Landmark {
	NONE, STRATOVOLCANO, SHIELD_VOLCANO, VOLCANIC_ISLAND, RIFT_VOLCANIC_FIELD,
}
const LANDMARK_NAMES := [
	"None", "Stratovolcano", "Shield volcano", "Volcanic island", "Rift volcanic field",
]
const LANDMARK_COLORS := [
	Color(0.08, 0.08, 0.09), Color(0.95, 0.22, 0.08), Color(0.92, 0.48, 0.12),
	Color(0.90, 0.12, 0.48), Color(0.72, 0.25, 0.92),
]

enum HydrologyAttribute {
	RIVER = 1,
	LAKE = 2,
	FLOODPLAIN = 4,
	WETLAND = 8,
}

var cfg: GenConfig
var grid: PlanetGrid

# --- macro geography ---
var plate := PackedByteArray()
var plate_boundary := PackedFloat32Array()   ## 0..1 proximity to a plate margin
var plate_boundary_type := PackedByteArray() ## TectonicBoundary
var uplift := PackedFloat32Array()           ## signed normal plate-motion forcing
var base_elev := PackedFloat32Array()        ## pre-erosion elevation, m rel. sea level
var elev := PackedFloat32Array()             ## post-erosion elevation, m rel. sea level

# --- exceptional landforms ---
var landmark := PackedByteArray()            ## Landmark kind, NONE outside influence
var landmark_id := PackedInt32Array()        ## 0 none; positive stable generated feature id
var landmark_strength := PackedFloat32Array() ## 0..1 local influence/core proximity

# --- geology ---
var rock := PackedByteArray()                ## surface bedrock family
var strata_phase := PackedFloat32Array()     ## vertical offset of the layer stack, m
var strata_dip := PackedFloat32Array()       ## radians of bedding dip
var erodibility := PackedFloat32Array()      ## continuous rock erodibility
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
	"plate_boundary", "uplift", "base_elev", "elev", "landmark_strength",
	"strata_phase", "strata_dip", "erodibility", "fault", "basin",
	"ore_iron", "ore_copper", "coal", "petroleum", "gas_fraction", "quartz", "aquifer",
	"sediment", "flow_accum", "discharge", "lake_level", "river_width", "floodplain", "wetland",
	"temp_mean", "temp_range", "precip", "humidity", "wind_u", "wind_v", "storm_risk",
	"soil_depth", "soil_sand", "soil_silt", "soil_clay", "soil_organic", "soil_moisture",
	"vegetation", "relief", "suitability", "corridor",
]
const BYTE_FIELDS := [
	"plate", "plate_boundary_type", "landmark", "rock", "flow_dir", "stream_order", "biome",
]
const INT_FIELDS := ["landmark_id", "watershed"]

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
	for field_name in FLOAT_FIELDS:
		f.store_var(get(field_name))
	for field_name in BYTE_FIELDS:
		f.store_var(get(field_name))
	for field_name in INT_FIELDS:
		f.store_var(get(field_name))
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
	var loaded_grid := PlanetGrid.new(res, radius)
	var fields := PlanetFields.new(p_cfg, loaded_grid)
	for field_name in FLOAT_FIELDS:
		fields.set(field_name, f.get_var())
	for field_name in BYTE_FIELDS:
		fields.set(field_name, f.get_var())
	for field_name in INT_FIELDS:
		fields.set(field_name, f.get_var())
	f.close()
	return fields

func is_lake(c: int) -> bool:
	return lake_level[c] > -1e8

func has_river(c: int, min_width_m: float = 0.0) -> bool:
	return river_width[c] > min_width_m

func hydrology_flags(c: int) -> int:
	var flags := 0
	if has_river(c):
		flags |= HydrologyAttribute.RIVER
	if is_lake(c):
		flags |= HydrologyAttribute.LAKE
	if floodplain[c] > 0.15:
		flags |= HydrologyAttribute.FLOODPLAIN
	if wetland[c] > 0.15:
		flags |= HydrologyAttribute.WETLAND
	return flags

func is_water(c: int) -> bool:
	return elev[c] < 0.0 or is_lake(c)

func surface_level(c: int) -> float:
	if is_lake(c):
		return lake_level[c]
	return 0.0 if elev[c] < 0.0 else elev[c]
