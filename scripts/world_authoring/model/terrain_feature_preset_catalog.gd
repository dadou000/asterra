class_name TerrainFeaturePresetCatalog
extends RefCounted
## Curated beginner templates for the Phase 45 simplified terrain UI.
##
## A preset is only a recipe for one or more ordinary TerrainGuidedFeatureGraph
## documents. There is no preset-only runtime and no hidden terrain state: after
## creation every generated feature can be edited, renamed, disabled, deleted or
## opened as the exact Node Graph that the authoritative runtime consumes.

const GUIDED := preload("res://scripts/world_authoring/model/terrain_guided_feature_graph.gd")

const MOUNTAIN_RANGE := "mountain_range"
const CRATER := "crater"
const PLATEAU := "plateau"
const ISLAND := "island"
const COASTAL_BELT := "coastal_belt"
const CANYON_REGION := "canyon_region"

const IDS: Array[String] = [
	MOUNTAIN_RANGE,
	CRATER,
	PLATEAU,
	ISLAND,
	COASTAL_BELT,
	CANYON_REGION,
]


static func label(preset_id: String) -> String:
	match preset_id:
		MOUNTAIN_RANGE: return "Mountain Range"
		CRATER: return "Impact Crater"
		PLATEAU: return "Plateau / Uplift"
		ISLAND: return "Island Massif"
		COASTAL_BELT: return "Coastal Belt"
		CANYON_REGION: return "Canyon Region"
	return "Terrain Preset"


static func description(preset_id: String) -> String:
	match preset_id:
		MOUNTAIN_RANGE:
			return "Broad ridged relief inside a soft geographic region. Good starting point for mountain chains."
		CRATER:
			return "Two editable features: a lowered radial basin plus a raised spherical rim."
		PLATEAU:
			return "Broad local uplift with a soft edge. It preserves existing small terrain variation rather than flattening it."
		ISLAND:
			return "Two editable features: broad land uplift plus a smaller mountainous interior. Coastline depends on sea level."
		COASTAL_BELT:
			return "Ring-shaped sediment deposition around a chosen center, useful as a beach/coastal starting band."
		CANYON_REGION:
			return "Strong erosion-channel pattern restricted to a soft geographic region."
	return "Creates editable guided terrain features."


static func feature_count(preset_id: String) -> int:
	return specs(preset_id).size()


static func specs(preset_id: String, seed_salt: int = 0) -> Array[Dictionary]:
	var out: Array[Dictionary] = []
	match preset_id:
		MOUNTAIN_RANGE:
			out.append(_spec("Mountain Range", {
				"effect_kind":GUIDED.EFFECT_MOUNTAINS,
				"area_kind":GUIDED.AREA_REGION,
				"amount_m":850.0,
				"scale":2.6,
				"passes":4,
				"seed":_seed(18371, seed_salt, 0),
				"south_deg":-8.0,
				"north_deg":8.0,
				"west_deg":-24.0,
				"east_deg":24.0,
				"latitude_feather_deg":3.0,
				"longitude_feather_deg":5.0,
			}))
		CRATER:
			out.append(_spec("Crater Basin", {
				"effect_kind":GUIDED.EFFECT_HEIGHT,
				"area_kind":GUIDED.AREA_RADIAL,
				"amount_m":-320.0,
				"center_latitude_deg":0.0,
				"center_longitude_deg":0.0,
				"radius_deg":5.0,
				"radial_feather_deg":1.0,
				"seed":_seed(29411, seed_salt, 0),
			}))
			out.append(_spec("Crater Rim", {
				"effect_kind":GUIDED.EFFECT_HEIGHT,
				"area_kind":GUIDED.AREA_RING,
				"amount_m":220.0,
				"center_latitude_deg":0.0,
				"center_longitude_deg":0.0,
				"inner_radius_deg":5.2,
				"outer_radius_deg":7.0,
				"radial_feather_deg":1.0,
				"seed":_seed(29411, seed_salt, 1),
			}))
		PLATEAU:
			out.append(_spec("Plateau Uplift", {
				"effect_kind":GUIDED.EFFECT_HEIGHT,
				"area_kind":GUIDED.AREA_RADIAL,
				"amount_m":280.0,
				"center_latitude_deg":0.0,
				"center_longitude_deg":0.0,
				"radius_deg":11.0,
				"radial_feather_deg":2.2,
				"seed":_seed(40763, seed_salt, 0),
			}))
		ISLAND:
			out.append(_spec("Island Landmass", {
				"effect_kind":GUIDED.EFFECT_HEIGHT,
				"area_kind":GUIDED.AREA_RADIAL,
				"amount_m":420.0,
				"center_latitude_deg":0.0,
				"center_longitude_deg":0.0,
				"radius_deg":10.5,
				"radial_feather_deg":4.0,
				"seed":_seed(51977, seed_salt, 0),
			}))
			out.append(_spec("Island Interior Relief", {
				"effect_kind":GUIDED.EFFECT_MOUNTAINS,
				"area_kind":GUIDED.AREA_RADIAL,
				"amount_m":360.0,
				"scale":3.4,
				"passes":4,
				"center_latitude_deg":0.0,
				"center_longitude_deg":0.0,
				"radius_deg":6.0,
				"radial_feather_deg":3.0,
				"seed":_seed(51977, seed_salt, 1),
			}))
		COASTAL_BELT:
			out.append(_spec("Coastal Sediment Belt", {
				"effect_kind":GUIDED.EFFECT_DEPOSIT,
				"area_kind":GUIDED.AREA_RING,
				"amount_m":22.0,
				"scale":8.0,
				"passes":3,
				"center_latitude_deg":0.0,
				"center_longitude_deg":0.0,
				"inner_radius_deg":8.0,
				"outer_radius_deg":12.0,
				"radial_feather_deg":2.5,
				"seed":_seed(63113, seed_salt, 0),
			}))
		CANYON_REGION:
			out.append(_spec("Canyon Region", {
				"effect_kind":GUIDED.EFFECT_CHANNELS,
				"area_kind":GUIDED.AREA_REGION,
				"amount_m":120.0,
				"scale":8.5,
				"passes":4,
				"south_deg":-10.0,
				"north_deg":10.0,
				"west_deg":-22.0,
				"east_deg":22.0,
				"latitude_feather_deg":4.0,
				"longitude_feather_deg":5.0,
				"seed":_seed(74231, seed_salt, 0),
			}))
	return out


static func _spec(name: String, overrides: Dictionary) -> Dictionary:
	var config: Dictionary = GUIDED.default_config()
	for key: Variant in overrides.keys():
		config[key] = overrides[key]
	return {
		"name":name,
		"config":GUIDED.normalized_config(config),
	}


static func _seed(base: int, salt: int, index: int) -> int:
	# Deterministic for CI/replay, while consecutive user-created preset groups can
	# pass a salt so their procedural patterns are not forced to be identical.
	return posmod(base + salt * 7919 + index * 104729, 1048576)
