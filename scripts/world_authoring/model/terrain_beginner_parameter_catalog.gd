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
	return ["Coarse Elevation"]


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
	# Terrain shape is: the coarse elevation map (these three EQ bands) + whatever
	# each biome authors on top in the Biome Terrain tab. The old synthesised
	# geomorph bands (mountains / valleys / dunes / glacial / fine detail) no
	# longer generate terrain, so their sliders were removed -- author that relief
	# per biome instead.
	return [
		_control("base_elevation_continental", "Continental",
			"Whole-continent rise and fall in the planet's resident elevation map. 1x is the untouched planet; 0x removes the land/ocean height difference.",
			"Coarse Elevation", 1.0, 0.0, 2.0, 0.01, "x"),
		_control("base_elevation_regional", "Regional",
			"Plateau, basin and mountain-range-scale shape in the coarse elevation map.",
			"Coarse Elevation", 1.0, 0.0, 2.0, 0.01, "x"),
		_control("base_elevation_local", "Local",
			"The finest structure already present in the coarse elevation map. All three at 0x gives a smooth sphere that just follows the clipmap grid; compose the rest per biome.",
			"Coarse Elevation", 1.0, 0.0, 2.0, 0.01, "x"),
	]
