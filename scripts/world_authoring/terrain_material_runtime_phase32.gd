extends "res://scripts/world_authoring/terrain_material_runtime_phase31.gd"
## Phase 32/33: production surface stage controls.
##
## Settings nodes do not run through the per-fragment bytecode interpreter. They
## bind the same uniforms/samplers consumed by the existing classifier,
## microrelief, anti-tiling, rock PBR and scanned-PBR stages. The canonical
## production Surface graph therefore remains a zero-bytecode pass-through.

const GRAPH_SCRIPT := preload(
	"res://scripts/world_authoring/model/terrain_shader_graph_definition.gd")
const CONTROL_TYPES: Array[String] = [
	"PRODUCTION_CLASSIFIER_SETTINGS",
	"PRODUCTION_MICRORELIEF_SETTINGS",
	"PRODUCTION_ANTITILE_SETTINGS",
	"PRODUCTION_ROCK_PBR_SETTINGS",
	"PRODUCTION_SCAN_PBR_SETTINGS",
	"PRODUCTION_SCAN_TEXTURES",
]
const DETAIL_WRAP_M: float = 4096.0
const SAFE_PERIODIC_CELLS: Array[float] = [
	1.0, 2.0, 4.0, 8.0, 16.0, 32.0, 64.0, 128.0, 256.0, 512.0,
]

var _production_controls: Dictionary = {}
var _production_texture_cache: Dictionary = {}
var _production_control_warnings: PackedStringArray = PackedStringArray()


func clear() -> void:
	super.clear()
	_production_controls = _default_controls()
	_production_control_warnings = PackedStringArray()


func compile_from_terrain(terrain: Resource) -> Dictionary:
	_production_control_warnings = PackedStringArray()
	_production_controls = _extract_production_controls(terrain)
	var result: Dictionary = super.compile_from_terrain(terrain)
	result["production_surface_controls"] = _production_controls.duplicate(true)
	result["production_control_warnings"] = _production_control_warnings.duplicate()
	return result


func bind_material(material: ShaderMaterial) -> void:
	super.bind_material(material)
	bind_production_controls(material)


func bind_production_controls(material: ShaderMaterial) -> void:
	if material == null:
		return
	var classifier: Dictionary = _control("PRODUCTION_CLASSIFIER_SETTINGS")
	material.set_shader_parameter("u_classifier_primary_scale", Vector4(
		maxf(float(classifier.get("rock_scale", 1.0)), 0.0),
		maxf(float(classifier.get("soil_scale", 1.0)), 0.0),
		maxf(float(classifier.get("vegetation_scale", 1.0)), 0.0),
		maxf(float(classifier.get("sand_scale", 1.0)), 0.0)))
	material.set_shader_parameter("u_classifier_secondary_scale", Vector4(
		maxf(float(classifier.get("mud_scale", 1.0)), 0.0),
		maxf(float(classifier.get("snow_scale", 1.0)), 0.0),
		maxf(float(classifier.get("scree_scale", 1.0)), 0.0),
		maxf(float(classifier.get("gravel_scale", 1.0)), 0.0)))
	material.set_shader_parameter("u_classifier_albedo_chroma",
		maxf(float(classifier.get("albedo_chroma", 1.24)), 0.0))
	material.set_shader_parameter("u_classifier_albedo_contrast",
		maxf(float(classifier.get("albedo_contrast", 1.10)), 0.0))
	material.set_shader_parameter("u_classifier_albedo_pivot",
		float(classifier.get("albedo_pivot", 0.115)))
	material.set_shader_parameter("u_classifier_roughness_scale",
		maxf(float(classifier.get("roughness_scale", 1.0)), 0.0))
	material.set_shader_parameter("u_classifier_roughness_bias",
		float(classifier.get("roughness_bias", 0.0)))
	material.set_shader_parameter("u_classifier_roughness_min",
		clampf(float(classifier.get("roughness_min", 0.42)), 0.0, 1.0))
	material.set_shader_parameter("u_classifier_roughness_max",
		clampf(float(classifier.get("roughness_max", 0.98)), 0.0, 1.0))

	var micro: Dictionary = _control("PRODUCTION_MICRORELIEF_SETTINGS")
	material.set_shader_parameter("u_microrelief_enabled", 1.0 if bool(micro.get("enabled", true)) else 0.0)
	material.set_shader_parameter("u_microrelief_strength",
		maxf(float(micro.get("strength", 1.0)), 0.0))

	var antitile: Dictionary = _control("PRODUCTION_ANTITILE_SETTINGS")
	material.set_shader_parameter("u_pbr_antitile_strength",
		maxf(float(antitile.get("strength", 1.0)), 0.0))
	var coarse_cell: float = _snap_periodic_cell(float(antitile.get("coarse_cell_m", 32.0)))
	var fine_cell: float = _snap_periodic_cell(float(antitile.get("fine_cell_m", 8.0)))
	material.set_shader_parameter("u_pbr_antitile_coarse_cell_m", coarse_cell)
	material.set_shader_parameter("u_pbr_antitile_fine_cell_m", fine_cell)
	material.set_shader_parameter("u_pbr_antitile_coarse_period",
		maxi(1, int(round(DETAIL_WRAP_M / coarse_cell))))
	material.set_shader_parameter("u_pbr_antitile_fine_period",
		maxi(1, int(round(DETAIL_WRAP_M / fine_cell))))
	material.set_shader_parameter("u_pbr_antitile_coarse_offset_m",
		maxf(float(antitile.get("coarse_offset_m", 0.58)), 0.0))
	material.set_shader_parameter("u_pbr_antitile_fine_offset_m",
		maxf(float(antitile.get("fine_offset_m", 0.14)), 0.0))
	material.set_shader_parameter("u_pbr_antitile_coarse_seed",
		maxi(1, int(antitile.get("coarse_seed", 29093))))
	material.set_shader_parameter("u_pbr_antitile_fine_seed",
		maxi(1, int(antitile.get("fine_seed", 46141))))

	var rock: Dictionary = _control("PRODUCTION_ROCK_PBR_SETTINGS")
	material.set_shader_parameter("u_rock_pbr_enabled", 1.0 if bool(rock.get("enabled", true)) else 0.0)
	material.set_shader_parameter("u_rock_detail_strength",
		maxf(float(rock.get("detail_strength", 1.0)), 0.0))
	material.set_shader_parameter("u_rock_normal_strength",
		maxf(float(rock.get("normal_strength", 1.0)), 0.0))
	material.set_shader_parameter("u_rock_color_strength",
		maxf(float(rock.get("color_strength", 1.0)), 0.0))

	var scan: Dictionary = _control("PRODUCTION_SCAN_PBR_SETTINGS")
	material.set_shader_parameter("u_pbr_enabled", 1.0 if bool(scan.get("enabled", true)) else 0.0)
	material.set_shader_parameter("u_pbr_ground_metres",
		maxf(float(scan.get("ground_metres", 2.0)), 0.001))
	material.set_shader_parameter("u_pbr_grass_metres",
		maxf(float(scan.get("grass_metres", 2.0)), 0.001))
	material.set_shader_parameter("u_pbr_mud_metres",
		maxf(float(scan.get("mud_metres", 1.0)), 0.001))
	material.set_shader_parameter("u_pbr_forest_metres",
		maxf(float(scan.get("forest_metres", 2.0)), 0.001))
	material.set_shader_parameter("u_pbr_triplanar_sharpness",
		maxf(float(scan.get("triplanar_sharpness", 5.0)), 0.01))
	material.set_shader_parameter("u_pbr_transfer_strength",
		maxf(float(scan.get("transfer_strength", 0.60)), 0.0))
	material.set_shader_parameter("u_pbr_transfer_min",
		maxf(float(scan.get("transfer_min", 0.48)), 0.0))
	material.set_shader_parameter("u_pbr_transfer_max",
		maxf(float(scan.get("transfer_max", 1.72)), 0.0))
	_bind_nonnegative(material, "u_pbr_surface_fade_near_m", scan, "surface_fade_near_m", 900.0)
	_bind_nonnegative(material, "u_pbr_surface_fade_far_m", scan, "surface_fade_far_m", 3200.0)
	_bind_nonnegative(material, "u_pbr_normal_fade_near_m", scan, "normal_fade_near_m", 120.0)
	_bind_nonnegative(material, "u_pbr_normal_fade_far_m", scan, "normal_fade_far_m", 520.0)
	_bind_nonnegative(material, "u_pbr_loose_threshold", scan, "loose_threshold", 0.015)
	_bind_nonnegative(material, "u_pbr_loose_albedo_strength", scan, "loose_albedo_strength", 0.72)
	_bind_nonnegative(material, "u_pbr_loose_roughness_strength", scan, "loose_roughness_strength", 0.70)
	_bind_nonnegative(material, "u_pbr_loose_normal_mix", scan, "loose_normal_mix", 0.42)
	_bind_nonnegative(material, "u_pbr_loose_normal_weight", scan, "loose_normal_weight", 0.48)
	_bind_nonnegative(material, "u_pbr_special_threshold", scan, "special_threshold", 0.015)
	_bind_nonnegative(material, "u_pbr_special_albedo_strength", scan, "special_albedo_strength", 0.88)
	_bind_nonnegative(material, "u_pbr_special_roughness_strength", scan, "special_roughness_strength", 0.82)
	_bind_nonnegative(material, "u_pbr_special_normal_mix", scan, "special_normal_mix", 0.58)
	_bind_nonnegative(material, "u_pbr_special_normal_weight", scan, "special_normal_weight", 0.72)
	_bind_scan_textures(material, _control("PRODUCTION_SCAN_TEXTURES"))


func _bind_nonnegative(material: ShaderMaterial, uniform_name: String, controls: Dictionary,
		key: String, fallback: float) -> void:
	material.set_shader_parameter(uniform_name,
		maxf(float(controls.get(key, fallback)), 0.0))


func _snap_periodic_cell(requested: float) -> float:
	var target: float = maxf(requested, 1.0)
	var best: float = SAFE_PERIODIC_CELLS[0]
	var best_error: float = absf(target - best)
	for candidate: float in SAFE_PERIODIC_CELLS:
		var error: float = absf(target - candidate)
		if error < best_error:
			best = candidate
			best_error = error
	return best


func _default_controls() -> Dictionary:
	var out: Dictionary = {}
	for node_type: String in CONTROL_TYPES:
		out[node_type] = GRAPH_SCRIPT.production_control_defaults(node_type)
	return out


func _control(node_type: String) -> Dictionary:
	var value: Variant = _production_controls.get(node_type,
		GRAPH_SCRIPT.production_control_defaults(node_type))
	return value as Dictionary if value is Dictionary else GRAPH_SCRIPT.production_control_defaults(node_type)


func _extract_production_controls(terrain: Resource) -> Dictionary:
	var out: Dictionary = _default_controls()
	if terrain == null:
		return out
	var slots_value: Variant = terrain.get(&"material_slots")
	if not (slots_value is Array):
		return out
	for slot_value: Variant in slots_value as Array:
		var slot: Resource = slot_value as Resource
		if slot == null or not bool(slot.get(&"enabled")) or int(slot.get(&"domain")) != 1:
			continue
		var graph: Resource = slot.get(&"graph") as Resource
		if graph == null:
			continue
		for node_value: Variant in graph.get(&"nodes") as Array:
			if not (node_value is Dictionary):
				continue
			var node: Dictionary = node_value as Dictionary
			var node_type: String = String(node.get("type", ""))
			if not CONTROL_TYPES.has(node_type):
				continue
			var merged: Dictionary = GRAPH_SCRIPT.production_control_defaults(node_type)
			var parameters: Dictionary = node.get("parameters", {}) as Dictionary
			for key: Variant in parameters.keys():
				merged[key] = parameters[key]
			out[node_type] = merged
	return out


func _bind_scan_textures(material: ShaderMaterial, controls: Dictionary) -> void:
	var mapping: Dictionary = {
		"ground_albedo": "u_pbr_ground_albedo",
		"ground_normal": "u_pbr_ground_normal",
		"ground_roughness": "u_pbr_ground_roughness",
		"grass_albedo": "u_pbr_grass_albedo",
		"grass_normal": "u_pbr_grass_normal",
		"grass_roughness": "u_pbr_grass_roughness",
		"mud_albedo": "u_pbr_mud_albedo",
		"mud_normal": "u_pbr_mud_normal",
		"mud_roughness": "u_pbr_mud_roughness",
		"forest_albedo": "u_pbr_forest_albedo",
		"forest_normal": "u_pbr_forest_normal",
		"forest_roughness": "u_pbr_forest_roughness",
	}
	for key: Variant in mapping.keys():
		var path: String = String(controls.get(key, "")).strip_edges()
		if path.is_empty():
			continue
		var texture: Texture2D = _production_texture(path)
		if texture != null:
			material.set_shader_parameter(String(mapping[key]), texture)


func _production_texture(path: String) -> Texture2D:
	if _production_texture_cache.has(path):
		return _production_texture_cache[path] as Texture2D
	if not ResourceLoader.exists(path):
		var missing := "Production terrain scan texture does not exist: %s" % path
		if not _production_control_warnings.has(missing):
			_production_control_warnings.append(missing)
		return null
	var resource: Resource = load(path)
	if not (resource is Texture2D):
		var invalid := "Production terrain scan resource is not Texture2D: %s" % path
		if not _production_control_warnings.has(invalid):
			_production_control_warnings.append(invalid)
		return null
	var texture := resource as Texture2D
	_production_texture_cache[path] = texture
	return texture


func _is_identity_surface_slot(slot: Resource) -> bool:
	if String(slot.get(&"slot_id")) != PRODUCTION_SURFACE_SLOT_ID:
		return false
	if int(slot.get(&"domain")) != 1 or int(slot.get(&"blend_mode")) != 5:
		return false
	if absf(float(slot.get(&"strength")) - 1.0) > 1e-6:
		return false
	var graph: Resource = slot.get(&"graph") as Resource
	if graph == null:
		return false
	var nodes: Array = graph.get(&"nodes") as Array
	var links: Array = graph.get(&"links") as Array
	if links.size() != 6:
		return false
	var output_id: String = ""
	var source_by_id: Dictionary = {}
	var seen_controls: Dictionary = {}
	for node_value: Variant in nodes:
		if not (node_value is Dictionary):
			return false
		var node: Dictionary = node_value as Dictionary
		var node_id: String = String(node.get("id", ""))
		var node_type: String = String(node.get("type", ""))
		if node_type == "OUTPUT_MATERIAL":
			output_id = node_id
			continue
		if CONTROL_TYPES.has(node_type):
			if seen_controls.has(node_type):
				return false
			seen_controls[node_type] = true
			continue
		var channel: int = _identity_surface_channel(node)
		if channel < 0:
			return false
		source_by_id[node_id] = channel
	if output_id.is_empty() or source_by_id.size() != 6:
		return false
	var seen: Dictionary = {}
	for link_value: Variant in links:
		var link: Dictionary = link_value as Dictionary
		if String(link.get("to", "")) != output_id:
			return false
		var from_id: String = String(link.get("from", ""))
		if not source_by_id.has(from_id):
			return false
		var source_channel: int = int(source_by_id[from_id])
		var output_channel: int = int(link.get("to_port", -1))
		if source_channel != output_channel or seen.has(output_channel):
			return false
		seen[output_channel] = true
	return seen.size() == 6


func stats() -> Dictionary:
	var out: Dictionary = super.stats()
	out["production_surface_controls"] = _production_controls.duplicate(true)
	out["production_control_warnings"] = _production_control_warnings.duplicate()
	return out
