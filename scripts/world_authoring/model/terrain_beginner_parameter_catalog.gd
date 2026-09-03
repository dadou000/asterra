class_name TerrainBeginnerParameterCatalog
extends RefCounted
## Human-facing metadata for the existing production terrain graph.
##
## This is deliberately NOT another terrain system. Every entry points to an
## existing serialized production-settings node/parameter. Simple and Detailed UI
## are therefore alternate views of the exact same graph document used by the node
## editor and renderer.

const MODE_SIMPLE := 0
const MODE_DETAILED := 1
const MODE_NODE_GRAPH := 2
const GEOMORPH_NODE_TYPE := "PRODUCTION_GEOMORPH_SETTINGS"


static func mode_name(mode: int) -> String:
	match mode:
		MODE_SIMPLE: return "Simple"
		MODE_DETAILED: return "Detailed"
		MODE_NODE_GRAPH: return "Node Graph"
	return "Simple"


static func controls_for_mode(mode: int) -> Array[Dictionary]:
	var out: Array[Dictionary] = []
	for item: Dictionary in _all_controls():
		var minimum_mode: int = int(item.get("minimum_mode", MODE_SIMPLE))
		if mode >= minimum_mode and mode <= MODE_DETAILED:
			out.append(item.duplicate(true))
	return out


static func simple_categories() -> Array[String]:
	return ["Overall Shape", "Mountains & Hills", "Valleys", "Fine Detail", "Special Terrain"]


static func _control(key: String, title: String, description: String,
		category: String, default_value: float, minimum: float, maximum: float,
		step: float, unit: String = "", minimum_mode: int = MODE_SIMPLE) -> Dictionary:
	return {
		"node_type": GEOMORPH_NODE_TYPE,
		"key": key,
		"title": title,
		"description": description,
		"category": category,
		"default": default_value,
		"min": minimum,
		"max": maximum,
		"step": step,
		"unit": unit,
		"minimum_mode": minimum_mode,
	}


static func _all_controls() -> Array[Dictionary]:
	return [
		_control("detail_strength", "Overall Terrain Detail",
			"Changes how strongly the smaller terrain shapes appear without changing the planet's large-scale base.",
			"Overall Shape", 1.0, 0.0, 2.5, 0.01),
		_control("warp_strength", "Natural Variation",
			"Makes terrain shapes less regular and repetitive. Low values look orderly; higher values bend ranges and valleys.",
			"Overall Shape", 1.0, 0.0, 2.5, 0.01),
		_control("broad_strength", "Large Landscape Shape",
			"Controls the very broad rises and depressions visible across many kilometres.",
			"Overall Shape", 1.0, 0.0, 2.5, 0.01),

		_control("base_elevation_continental", "Base: Continental",
			"EQ band over the planet's own resident elevation: whole-continent rise and fall. 1x is the untouched planet, 0x removes the land/ocean height difference.",
			"Overall Shape", 1.0, 0.0, 2.0, 0.01, "x"),
		_control("base_elevation_regional", "Base: Regional",
			"EQ band over the planet's own resident elevation: plateau, basin and mountain-range-scale shape. Set every Base band to 0 for an almost-flat world that follows the coarse grid.",
			"Overall Shape", 1.0, 0.0, 2.0, 0.01, "x"),
		_control("base_elevation_local", "Base: Local",
			"EQ band over the planet's own resident elevation: the finest structure already present in the base field.",
			"Overall Shape", 1.0, 0.0, 2.0, 0.01, "x"),
		_control("biome_terrain_variation", "Vary Terrain by Biome",
			"1x lets the shared terrain add its own per-biome relief (mountains where it's tectonic, dunes where it's arid, glacial shaping where it's cold). 0x makes the Global Terrain uniform everywhere so all per-biome relief comes from the Biome Terrain tab. Surface materials and colours are unaffected.",
			"Overall Shape", 1.0, 0.0, 1.0, 0.01, "x"),

		_control("mountain_strength", "Mountain Amount",
			"Controls how strongly mountain ranges contribute to this terrain.",
			"Mountains & Hills", 1.0, 0.0, 2.5, 0.01),
		_control("mountain_amplitude_m", "Mountain Height",
			"Maximum strength of the production mountain layer. The real result also depends on the underlying landscape.",
			"Mountains & Hills", 210.0, 0.0, 1800.0, 1.0, "m"),
		_control("mountain_wavelength_m", "Mountain Size",
			"Approximate spacing of the large mountain shapes. Higher values create broader ranges.",
			"Mountains & Hills", 6000.0, 500.0, 30000.0, 50.0, "m"),
		_control("mid_strength", "Hills & Medium Relief",
			"Controls kilometre-scale hills and ridges between the largest mountains and small ground detail.",
			"Mountains & Hills", 1.0, 0.0, 2.5, 0.01),

		_control("channel_strength", "Valleys & Drainage",
			"Controls the carved channels and valley network.",
			"Valleys", 1.0, 0.0, 2.5, 0.01),
		_control("channel_depth_max_m", "Valley Depth",
			"Sets the strongest depth of the channel layer.",
			"Valleys", 34.0, 0.0, 350.0, 0.5, "m"),
		_control("channel_wavelength_m", "Valley Spacing",
			"Controls the approximate spacing between smaller drainage features.",
			"Valleys", 420.0, 40.0, 4000.0, 10.0, "m", MODE_DETAILED),
		_control("channel_power", "Valley Sharpness",
			"Higher values concentrate erosion into narrower channels; lower values make broader valleys.",
			"Valleys", 4.6, 0.5, 10.0, 0.05, "", MODE_DETAILED),

		_control("fine_strength", "Small Ground Detail",
			"Controls the visible small terrain bumps without changing the broad landscape.",
			"Fine Detail", 1.0, 0.0, 2.5, 0.01),
		_control("fine_amplitude_m", "Small Detail Height",
			"Height of the fine terrain layer.",
			"Fine Detail", 4.5, 0.0, 80.0, 0.1, "m", MODE_DETAILED),
		_control("fine_wavelength_m", "Small Detail Size",
			"Approximate spacing of fine terrain undulations.",
			"Fine Detail", 120.0, 5.0, 1500.0, 5.0, "m", MODE_DETAILED),
		_control("micro_amplitude_m", "Tiny Ground Bumps",
			"Height of the smallest geometry-scale terrain variation.",
			"Fine Detail", 0.9, 0.0, 12.0, 0.05, "m", MODE_DETAILED),
		_control("micro_wavelength_m", "Tiny Bump Size",
			"Approximate spacing of the smallest geometry-scale variation.",
			"Fine Detail", 24.0, 1.0, 250.0, 1.0, "m", MODE_DETAILED),

		_control("dune_strength", "Dunes",
			"Controls how strongly dune-shaped terrain appears where the production rules allow it.",
			"Special Terrain", 1.0, 0.0, 2.5, 0.01),
		_control("glacial_strength", "Glacial Shaping",
			"Controls broad glacial-style shaping where the production terrain uses it.",
			"Special Terrain", 1.0, 0.0, 2.5, 0.01),
		_control("dune_amplitude_m", "Dune Height",
			"Maximum height contribution of the dune layer.",
			"Special Terrain", 9.0, 0.0, 120.0, 0.5, "m", MODE_DETAILED),
		_control("dune_wavelength_m", "Dune Size",
			"Approximate spacing of dune forms.",
			"Special Terrain", 180.0, 10.0, 2500.0, 10.0, "m", MODE_DETAILED),
		_control("glacial_amplitude_m", "Glacial Relief",
			"Maximum height contribution of the glacial shaping layer.",
			"Special Terrain", 52.0, 0.0, 800.0, 1.0, "m", MODE_DETAILED),
		_control("glacial_wavelength_m", "Glacial Feature Size",
			"Approximate scale of broad glacial terrain forms.",
			"Special Terrain", 2600.0, 200.0, 20000.0, 50.0, "m", MODE_DETAILED),
	]
