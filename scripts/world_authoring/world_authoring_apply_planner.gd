class_name WorldAuthoringApplyPlanner
extends RefCounted
## Computes the minimum live-runtime work required for a Planet Studio Apply.
##
## The editor's ApplyScope remains a useful UI hint, but it is intentionally not
## the authority here: TILES can mean biome paint or authored water, and HOT can
## mean atmosphere, waves, direct runtime shader overrides, or metadata. The
## planner compares the last applied snapshot with the new snapshot and returns
## independent subsystem dirty flags.

const RUNTIME_GENERATION_FIELDS: PackedStringArray = [
	"detail_amplitude",
	"detail_octaves",
	"detail_base_frequency",
	"quadtree_max_depth",
	"chunk_grid",
	"lod_split_factor",
	"lod_target_error_px",
	"lod_collapse_ratio",
	"collision_depth",
	"collision_stream_depth",
	"collision_stream_radius",
	"collision_grid",
	"edit_cell_size",
	"use_gpu_bake",
]

# These are represented by body/profile fields at runtime and therefore should
# not trigger a generator bake merely because the generation-profile copy differs.
const REDUNDANT_GENERATION_FIELDS: PackedStringArray = [
	"planet_radius",
	"axial_tilt_deg",
	"atmosphere_height",
]

const WATER_HOT_FIELDS: PackedStringArray = [
	"wave_amplitude_scale",
	"wave_frequency_scale",
	"wind_response",
	"foam_strength",
	"absorption_scale",
	"scattering_scale",
]

static func build(previous_system: Resource, next_system: Resource,
		fallback_scope: int = 0) -> Dictionary:
	var plan := _empty_plan()
	plan["fallback_scope"] = fallback_scope
	if previous_system == null or next_system == null:
		plan["full_rebuild"] = true
		plan["reason"] = "missing runtime snapshot"
		return plan

	var previous_body: Resource = previous_system.call("active_body") as Resource
	var next_body: Resource = next_system.call("active_body") as Resource
	if previous_body == null or next_body == null:
		plan["full_rebuild"] = true
		plan["reason"] = "active body unavailable"
		return plan
	if String(previous_body.get(&"body_id")) != String(next_body.get(&"body_id")):
		plan["full_rebuild"] = true
		plan["reason"] = "active body changed"
		return plan

	if not is_equal_approx(float(previous_body.get(&"radius_m")),
			float(next_body.get(&"radius_m"))):
		plan["full_rebuild"] = true
		plan["reason"] = "planet radius changed"

	plan["frames"] = (
		not is_equal_approx(float(previous_body.get(&"axial_tilt_deg")),
			float(next_body.get(&"axial_tilt_deg")))
		or not is_equal_approx(float(previous_body.get(&"sidereal_rotation_period_s")),
			float(next_body.get(&"sidereal_rotation_period_s"))))

	var previous_profile: Resource = previous_body.get(&"planet_profile") as Resource
	var next_profile: Resource = next_body.get(&"planet_profile") as Resource
	if previous_profile == null or next_profile == null:
		plan["full_rebuild"] = true
		plan["reason"] = "planet profile changed incompatibly"
		return plan

	plan["runtime_shader"] = (
		not _equivalent(previous_profile.get(&"runtime_shader_paths"),
			next_profile.get(&"runtime_shader_paths"))
		or not _equivalent(previous_profile.get(&"runtime_shader_overrides"),
			next_profile.get(&"runtime_shader_overrides")))

	var previous_terrain: Resource = previous_profile.get(&"terrain") as Resource
	var next_terrain: Resource = next_profile.get(&"terrain") as Resource
	if previous_terrain == null or next_terrain == null:
		plan["full_rebuild"] = true
		plan["reason"] = "terrain profile changed incompatibly"
		return plan

	var previous_generation: Resource = previous_terrain.get(&"generation_profile") as Resource
	var next_generation: Resource = next_terrain.get(&"generation_profile") as Resource
	_classify_generation(previous_generation, next_generation, plan)

	plan["biome"] = not _equivalent(
		previous_terrain.get(&"biome_override_layers"),
		next_terrain.get(&"biome_override_layers"))
	plan["graph"] = (
		not _equivalent(previous_terrain.get(&"displacement_slots"),
			next_terrain.get(&"displacement_slots"))
		or not _equivalent(previous_terrain.get(&"material_slots"),
			next_terrain.get(&"material_slots"))
		or not _equivalent(previous_terrain.get(&"imported_texture_asset_ids"),
			next_terrain.get(&"imported_texture_asset_ids")))
	plan["sculpt"] = (
		int(previous_terrain.get(&"sculpt_delta_version")) != int(next_terrain.get(&"sculpt_delta_version"))
		or previous_terrain.get(&"sculpt_delta_keys") != next_terrain.get(&"sculpt_delta_keys")
		or previous_terrain.get(&"sculpt_delta_tiles") != next_terrain.get(&"sculpt_delta_tiles"))

	var previous_water: Resource = previous_profile.get(&"water") as Resource
	var next_water: Resource = next_profile.get(&"water") as Resource
	_classify_water(previous_water, next_water, plan)

	var previous_atmosphere: Resource = previous_profile.get(&"atmosphere") as Resource
	var next_atmosphere: Resource = next_profile.get(&"atmosphere") as Resource
	plan["atmosphere"] = not _equivalent(previous_atmosphere, next_atmosphere)

	# Body metadata, gravity and orbital authoring do not require terrain data to
	# be regenerated. They are still an Apply, but the terrain pipeline does zero
	# work unless one of the explicit flags above is set.
	plan["hot"] = bool(plan["frames"]) or bool(plan["water_material"]) \
		or bool(plan["atmosphere"]) or bool(plan["runtime_shader"])

	if bool(plan["full_rebuild"]):
		return plan
	if not _has_runtime_work(plan):
		# Unknown future edits fall back conservatively to the declared scope, while
		# known metadata-only HOT/FULL_REBUILD fields stay cheap because their data
		# was already inspected above.
		match fallback_scope:
			2: plan["graph"] = true
			3: plan["tiles"] = true
			4: plan["clipmap"] = true
	return plan

static func _classify_generation(previous: Resource, next: Resource,
		plan: Dictionary) -> void:
	if previous == null or next == null:
		if previous != next:
			plan["full_rebuild"] = true
			plan["reason"] = "generation profile replaced"
		return
	var names: Dictionary = {}
	for property: Dictionary in previous.get_property_list():
		if (int(property.get("usage", 0)) & PROPERTY_USAGE_SCRIPT_VARIABLE) != 0:
			names[String(property.get("name", ""))] = true
	for property: Dictionary in next.get_property_list():
		if (int(property.get("usage", 0)) & PROPERTY_USAGE_SCRIPT_VARIABLE) != 0:
			names[String(property.get("name", ""))] = true
	for property_name_value: Variant in names.keys():
		var property_name := String(property_name_value)
		if property_name in REDUNDANT_GENERATION_FIELDS:
			continue
		if _equivalent(previous.get(property_name), next.get(property_name)):
			continue
		if property_name in RUNTIME_GENERATION_FIELDS:
			plan["clipmap"] = true
		else:
			plan["full_rebuild"] = true
			plan["reason"] = "generator field changed: %s" % property_name

static func _classify_water(previous: Resource, next: Resource,
		plan: Dictionary) -> void:
	if previous == null or next == null:
		if previous != next:
			plan["water_geometry"] = true
			plan["ocean"] = true
			plan["water_material"] = true
			plan["water"] = true
		return
	plan["water_geometry"] = not _equivalent(
		previous.get(&"authored_features"), next.get(&"authored_features"))
	plan["ocean"] = (
		bool(previous.get(&"ocean_enabled")) != bool(next.get(&"ocean_enabled"))
		or not is_equal_approx(float(previous.get(&"sea_level_m")),
			float(next.get(&"sea_level_m"))))
	var hot_changed := false
	for field_name: String in WATER_HOT_FIELDS:
		if not _equivalent(previous.get(field_name), next.get(field_name)):
			hot_changed = true
			break
	plan["water_material"] = hot_changed
	if not _equivalent(previous.get(&"material_slots"), next.get(&"material_slots")):
		plan["graph"] = true
	plan["water"] = bool(plan["water_geometry"]) or bool(plan["ocean"]) \
		or bool(plan["water_material"])

static func _empty_plan() -> Dictionary:
	return {
		"full_rebuild": false,
		"clipmap": false,
		"tiles": false,
		"biome": false,
		"water": false,
		"water_geometry": false,
		"water_material": false,
		"ocean": false,
		"atmosphere": false,
		"runtime_shader": false,
		"graph": false,
		"sculpt": false,
		"frames": false,
		"hot": false,
		"fallback_scope": 0,
		"reason": "",
	}

static func _has_runtime_work(plan: Dictionary) -> bool:
	for key: String in ["full_rebuild", "clipmap", "tiles", "biome", "water",
			"water_geometry", "water_material", "ocean", "atmosphere",
			"runtime_shader", "graph", "sculpt", "frames", "hot"]:
		if bool(plan.get(key, false)):
			return true
	return false

static func _equivalent(a: Variant, b: Variant) -> bool:
	return _normalized(a, 0) == _normalized(b, 0)

static func _normalized(value: Variant, depth: int) -> Variant:
	if depth > 18:
		return "<max-depth>"
	if value is Resource:
		var resource: Resource = value as Resource
		var out: Dictionary = {}
		for property: Dictionary in resource.get_property_list():
			var usage := int(property.get("usage", 0))
			if (usage & PROPERTY_USAGE_STORAGE) == 0:
				continue
			var property_name := String(property.get("name", ""))
			if property_name in ["resource_local_to_scene", "resource_name", "script"]:
				continue
			out[property_name] = _normalized(resource.get(property_name), depth + 1)
		return out
	if value is Array:
		var out_array: Array = []
		for item: Variant in value as Array:
			out_array.append(_normalized(item, depth + 1))
		return out_array
	if value is Dictionary:
		var out_dictionary: Dictionary = {}
		var source: Dictionary = value as Dictionary
		var keys: Array = source.keys()
		keys.sort_custom(func(left: Variant, right: Variant) -> bool:
			return str(left) < str(right))
		for key: Variant in keys:
			out_dictionary[str(key)] = _normalized(source[key], depth + 1)
		return out_dictionary
	return value
