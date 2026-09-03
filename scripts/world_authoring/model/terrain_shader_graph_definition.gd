class_name TerrainShaderGraphDefinition
extends Resource
## Serializable node graph used by terrain displacement/material authoring slots.
##
## Production terrain stages are represented by ordinary serialized graph nodes.
## Control-node defaults mirror the production shaders exactly so Reset Flow is a
## lossless representation of the renderer rather than an approximate preset.

enum Domain {
	DISPLACEMENT,
	MATERIAL,
}

enum DisplacementOutputMode {
	DELTA,
	ABSOLUTE_HEIGHT,
}

const CATEGORY_OUTPUT := "Output"
const CATEGORY_PRODUCTION := "Production stages"
const CATEGORY_TERRAIN_SOURCE := "Terrain source"
const CATEGORY_GEOMORPH := "Geomorph"
const CATEGORY_WORLD_DATA := "World data"
const CATEGORY_MASKS := "Masks"
const CATEGORY_CLASSIFICATION := "Classification"
const CATEGORY_PALETTE := "Palette / materials"
const CATEGORY_MICRODETAIL := "Microdetail"
const CATEGORY_SURFACE_PBR := "Surface / PBR"
const CATEGORY_TEXTURES := "Textures"
const CATEGORY_MATH := "Math"
const CATEGORY_UTILITY := "Utility"

const NODE_CATEGORIES: Array[String] = [
	CATEGORY_OUTPUT,
	CATEGORY_PRODUCTION,
	CATEGORY_TERRAIN_SOURCE,
	CATEGORY_GEOMORPH,
	CATEGORY_WORLD_DATA,
	CATEGORY_MASKS,
	CATEGORY_CLASSIFICATION,
	CATEGORY_PALETTE,
	CATEGORY_MICRODETAIL,
	CATEGORY_SURFACE_PBR,
	CATEGORY_TEXTURES,
	CATEGORY_MATH,
	CATEGORY_UTILITY,
]

const MATERIAL_GAME_INPUTS: Array[String] = [
	"base_albedo",
	"base_normal",
	"base_roughness",
	"base_metallic",
	"base_ao",
	"base_specular",
	"world_position",
	"planet_direction",
	"terrain_height_m",
	"surface_normal",
	"slope",
	"biome_id",
	"temperature",
	"precipitation",
	"temperature_range",
	"moisture",
	"vegetation_biomass",
	"soil_sand",
	"soil_silt",
	"soil_clay",
	"soil_depth_m",
	"surface_sediment_m",
	"rock_id",
	"erodibility",
	"strata_dip",
	"uplift",
	"flow_x",
	"flow_y",
	"hydrology",
	"soil",
	"surface",
	"geology",
	"structure",
	"climate",
	"landform",
	"material_primary",
	"material_secondary",
	"rock_mix",
	"micro_layer",
	"camera_distance_m",
	"clipmap_level",
	"time_s",
]

const DISPLACEMENT_GAME_INPUTS: Array[String] = [
	"generated_height_m",
	"sculpt_delta_m",
	"terrain_height_m",
	"biome_id",
	"clipmap_level",
	"time_s",
]

const GAME_INPUTS: Array[String] = [
	"generated_height_m",
	"sculpt_delta_m",
	"base_albedo",
	"base_normal",
	"base_roughness",
	"base_metallic",
	"base_ao",
	"base_specular",
	"world_position",
	"planet_direction",
	"terrain_height_m",
	"surface_normal",
	"slope",
	"biome_id",
	"temperature",
	"precipitation",
	"temperature_range",
	"moisture",
	"vegetation_biomass",
	"soil_sand",
	"soil_silt",
	"soil_clay",
	"soil_depth_m",
	"surface_sediment_m",
	"rock_id",
	"erodibility",
	"strata_dip",
	"uplift",
	"flow_x",
	"flow_y",
	"hydrology",
	"soil",
	"surface",
	"geology",
	"structure",
	"climate",
	"landform",
	"material_primary",
	"material_secondary",
	"rock_mix",
	"micro_layer",
	"camera_distance_m",
	"clipmap_level",
	"time_s",
]

const DISPLACEMENT_ONLY_NODES: Array[String] = [
	"PRODUCTION_GENERATED_HEIGHT",
	"PRODUCTION_GEOMORPH_SETTINGS",
	"PRODUCTION_SCULPT_DELTA",
	"LATITUDE_MASK",
	"NOISE_LAYER",
	"RIDGED_MOUNTAINS",
	"EROSION_CHANNELS",
	"SEDIMENT_DEPOSIT",
	"TERRACE_RELIEF",
	"BILLOW_NOISE",
	"VORONOI_RIDGES",
	"OUTPUT_DISPLACEMENT",
]

const MATERIAL_ONLY_NODES: Array[String] = [
	"PRODUCTION_ALBEDO",
	"PRODUCTION_NORMAL",
	"PRODUCTION_ROUGHNESS",
	"PRODUCTION_METALLIC",
	"PRODUCTION_AO",
	"PRODUCTION_SPECULAR",
	"PRODUCTION_CLASSIFIER_SETTINGS",
	"PRODUCTION_CLASSIFIER_THRESHOLDS",
	"PRODUCTION_SURFACE_PALETTE",
	"PRODUCTION_MICRORELIEF_SETTINGS",
	"PRODUCTION_ANTITILE_SETTINGS",
	"PRODUCTION_ROCK_PBR_SETTINGS",
	"PRODUCTION_SCAN_PBR_SETTINGS",
	"PRODUCTION_SCAN_TEXTURES",
	"CLASSIFIER_PRIMARY",
	"CLASSIFIER_SECONDARY",
	"CHANNEL_R",
	"CHANNEL_G",
	"CHANNEL_B",
	"CHANNEL_A",
	"COMBINE_RGB",
	"NORMAL_BLEND",
	"TRIPLANAR",
	"OUTPUT_MATERIAL",
]

const PRODUCTION_CONTROL_NODES: Array[String] = [
	"PRODUCTION_GEOMORPH_SETTINGS",
	"PRODUCTION_CLASSIFIER_SETTINGS",
	"PRODUCTION_CLASSIFIER_THRESHOLDS",
	"PRODUCTION_SURFACE_PALETTE",
	"PRODUCTION_MICRORELIEF_SETTINGS",
	"PRODUCTION_ANTITILE_SETTINGS",
	"PRODUCTION_ROCK_PBR_SETTINGS",
	"PRODUCTION_SCAN_PBR_SETTINGS",
	"PRODUCTION_SCAN_TEXTURES",
]

const NODE_TYPES: Array[String] = [
	"PRODUCTION_GENERATED_HEIGHT",
	"PRODUCTION_GEOMORPH_SETTINGS",
	"PRODUCTION_SCULPT_DELTA",
	"PRODUCTION_ALBEDO",
	"PRODUCTION_NORMAL",
	"PRODUCTION_ROUGHNESS",
	"PRODUCTION_METALLIC",
	"PRODUCTION_AO",
	"PRODUCTION_SPECULAR",
	"PRODUCTION_CLASSIFIER_SETTINGS",
	"PRODUCTION_CLASSIFIER_THRESHOLDS",
	"PRODUCTION_SURFACE_PALETTE",
	"PRODUCTION_MICRORELIEF_SETTINGS",
	"PRODUCTION_ANTITILE_SETTINGS",
	"PRODUCTION_ROCK_PBR_SETTINGS",
	"PRODUCTION_SCAN_PBR_SETTINGS",
	"PRODUCTION_SCAN_TEXTURES",
	"CLASSIFIER_PRIMARY",
	"CLASSIFIER_SECONDARY",
	"GAME_INPUT",
	"TEXTURE_2D",
	"CONSTANT_FLOAT",
	"CONSTANT_COLOR",
	"ADD",
	"SUBTRACT",
	"MULTIPLY",
	"DIVIDE",
	"MIN",
	"MAX",
	"ABS",
	"POWER",
	"CLAMP",
	"SATURATE",
	"ONE_MINUS",
	"SMOOTHSTEP",
	"REMAP",
	"MIX",
	"CHANNEL_R",
	"CHANNEL_G",
	"CHANNEL_B",
	"CHANNEL_A",
	"COMBINE_RGB",
	"NORMAL_BLEND",
	"NOISE",
	"LATITUDE_MASK",
	"NOISE_LAYER",
	"RIDGED_MOUNTAINS",
	"EROSION_CHANNELS",
	"SEDIMENT_DEPOSIT",
	"TERRACE_RELIEF",
	"BILLOW_NOISE",
	"VORONOI_RIDGES",
	"TRIPLANAR",
	"OUTPUT_DISPLACEMENT",
	"OUTPUT_MATERIAL",
]

const NODE_CATEGORY_BY_TYPE: Dictionary = {
	"OUTPUT_DISPLACEMENT": CATEGORY_OUTPUT,
	"OUTPUT_MATERIAL": CATEGORY_OUTPUT,
	"PRODUCTION_GENERATED_HEIGHT": CATEGORY_PRODUCTION,
	"PRODUCTION_SCULPT_DELTA": CATEGORY_PRODUCTION,
	"PRODUCTION_ALBEDO": CATEGORY_PRODUCTION,
	"PRODUCTION_NORMAL": CATEGORY_PRODUCTION,
	"PRODUCTION_ROUGHNESS": CATEGORY_PRODUCTION,
	"PRODUCTION_METALLIC": CATEGORY_PRODUCTION,
	"PRODUCTION_AO": CATEGORY_PRODUCTION,
	"PRODUCTION_SPECULAR": CATEGORY_PRODUCTION,
	"PRODUCTION_GEOMORPH_SETTINGS": CATEGORY_GEOMORPH,
	"PRODUCTION_CLASSIFIER_SETTINGS": CATEGORY_CLASSIFICATION,
	"PRODUCTION_CLASSIFIER_THRESHOLDS": CATEGORY_CLASSIFICATION,
	"PRODUCTION_SURFACE_PALETTE": CATEGORY_PALETTE,
	"PRODUCTION_MICRORELIEF_SETTINGS": CATEGORY_MICRODETAIL,
	"PRODUCTION_ANTITILE_SETTINGS": CATEGORY_MICRODETAIL,
	"PRODUCTION_ROCK_PBR_SETTINGS": CATEGORY_SURFACE_PBR,
	"PRODUCTION_SCAN_PBR_SETTINGS": CATEGORY_SURFACE_PBR,
	"PRODUCTION_SCAN_TEXTURES": CATEGORY_TEXTURES,
	"CLASSIFIER_PRIMARY": CATEGORY_CLASSIFICATION,
	"CLASSIFIER_SECONDARY": CATEGORY_CLASSIFICATION,
	"GAME_INPUT": CATEGORY_WORLD_DATA,
	"NOISE": CATEGORY_TERRAIN_SOURCE,
	"LATITUDE_MASK": CATEGORY_MASKS,
	"NOISE_LAYER": CATEGORY_GEOMORPH,
	"RIDGED_MOUNTAINS": CATEGORY_GEOMORPH,
	"EROSION_CHANNELS": CATEGORY_GEOMORPH,
	"SEDIMENT_DEPOSIT": CATEGORY_GEOMORPH,
	"TERRACE_RELIEF": CATEGORY_GEOMORPH,
	"BILLOW_NOISE": CATEGORY_GEOMORPH,
	"VORONOI_RIDGES": CATEGORY_GEOMORPH,
	"TEXTURE_2D": CATEGORY_TEXTURES,
	"TRIPLANAR": CATEGORY_TEXTURES,
	"NORMAL_BLEND": CATEGORY_SURFACE_PBR,
	"COMBINE_RGB": CATEGORY_SURFACE_PBR,
	"CHANNEL_R": CATEGORY_UTILITY,
	"CHANNEL_G": CATEGORY_UTILITY,
	"CHANNEL_B": CATEGORY_UTILITY,
	"CHANNEL_A": CATEGORY_UTILITY,
	"ADD": CATEGORY_MATH,
	"SUBTRACT": CATEGORY_MATH,
	"MULTIPLY": CATEGORY_MATH,
	"DIVIDE": CATEGORY_MATH,
	"MIN": CATEGORY_MATH,
	"MAX": CATEGORY_MATH,
	"ABS": CATEGORY_MATH,
	"POWER": CATEGORY_MATH,
	"CLAMP": CATEGORY_MATH,
	"SATURATE": CATEGORY_MATH,
	"ONE_MINUS": CATEGORY_MATH,
	"SMOOTHSTEP": CATEGORY_MATH,
	"REMAP": CATEGORY_MATH,
	"MIX": CATEGORY_MATH,
	"CONSTANT_FLOAT": CATEGORY_UTILITY,
	"CONSTANT_COLOR": CATEGORY_UTILITY,
}

@export var graph_id: String = ""
@export var display_name: String = "Terrain Graph"
@export_enum("Displacement", "Material") var domain: int = Domain.DISPLACEMENT
@export_enum("Additive delta", "Absolute production height") var displacement_output_mode: int = DisplacementOutputMode.DELTA
@export var revision: int = 1
@export var nodes: Array[Dictionary] = []
@export var links: Array[Dictionary] = []

func ensure_valid() -> void:
	if graph_id.is_empty():
		graph_id = make_graph_id(display_name)
	if nodes.is_empty():
		create_default_graph(domain)
	_remove_invalid_links()

func create_default_graph(next_domain: int) -> void:
	domain = next_domain
	if domain == Domain.DISPLACEMENT:
		displacement_output_mode = DisplacementOutputMode.DELTA
	nodes.clear()
	links.clear()
	var output_type := "OUTPUT_DISPLACEMENT" if domain == Domain.DISPLACEMENT else "OUTPUT_MATERIAL"
	add_node(output_type, Vector2(520.0, 180.0), {})
	revision += 1

func create_production_graph(next_domain: int) -> void:
	domain = next_domain
	nodes.clear()
	links.clear()
	if domain == Domain.DISPLACEMENT:
		displacement_output_mode = DisplacementOutputMode.ABSOLUTE_HEIGHT
		var source_id: String = add_node("GAME_INPUT", Vector2(90.0, 180.0),
			{"source":"terrain_height_m"})
		var output_id: String = add_node("OUTPUT_DISPLACEMENT", Vector2(520.0, 180.0), {})
		connect_nodes(source_id, 0, output_id, 0)
	else:
		displacement_output_mode = DisplacementOutputMode.DELTA
		var output_id: String = add_node("OUTPUT_MATERIAL", Vector2(650.0, 210.0), {})
		var sources: Array[String] = [
			"base_albedo", "base_normal", "base_roughness",
			"base_metallic", "base_ao", "base_specular",
		]
		for port: int in sources.size():
			var source_id: String = add_node("GAME_INPUT",
				Vector2(90.0, 55.0 + float(port) * 96.0), {"source":sources[port]})
			connect_nodes(source_id, 0, output_id, port)
	revision += 1

func create_production_stage_graph(next_domain: int) -> void:
	domain = next_domain
	nodes.clear()
	links.clear()
	if domain == Domain.DISPLACEMENT:
		displacement_output_mode = DisplacementOutputMode.ABSOLUTE_HEIGHT
		var generated_id: String = add_node("PRODUCTION_GENERATED_HEIGHT",
			Vector2(70.0, 100.0), {})
		add_node("PRODUCTION_GEOMORPH_SETTINGS", Vector2(70.0, 300.0),
			production_control_defaults("PRODUCTION_GEOMORPH_SETTINGS"))
		var sculpt_id: String = add_node("PRODUCTION_SCULPT_DELTA",
			Vector2(70.0, 650.0), {})
		var add_id: String = add_node("ADD", Vector2(430.0, 205.0), {})
		var output_id: String = add_node("OUTPUT_DISPLACEMENT", Vector2(760.0, 205.0), {})
		connect_nodes(generated_id, 0, add_id, 0)
		connect_nodes(sculpt_id, 0, add_id, 1)
		connect_nodes(add_id, 0, output_id, 0)
	else:
		displacement_output_mode = DisplacementOutputMode.DELTA
		var output_id: String = add_node("OUTPUT_MATERIAL", Vector2(740.0, 245.0), {})
		var source_types: Array[String] = [
			"PRODUCTION_ALBEDO",
			"PRODUCTION_NORMAL",
			"PRODUCTION_ROUGHNESS",
			"PRODUCTION_METALLIC",
			"PRODUCTION_AO",
			"PRODUCTION_SPECULAR",
		]
		for port: int in source_types.size():
			var source_id: String = add_node(source_types[port],
				Vector2(70.0, 55.0 + float(port) * 96.0), {})
			connect_nodes(source_id, 0, output_id, port)
		add_node("PRODUCTION_CLASSIFIER_SETTINGS", Vector2(1080.0, 30.0),
			production_control_defaults("PRODUCTION_CLASSIFIER_SETTINGS"))
		add_node("PRODUCTION_CLASSIFIER_THRESHOLDS", Vector2(1080.0, 500.0),
			production_control_defaults("PRODUCTION_CLASSIFIER_THRESHOLDS"))
		add_node("PRODUCTION_SURFACE_PALETTE", Vector2(1080.0, 1050.0),
			production_control_defaults("PRODUCTION_SURFACE_PALETTE"))
		add_node("PRODUCTION_MICRORELIEF_SETTINGS", Vector2(1440.0, 30.0),
			production_control_defaults("PRODUCTION_MICRORELIEF_SETTINGS"))
		add_node("PRODUCTION_ANTITILE_SETTINGS", Vector2(1440.0, 370.0),
			production_control_defaults("PRODUCTION_ANTITILE_SETTINGS"))
		add_node("PRODUCTION_ROCK_PBR_SETTINGS", Vector2(1800.0, 30.0),
			production_control_defaults("PRODUCTION_ROCK_PBR_SETTINGS"))
		add_node("PRODUCTION_SCAN_PBR_SETTINGS", Vector2(2160.0, 30.0),
			production_control_defaults("PRODUCTION_SCAN_PBR_SETTINGS"))
		add_node("PRODUCTION_SCAN_TEXTURES", Vector2(2520.0, 30.0),
			production_control_defaults("PRODUCTION_SCAN_TEXTURES"))
	revision += 1

static func production_control_defaults(node_type: String) -> Dictionary:
	match node_type:
		"PRODUCTION_GEOMORPH_SETTINGS":
			return {
				"detail_strength":1.0, "override_seed":false, "detail_seed":1337,
				"warp_strength":1.0, "broad_strength":1.0, "mountain_strength":1.0,
				"mid_strength":1.0, "channel_strength":1.0, "deposit_strength":1.0,
				"fine_strength":1.0, "dune_strength":1.0, "glacial_strength":1.0,
				"broad_wavelength_m":16000.0, "broad_low_amplitude_m":24.0,
				"broad_mountain_amplitude_m":125.0, "broad_warp":0.8,
				"mountain_wavelength_m":6000.0, "mountain_amplitude_m":210.0,
				"mountain_warp":1.1, "mountain_ridge_scale":1.55, "mountain_cell_mix":0.58,
				"mid_wavelength_m":1400.0, "mid_ridge_amplitude_m":72.0,
				"mid_noise_amplitude_m":24.0, "mid_warp":0.72,
				"mid_ridge_scale":1.25, "mid_detail_scale":2.1,
				"channel_wavelength_m":420.0, "channel_depth_min_m":2.0,
				"channel_depth_max_m":34.0, "channel_warp":0.55, "channel_power":4.6,
				"flow_along_scale":0.42, "flow_across_scale":1.45,
				"deposit_amplitude_min_m":1.0, "deposit_amplitude_max_m":12.0,
				"deposit_scale":0.48, "deposit_power":2.2,
				"fine_wavelength_m":120.0, "fine_amplitude_m":4.5,
				"dune_wavelength_m":180.0, "dune_amplitude_m":9.0, "dune_warp":0.45,
				"micro_wavelength_m":24.0, "micro_amplitude_m":0.9,
				"glacial_wavelength_m":2600.0, "glacial_amplitude_m":52.0,
				"glacial_base_scale":0.62, "glacial_mix":0.72,
			}
		"PRODUCTION_CLASSIFIER_SETTINGS":
			return {
				"rock_scale":1.0, "soil_scale":1.0, "vegetation_scale":1.0, "sand_scale":1.0,
				"mud_scale":1.0, "snow_scale":1.0, "scree_scale":1.0, "gravel_scale":1.0,
				"albedo_chroma":1.24, "albedo_contrast":1.10, "albedo_pivot":0.115,
				"roughness_scale":1.0, "roughness_bias":0.0,
				"roughness_min":0.42, "roughness_max":0.98,
			}
		"PRODUCTION_CLASSIFIER_THRESHOLDS":
			return {
				"soil_repose_dry_deg":42.0, "soil_repose_wet_deg":27.0,
				"loose_margin_low_deg":4.0, "loose_margin_high_deg":5.0,
				"sand_slope_start_deg":29.0, "sand_slope_end_deg":36.0,
				"vegetation_slope_start_deg":32.0, "vegetation_slope_end_deg":46.0,
				"thin_soil_start_m":0.06, "thin_soil_end_m":0.55,
				"rock_slope_start_deg":43.0, "rock_slope_end_deg":58.0,
				"mountain_rock_slope_start_deg":48.0, "mountain_rock_slope_end_deg":62.0,
				"arid_bare_start":0.58, "arid_bare_end":0.94,
				"bare_precip_start_mm":220.0, "bare_precip_end_mm":650.0,
				"bare_temp_start_c":-14.0, "bare_temp_end_c":-3.0,
				"dune_arid_start":0.55, "dune_arid_end":0.92,
				"dune_precip_start_mm":300.0, "dune_precip_end_mm":850.0,
				"thermal_growth_start_c":-7.0, "thermal_growth_end_c":4.0,
				"vegetation_depth_start_m":0.04, "vegetation_depth_end_m":0.28,
				"soil_depth_start_m":0.025, "soil_depth_end_m":0.22,
				"mud_slope_start_deg":12.0, "mud_slope_end_deg":28.0,
				"saturated_wet_start":0.68, "saturated_wet_end":0.92,
				"saturated_precip_start_mm":350.0, "saturated_precip_end_mm":900.0,
				"snow_temp_start_c":-8.0, "snow_temp_end_c":2.0,
				"snow_slope_start_deg":28.0, "snow_slope_end_deg":50.0,
				"marginal_snow_temp_start_c":-2.0, "marginal_snow_temp_end_c":5.0,
				"snow_precip_start_mm":500.0, "snow_precip_end_mm":1400.0,
				"scree_slope_start_deg":25.0, "scree_slope_full_deg":34.0,
				"scree_slope_fade_deg":45.0, "scree_slope_end_deg":55.0,
				"gravel_slope_start_deg":18.0, "gravel_slope_end_deg":34.0,
			}
		"PRODUCTION_SURFACE_PALETTE":
			return {
				"rock_granite":Color(0.335,0.305,0.275), "rock_basalt":Color(0.085,0.082,0.080),
				"rock_gabbro":Color(0.105,0.104,0.098), "rock_gneiss":Color(0.250,0.240,0.232),
				"rock_schist":Color(0.175,0.170,0.158), "rock_sandstone":Color(0.365,0.230,0.140),
				"rock_shale":Color(0.155,0.146,0.132), "rock_limestone":Color(0.415,0.400,0.352),
				"rock_dolomite":Color(0.375,0.358,0.318), "rock_conglomerate":Color(0.265,0.222,0.180),
				"rock_quartzite":Color(0.455,0.442,0.422), "rock_tuff":Color(0.330,0.298,0.260),
				"rock_serpentinite":Color(0.120,0.145,0.118),
				"soil_sandy":Color(0.385,0.225,0.075), "soil_silty":Color(0.220,0.120,0.040),
				"soil_clayey":Color(0.315,0.075,0.028), "soil_humus":Color(0.028,0.017,0.006),
				"vegetation_dry":Color(0.285,0.175,0.030), "vegetation_grass":Color(0.185,0.255,0.042),
				"vegetation_wet":Color(0.030,0.155,0.030), "vegetation_lush":Color(0.010,0.075,0.016),
				"vegetation_cold":Color(0.095,0.105,0.045),
				"sand_low":Color(0.485,0.285,0.095), "sand_high":Color(0.660,0.445,0.155),
				"mud_tint":Color(0.42,0.36,0.30), "snow":Color(0.84,0.89,0.94),
				"gravel":Color(0.205,0.185,0.155),
				"scree_rock_mix":0.80, "scree_add":0.026, "gravel_rock_mix":0.44,
				"roughness_fallback":0.90, "roughness_rock":0.76, "roughness_soil":0.92,
				"roughness_vegetation":0.96, "roughness_sand":0.82,
				"roughness_mud_dry":0.72, "roughness_mud_wet":0.48,
				"roughness_snow":0.92, "roughness_scree":0.86, "roughness_gravel":0.80,
			}
		"PRODUCTION_MICRORELIEF_SETTINGS":
			return {
				"enabled":true, "strength":1.0, "rock_scale":1.0, "soil_scale":1.0,
				"sand_scale":1.0, "mud_scale":1.0, "snow_scale":1.0,
				"gravel_scale":1.0, "scree_scale":1.0, "base_noise_scale":1.0,
			}
		"PRODUCTION_ANTITILE_SETTINGS":
			return {
				"strength":1.0, "coarse_cell_m":32.0, "fine_cell_m":8.0,
				"coarse_offset_m":0.58, "fine_offset_m":0.14,
				"coarse_seed":29093, "fine_seed":46141,
			}
		"PRODUCTION_ROCK_PBR_SETTINGS":
			return {
				"enabled":true, "detail_strength":1.0, "normal_strength":1.0, "color_strength":1.0,
				"coarse_cell_m":4.0, "fine_cell_m":1.0, "coarse_seed":11665, "fine_seed":35415,
				"coarse_fade_near_m":850.0, "coarse_fade_far_m":3400.0,
				"fine_fade_near_m":130.0, "fine_fade_far_m":920.0,
				"normal_fade_near_m":90.0, "normal_fade_far_m":620.0,
				"weatherability_pivot":0.45, "weatherability_roughness":0.055,
				"family_roughness_min":0.48, "family_roughness_max":0.97,
				"fracture_color_base":0.09, "fracture_color_fault":0.15,
				"fracture_roughness":0.040, "normal_fracture_mix":0.55,
				"output_roughness_min":0.45, "output_roughness_max":0.98,
			}
		"PRODUCTION_SCAN_PBR_SETTINGS":
			return {
				"enabled":true, "ground_metres":2.0, "grass_metres":2.0,
				"mud_metres":1.0, "forest_metres":2.0, "triplanar_sharpness":5.0,
				"transfer_strength":0.60, "transfer_min":0.48, "transfer_max":1.72,
				"surface_fade_near_m":900.0, "surface_fade_far_m":3200.0,
				"normal_fade_near_m":120.0, "normal_fade_far_m":520.0,
				"loose_threshold":0.015, "loose_albedo_strength":0.72,
				"loose_roughness_strength":0.70, "loose_normal_mix":0.42,
				"loose_normal_weight":0.48, "special_threshold":0.015,
				"special_albedo_strength":0.88, "special_roughness_strength":0.82,
				"special_normal_mix":0.58, "special_normal_weight":0.72,
			}
		"PRODUCTION_SCAN_TEXTURES":
			return {
				"ground_albedo":"res://assets/textures/terrain/ground003_color_2k.jpg",
				"ground_normal":"res://assets/textures/terrain/ground003_normal_gl_2k.jpg",
				"ground_roughness":"res://assets/textures/terrain/ground003_roughness_2k.jpg",
				"grass_albedo":"res://assets/textures/terrain/leafy_grass_diff_2k.jpg",
				"grass_normal":"res://assets/textures/terrain/leafy_grass_nor_gl_2k.jpg",
				"grass_roughness":"res://assets/textures/terrain/leafy_grass_rough_2k.jpg",
				"mud_albedo":"res://assets/textures/terrain/brown_mud_diff_2k.jpg",
				"mud_normal":"res://assets/textures/terrain/brown_mud_nor_gl_2k.jpg",
				"mud_roughness":"res://assets/textures/terrain/brown_mud_rough_2k.jpg",
				"forest_albedo":"res://assets/textures/terrain/forrest_ground_01_diff_2k.jpg",
				"forest_normal":"res://assets/textures/terrain/forrest_ground_01_nor_gl_2k.jpg",
				"forest_roughness":"res://assets/textures/terrain/forrest_ground_01_rough_2k.jpg",
			}
	return {}

static func node_parameter_defaults(node_type: String) -> Dictionary:
	if node_type == "LATITUDE_MASK":
		return {
			"south_deg":-30.0,
			"north_deg":30.0,
			"feather_deg":5.0,
			"invert":false,
		}
	if node_type == "TERRACE_RELIEF":
		return {
			"scale":6.0,
			"amount":80.0,
			"steps":6,
			"seed":1337,
		}
	if node_type == "BILLOW_NOISE":
		return {
			"scale":6.0,
			"amount":100.0,
			"passes":3,
			"seed":1337,
		}
	if node_type == "VORONOI_RIDGES":
		return {
			"scale":6.0,
			"amount":200.0,
			"passes":3,
			"seed":1337,
		}
	return {}

func add_node(node_type: String, position: Vector2, parameters: Dictionary = {}) -> String:
	var resolved_type := node_type if NODE_TYPES.has(node_type) else "CONSTANT_FLOAT"
	var node_id := make_node_id(resolved_type)
	var resolved_parameters: Dictionary = parameters.duplicate(true)
	if resolved_parameters.is_empty():
		if PRODUCTION_CONTROL_NODES.has(resolved_type):
			resolved_parameters = production_control_defaults(resolved_type)
		else:
			resolved_parameters = node_parameter_defaults(resolved_type)
	nodes.append({"id":node_id, "type":resolved_type, "position":position, "parameters":resolved_parameters})
	revision += 1
	return node_id

func remove_node(node_id: String) -> bool:
	for index: int in nodes.size():
		if String(nodes[index].get("id", "")) != node_id:
			continue
		nodes.remove_at(index)
		for link_index: int in range(links.size() - 1, -1, -1):
			var link: Dictionary = links[link_index]
			if String(link.get("from", "")) == node_id or String(link.get("to", "")) == node_id:
				links.remove_at(link_index)
		revision += 1
		return true
	return false

func set_node_position(node_id: String, position: Vector2) -> bool:
	for index: int in nodes.size():
		var node: Dictionary = nodes[index]
		if String(node.get("id", "")) != node_id:
			continue
		node["position"] = position
		nodes[index] = node
		revision += 1
		return true
	return false

func set_node_parameter(node_id: String, key: String, value: Variant) -> bool:
	for index: int in nodes.size():
		var node: Dictionary = nodes[index]
		if String(node.get("id", "")) != node_id:
			continue
		var parameters: Dictionary = node.get("parameters", {}) as Dictionary
		parameters[key] = value
		node["parameters"] = parameters
		nodes[index] = node
		revision += 1
		return true
	return false

func connect_nodes(from_id: String, from_port: int, to_id: String, to_port: int) -> bool:
	if from_id == to_id or not has_node(from_id) or not has_node(to_id):
		return false
	for link: Dictionary in links:
		if String(link.get("to", "")) == to_id and int(link.get("to_port", -1)) == to_port:
			return false
	links.append({"from":from_id, "from_port":maxi(0, from_port), "to":to_id, "to_port":maxi(0, to_port)})
	revision += 1
	return true

func disconnect_nodes(from_id: String, from_port: int, to_id: String, to_port: int) -> bool:
	for index: int in links.size():
		var link: Dictionary = links[index]
		if String(link.get("from", "")) == from_id and int(link.get("from_port", -1)) == from_port \
		and String(link.get("to", "")) == to_id and int(link.get("to_port", -1)) == to_port:
			links.remove_at(index)
			revision += 1
			return true
	return false

func has_node(node_id: String) -> bool:
	for node: Dictionary in nodes:
		if String(node.get("id", "")) == node_id:
			return true
	return false

func _remove_invalid_links() -> void:
	for index: int in range(links.size() - 1, -1, -1):
		var link: Dictionary = links[index]
		if not has_node(String(link.get("from", ""))) or not has_node(String(link.get("to", ""))):
			links.remove_at(index)

static func node_category(node_type: String) -> String:
	return String(NODE_CATEGORY_BY_TYPE.get(node_type, CATEGORY_UTILITY))

static func node_catalog(next_domain: int, category: String = "") -> Array[String]:
	var out: Array[String] = []
	for node_type: String in NODE_TYPES:
		if DISPLACEMENT_ONLY_NODES.has(node_type) and next_domain != Domain.DISPLACEMENT:
			continue
		if MATERIAL_ONLY_NODES.has(node_type) and next_domain != Domain.MATERIAL:
			continue
		if not category.is_empty() and node_category(node_type) != category:
			continue
		out.append(node_type)
	return out

static func node_categories(next_domain: int) -> Array[String]:
	var out: Array[String] = []
	for category: String in NODE_CATEGORIES:
		for node_type: String in node_catalog(next_domain, category):
			if node_type.begins_with("OUTPUT_"):
				continue
			out.append(category)
			break
	return out

static func game_input_options(next_domain: int) -> Array[String]:
	return DISPLACEMENT_GAME_INPUTS.duplicate() if next_domain == Domain.DISPLACEMENT else MATERIAL_GAME_INPUTS.duplicate()

static func game_input_category(source: String) -> String:
	if source.begins_with("base_"):
		return CATEGORY_SURFACE_PBR
	if source in ["material_primary", "material_secondary", "biome_id", "rock_id", "rock_mix"]:
		return CATEGORY_CLASSIFICATION
	if source in ["soil", "surface", "geology", "structure", "climate", "landform",
		"soil_sand", "soil_silt", "soil_clay", "soil_depth_m", "surface_sediment_m",
		"temperature", "precipitation", "temperature_range", "moisture",
		"vegetation_biomass", "erodibility", "strata_dip", "uplift", "flow_x", "flow_y", "hydrology"]:
		return CATEGORY_WORLD_DATA
	if source in ["terrain_height_m", "generated_height_m", "sculpt_delta_m", "micro_layer"]:
		return CATEGORY_TERRAIN_SOURCE
	return CATEGORY_UTILITY

static func make_graph_id(label: String) -> String:
	var safe := label.strip_edges().to_lower().replace(" ", "-")
	if safe.is_empty():
		safe = "terrain-graph"
	return "%s-%d-%d" % [safe, Time.get_ticks_usec(), randi() & 0x7fffffff]

static func make_node_id(node_type: String) -> String:
	return "%s-%d-%d" % [node_type.to_lower(), Time.get_ticks_usec(), randi() & 0x7fffffff]
