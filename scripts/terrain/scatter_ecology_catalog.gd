class_name ScatterEcologyCatalog
extends RefCounted
## Data-side contract for the ecological scatter rewrite.
##
## This deliberately does not replace TerrainScatter yet. The current three-family
## GPU renderer remains authoritative until optimized runtime meshes exist. This
## catalog makes asset selection, biome coverage and per-kind render budgets data
## driven so the next renderer can consume the same manifest without hard-coding
## individual species/assets into shaders.

const MANIFEST_PATH := "res://assets/scatter/asset_manifest.json"
const BIOME_COUNT: int = 18

const KIND_DEFAULTS := {
	"grass": {"tier": "micro", "spacing_m": 0.55, "max_distance_m": 95.0, "shadow_distance_m": 28.0, "collision_distance_m": 0.0, "wind": 1.0},
	"moss": {"tier": "micro", "spacing_m": 0.75, "max_distance_m": 65.0, "shadow_distance_m": 18.0, "collision_distance_m": 0.0, "wind": 0.15},
	"groundcover": {"tier": "micro", "spacing_m": 0.85, "max_distance_m": 90.0, "shadow_distance_m": 24.0, "collision_distance_m": 0.0, "wind": 0.75},
	"wildflower": {"tier": "micro", "spacing_m": 1.15, "max_distance_m": 80.0, "shadow_distance_m": 20.0, "collision_distance_m": 0.0, "wind": 0.9},
	"fern": {"tier": "ground", "spacing_m": 1.25, "max_distance_m": 120.0, "shadow_distance_m": 35.0, "collision_distance_m": 0.0, "wind": 0.55},
	"shrub": {"tier": "ground", "spacing_m": 2.2, "max_distance_m": 220.0, "shadow_distance_m": 70.0, "collision_distance_m": 0.0, "wind": 0.5},
	"dry_shrub": {"tier": "ground", "spacing_m": 3.0, "max_distance_m": 240.0, "shadow_distance_m": 75.0, "collision_distance_m": 0.0, "wind": 0.4},
	"succulent": {"tier": "ground", "spacing_m": 2.8, "max_distance_m": 150.0, "shadow_distance_m": 45.0, "collision_distance_m": 0.0, "wind": 0.08},
	"succulent_shrub": {"tier": "ground", "spacing_m": 3.8, "max_distance_m": 220.0, "shadow_distance_m": 65.0, "collision_distance_m": 0.0, "wind": 0.12},
	"tropical_groundcover": {"tier": "ground", "spacing_m": 1.35, "max_distance_m": 130.0, "shadow_distance_m": 40.0, "collision_distance_m": 0.0, "wind": 0.5},
	"tropical_shrub": {"tier": "ground", "spacing_m": 3.0, "max_distance_m": 240.0, "shadow_distance_m": 75.0, "collision_distance_m": 0.0, "wind": 0.45},
	"sapling": {"tier": "major", "spacing_m": 5.5, "max_distance_m": 520.0, "shadow_distance_m": 140.0, "collision_distance_m": 30.0, "wind": 0.35},
	"conifer_tree": {"tier": "canopy", "spacing_m": 8.0, "max_distance_m": 2200.0, "shadow_distance_m": 260.0, "collision_distance_m": 80.0, "wind": 0.28},
	"broadleaf_tree": {"tier": "canopy", "spacing_m": 8.5, "max_distance_m": 2200.0, "shadow_distance_m": 260.0, "collision_distance_m": 80.0, "wind": 0.35},
	"windswept_tree": {"tier": "canopy", "spacing_m": 10.0, "max_distance_m": 2000.0, "shadow_distance_m": 240.0, "collision_distance_m": 80.0, "wind": 0.32},
	"desert_tree": {"tier": "canopy", "spacing_m": 13.0, "max_distance_m": 1800.0, "shadow_distance_m": 220.0, "collision_distance_m": 80.0, "wind": 0.14},
	"deadwood": {"tier": "major", "spacing_m": 10.0, "max_distance_m": 420.0, "shadow_distance_m": 120.0, "collision_distance_m": 40.0, "wind": 0.0},
	"dry_deadwood": {"tier": "major", "spacing_m": 13.0, "max_distance_m": 450.0, "shadow_distance_m": 130.0, "collision_distance_m": 40.0, "wind": 0.0},
	"stump": {"tier": "major", "spacing_m": 8.0, "max_distance_m": 330.0, "shadow_distance_m": 100.0, "collision_distance_m": 30.0, "wind": 0.0},
	"root": {"tier": "major", "spacing_m": 7.0, "max_distance_m": 260.0, "shadow_distance_m": 75.0, "collision_distance_m": 25.0, "wind": 0.0},
	"dry_debris": {"tier": "ground", "spacing_m": 3.0, "max_distance_m": 130.0, "shadow_distance_m": 32.0, "collision_distance_m": 0.0, "wind": 0.0},
	"stone": {"tier": "micro", "spacing_m": 1.6, "max_distance_m": 150.0, "shadow_distance_m": 32.0, "collision_distance_m": 0.0, "wind": 0.0},
	"stone_set": {"tier": "ground", "spacing_m": 3.0, "max_distance_m": 320.0, "shadow_distance_m": 80.0, "collision_distance_m": 20.0, "wind": 0.0},
	"mossy_stone_set": {"tier": "ground", "spacing_m": 3.2, "max_distance_m": 320.0, "shadow_distance_m": 85.0, "collision_distance_m": 20.0, "wind": 0.0},
	"boulder": {"tier": "major", "spacing_m": 9.0, "max_distance_m": 1100.0, "shadow_distance_m": 220.0, "collision_distance_m": 100.0, "wind": 0.0},
	"desert_boulder": {"tier": "major", "spacing_m": 8.0, "max_distance_m": 1200.0, "shadow_distance_m": 240.0, "collision_distance_m": 110.0, "wind": 0.0},
	"coastal_rocks": {"tier": "major", "spacing_m": 12.0, "max_distance_m": 1400.0, "shadow_distance_m": 250.0, "collision_distance_m": 120.0, "wind": 0.0},
}

const BIOME_DENSITY_MULTIPLIER := {
	"OCEAN": 0.0,
	"SHELF_SEA": 0.18,
	"ICE_CAP": 0.025,
	"TUNDRA": 0.42,
	"TAIGA": 0.88,
	"COLD_DESERT": 0.24,
	"TEMPERATE_GRASSLAND": 1.0,
	"TEMPERATE_FOREST": 0.95,
	"TEMPERATE_RAINFOREST": 1.12,
	"MEDITERRANEAN": 0.72,
	"STEPPE": 0.50,
	"HOT_DESERT": 0.18,
	"SAVANNA": 0.66,
	"TROPICAL_SEASONAL_FOREST": 0.96,
	"TROPICAL_RAINFOREST": 1.18,
	"WETLAND": 1.02,
	"ALPINE": 0.30,
	"BARE_ROCK": 0.12,
}

static var _manifest_cache: Dictionary = {}


static func manifest() -> Dictionary:
	if not _manifest_cache.is_empty():
		return _manifest_cache
	var text: String = FileAccess.get_file_as_string(MANIFEST_PATH)
	if text.is_empty():
		push_error("Scatter ecology manifest is missing or empty: %s" % MANIFEST_PATH)
		return {}
	var parsed: Variant = JSON.parse_string(text)
	if not (parsed is Dictionary):
		push_error("Scatter ecology manifest is not a JSON object: %s" % MANIFEST_PATH)
		return {}
	_manifest_cache = parsed
	return _manifest_cache


static func clear_cache() -> void:
	_manifest_cache.clear()


static func all_assets() -> Array[Dictionary]:
	var result: Array[Dictionary] = []
	var raw_assets: Variant = manifest().get("assets", [])
	if not (raw_assets is Array):
		return result
	for raw: Variant in raw_assets:
		if raw is Dictionary:
			result.append(raw)
	return result


static func assets_for_biome(biome_id: int) -> Array[Dictionary]:
	var result: Array[Dictionary] = []
	if biome_id < 0 or biome_id >= BIOME_COUNT:
		return result
	var biome_name: String = PlanetFields.BIOME_NAMES[biome_id]
	var manifest_name: String = biome_name.to_upper().replace(" ", "_")
	for asset: Dictionary in all_assets():
		var raw_biomes: Variant = asset.get("biomes", [])
		if raw_biomes is Array and raw_biomes.has(manifest_name):
			result.append(asset)
	return result


static func render_defaults_for(asset: Dictionary) -> Dictionary:
	var kind: String = str(asset.get("kind", ""))
	var raw: Variant = KIND_DEFAULTS.get(kind, {})
	return raw.duplicate(true) if raw is Dictionary else {}


static func biome_density_multiplier(biome_id: int) -> float:
	if biome_id < 0 or biome_id >= BIOME_COUNT:
		return 0.0
	var name: String = String(PlanetFields.BIOME_NAMES[biome_id]).to_upper().replace(" ", "_")
	return float(BIOME_DENSITY_MULTIPLIER.get(name, 0.0))


static func runtime_asset_root(asset: Dictionary) -> String:
	var asset_id: String = str(asset.get("id", ""))
	if asset_id.is_empty():
		return ""
	return "res://assets/scatter/runtime/%s" % asset_id


static func validation_errors() -> PackedStringArray:
	var errors := PackedStringArray()
	var data: Dictionary = manifest()
	if data.is_empty():
		errors.append("Manifest could not be loaded")
		return errors

	var valid_biomes: Dictionary = {}
	for biome_name: String in PlanetFields.BIOME_NAMES:
		valid_biomes[biome_name.to_upper().replace(" ", "_")] = true

	var plan: Variant = data.get("biome_plan", {})
	if not (plan is Dictionary):
		errors.append("biome_plan must be a dictionary")
	else:
		for biome_key: Variant in valid_biomes.keys():
			if not plan.has(biome_key):
				errors.append("Missing biome plan: %s" % str(biome_key))

	var seen: Dictionary = {}
	for asset: Dictionary in all_assets():
		var asset_id: String = str(asset.get("id", ""))
		if asset_id.is_empty():
			errors.append("Asset with empty id")
			continue
		if seen.has(asset_id):
			errors.append("Duplicate asset id: %s" % asset_id)
		seen[asset_id] = true
		var kind: String = str(asset.get("kind", ""))
		if not KIND_DEFAULTS.has(kind):
			errors.append("Unknown scatter kind '%s' on %s" % [kind, asset_id])
		var raw_biomes: Variant = asset.get("biomes", [])
		if not (raw_biomes is Array):
			errors.append("Asset %s has non-array biomes" % asset_id)
			continue
		for biome_value: Variant in raw_biomes:
			var biome: String = str(biome_value)
			if not valid_biomes.has(biome):
				errors.append("Asset %s references unknown biome %s" % [asset_id, biome])
	return errors


static func stats() -> Dictionary:
	var by_kind: Dictionary = {}
	var by_biome: Dictionary = {}
	for biome_name: String in PlanetFields.BIOME_NAMES:
		by_biome[biome_name] = 0
	for asset: Dictionary in all_assets():
		var kind: String = str(asset.get("kind", "unknown"))
		by_kind[kind] = int(by_kind.get(kind, 0)) + 1
		var raw_biomes: Variant = asset.get("biomes", [])
		if raw_biomes is Array:
			for biome_value: Variant in raw_biomes:
				var display_name: String = str(biome_value).capitalize().replace("_", " ")
				if by_biome.has(display_name):
					by_biome[display_name] = int(by_biome[display_name]) + 1
	return {
		"asset_count": all_assets().size(),
		"by_kind": by_kind,
		"by_biome": by_biome,
		"validation_errors": validation_errors(),
	}
