extends "res://scripts/world_authoring/terrain_displacement_runtime_phase31.gd"
## Phase 32/42: bind serialized production geomorph settings directly into the
## authoritative cached terrain shader. The visible production Shape graph remains
## bytecode-free while it is an identity graph; settings are ordinary uniforms.
##
## Phase 42 centralizes all safety clamps/defaults in TerrainGeomorphGPUContract so
## the visible analytic shader and warm cache consume the same normalized snapshot.

const GRAPH_SCRIPT := preload(
	"res://scripts/world_authoring/model/terrain_shader_graph_definition.gd")
const GEOMORPH_CONTRACT := preload(
	"res://scripts/world_authoring/model/terrain_geomorph_gpu_contract.gd")
const GEOMORPH_SETTINGS := "PRODUCTION_GEOMORPH_SETTINGS"

var _production_controls: Dictionary = GRAPH_SCRIPT.production_control_defaults(GEOMORPH_SETTINGS)


func clear() -> void:
	super.clear()
	_production_controls = GRAPH_SCRIPT.production_control_defaults(GEOMORPH_SETTINGS)


func compile_from_terrain(terrain: Resource) -> Dictionary:
	_production_controls = _extract_geomorph_controls(terrain)
	var result: Dictionary = super.compile_from_terrain(terrain)
	result["production_geomorph_controls"] = _production_controls.duplicate(true)
	return result


func bind_material(material: ShaderMaterial) -> void:
	super.bind_material(material)
	bind_production_controls(material)


func bind_production_controls(material: ShaderMaterial) -> void:
	if material == null:
		return
	var c: Dictionary = GEOMORPH_CONTRACT.normalized_controls(_production_controls)
	material.set_shader_parameter("u_detail_strength",
		float(c.get("detail_strength", 1.0)))
	material.set_shader_parameter("u_geomorph_warp_strength",
		float(c.get("warp_strength", 1.0)))
	material.set_shader_parameter("u_geomorph_broad_strength",
		float(c.get("broad_strength", 1.0)))
	material.set_shader_parameter("u_geomorph_mountain_strength",
		float(c.get("mountain_strength", 1.0)))
	material.set_shader_parameter("u_geomorph_mid_strength",
		float(c.get("mid_strength", 1.0)))
	material.set_shader_parameter("u_geomorph_channel_strength",
		float(c.get("channel_strength", 1.0)))
	material.set_shader_parameter("u_geomorph_deposit_strength",
		float(c.get("deposit_strength", 1.0)))
	material.set_shader_parameter("u_geomorph_fine_strength",
		float(c.get("fine_strength", 1.0)))
	material.set_shader_parameter("u_geomorph_dune_strength",
		float(c.get("dune_strength", 1.0)))
	material.set_shader_parameter("u_geomorph_glacial_strength",
		float(c.get("glacial_strength", 1.0)))

	# Physical geometry of the production bands. Every value is already normalized
	# by the shared contract; helpers remain for readable uniform mapping only.
	_bind_positive(material, "u_geomorph_broad_wavelength_m", c, "broad_wavelength_m", 16000.0, 0.001)
	_bind_nonnegative(material, "u_geomorph_broad_low_amplitude_m", c, "broad_low_amplitude_m", 24.0)
	_bind_nonnegative(material, "u_geomorph_broad_mountain_amplitude_m", c, "broad_mountain_amplitude_m", 125.0)
	_bind_nonnegative(material, "u_geomorph_broad_warp", c, "broad_warp", 0.8)
	_bind_positive(material, "u_geomorph_mountain_wavelength_m", c, "mountain_wavelength_m", 6000.0, 0.001)
	_bind_nonnegative(material, "u_geomorph_mountain_amplitude_m", c, "mountain_amplitude_m", 210.0)
	_bind_nonnegative(material, "u_geomorph_mountain_warp", c, "mountain_warp", 1.1)
	_bind_positive(material, "u_geomorph_mountain_ridge_scale", c, "mountain_ridge_scale", 1.55, 0.001)
	material.set_shader_parameter("u_geomorph_mountain_cell_mix",
		float(c.get("mountain_cell_mix", 0.58)))
	_bind_positive(material, "u_geomorph_mid_wavelength_m", c, "mid_wavelength_m", 1400.0, 0.001)
	_bind_nonnegative(material, "u_geomorph_mid_ridge_amplitude_m", c, "mid_ridge_amplitude_m", 72.0)
	_bind_nonnegative(material, "u_geomorph_mid_noise_amplitude_m", c, "mid_noise_amplitude_m", 24.0)
	_bind_nonnegative(material, "u_geomorph_mid_warp", c, "mid_warp", 0.72)
	_bind_positive(material, "u_geomorph_mid_ridge_scale", c, "mid_ridge_scale", 1.25, 0.001)
	_bind_positive(material, "u_geomorph_mid_detail_scale", c, "mid_detail_scale", 2.1, 0.001)
	_bind_positive(material, "u_geomorph_channel_wavelength_m", c, "channel_wavelength_m", 420.0, 0.001)
	_bind_nonnegative(material, "u_geomorph_channel_depth_min_m", c, "channel_depth_min_m", 2.0)
	_bind_nonnegative(material, "u_geomorph_channel_depth_max_m", c, "channel_depth_max_m", 34.0)
	_bind_nonnegative(material, "u_geomorph_channel_warp", c, "channel_warp", 0.55)
	_bind_positive(material, "u_geomorph_channel_power", c, "channel_power", 4.6, 0.01)
	_bind_positive(material, "u_geomorph_flow_along_scale", c, "flow_along_scale", 0.42, 0.001)
	_bind_positive(material, "u_geomorph_flow_across_scale", c, "flow_across_scale", 1.45, 0.001)
	_bind_nonnegative(material, "u_geomorph_deposit_amplitude_min_m", c, "deposit_amplitude_min_m", 1.0)
	_bind_nonnegative(material, "u_geomorph_deposit_amplitude_max_m", c, "deposit_amplitude_max_m", 12.0)
	_bind_positive(material, "u_geomorph_deposit_scale", c, "deposit_scale", 0.48, 0.001)
	_bind_positive(material, "u_geomorph_deposit_power", c, "deposit_power", 2.2, 0.01)
	_bind_positive(material, "u_geomorph_fine_wavelength_m", c, "fine_wavelength_m", 120.0, 0.001)
	_bind_nonnegative(material, "u_geomorph_fine_amplitude_m", c, "fine_amplitude_m", 4.5)
	_bind_positive(material, "u_geomorph_dune_wavelength_m", c, "dune_wavelength_m", 180.0, 0.001)
	_bind_nonnegative(material, "u_geomorph_dune_amplitude_m", c, "dune_amplitude_m", 9.0)
	_bind_nonnegative(material, "u_geomorph_dune_warp", c, "dune_warp", 0.45)
	_bind_positive(material, "u_geomorph_micro_wavelength_m", c, "micro_wavelength_m", 24.0, 0.001)
	_bind_nonnegative(material, "u_geomorph_micro_amplitude_m", c, "micro_amplitude_m", 0.9)
	_bind_positive(material, "u_geomorph_glacial_wavelength_m", c, "glacial_wavelength_m", 2600.0, 0.001)
	_bind_nonnegative(material, "u_geomorph_glacial_amplitude_m", c, "glacial_amplitude_m", 52.0)
	_bind_nonnegative(material, "u_geomorph_glacial_base_scale", c, "glacial_base_scale", 0.62)
	_bind_nonnegative(material, "u_geomorph_glacial_mix", c, "glacial_mix", 0.72)

	if bool(c.get("override_seed", false)):
		material.set_shader_parameter("u_detail_seed", int(c.get("detail_seed", 1337)))


func _bind_positive(material: ShaderMaterial, uniform_name: String, controls: Dictionary,
		key: String, fallback: float, minimum: float) -> void:
	material.set_shader_parameter(uniform_name,
		maxf(float(controls.get(key, fallback)), minimum))


func _bind_nonnegative(material: ShaderMaterial, uniform_name: String, controls: Dictionary,
		key: String, fallback: float) -> void:
	material.set_shader_parameter(uniform_name,
		maxf(float(controls.get(key, fallback)), 0.0))


func _extract_geomorph_controls(terrain: Resource) -> Dictionary:
	var out: Dictionary = GRAPH_SCRIPT.production_control_defaults(GEOMORPH_SETTINGS)
	if terrain == null:
		return out
	var slots_value: Variant = terrain.get(&"displacement_slots")
	if not (slots_value is Array):
		return out
	for slot_value: Variant in slots_value as Array:
		var slot: Resource = slot_value as Resource
		if slot == null or not bool(slot.get(&"enabled")) or int(slot.get(&"domain")) != 0:
			continue
		var graph: Resource = slot.get(&"graph") as Resource
		if graph == null:
			continue
		for node_value: Variant in graph.get(&"nodes") as Array:
			var node: Dictionary = node_value as Dictionary
			if String(node.get("type", "")) != GEOMORPH_SETTINGS:
				continue
			var parameters: Dictionary = node.get("parameters", {}) as Dictionary
			for key: Variant in parameters.keys():
				out[key] = parameters[key]
	return out


func _is_identity_shape_slot(slot: Resource) -> bool:
	if String(slot.get(&"slot_id")) != PRODUCTION_SHAPE_SLOT_ID:
		return false
	var graph: Resource = slot.get(&"graph") as Resource
	if graph == null or int(graph.get(&"displacement_output_mode")) != GRAPH_OUTPUT_ABSOLUTE_HEIGHT:
		return false
	var nodes: Array = graph.get(&"nodes") as Array
	var links: Array = graph.get(&"links") as Array
	if links.size() != 3:
		return false
	var generated_id: String = ""
	var sculpt_id: String = ""
	var add_id: String = ""
	var output_id: String = ""
	var settings_count: int = 0
	for node_value: Variant in nodes:
		if not (node_value is Dictionary):
			return false
		var node: Dictionary = node_value as Dictionary
		var node_type: String = String(node.get("type", ""))
		var node_id: String = String(node.get("id", ""))
		match node_type:
			"PRODUCTION_GENERATED_HEIGHT": generated_id = node_id
			"PRODUCTION_SCULPT_DELTA": sculpt_id = node_id
			"ADD": add_id = node_id
			"OUTPUT_DISPLACEMENT": output_id = node_id
			GEOMORPH_SETTINGS: settings_count += 1
			"GAME_INPUT":
				var parameters: Dictionary = node.get("parameters", {}) as Dictionary
				var source: String = String(parameters.get("source", ""))
				if source == "generated_height_m": generated_id = node_id
				elif source == "sculpt_delta_m": sculpt_id = node_id
				else: return false
			_: return false
	if nodes.size() != 4 + settings_count or settings_count > 1:
		return false
	if generated_id.is_empty() or sculpt_id.is_empty() or add_id.is_empty() or output_id.is_empty():
		return false
	var expected: Dictionary = {
		"%s:0" % add_id: generated_id,
		"%s:1" % add_id: sculpt_id,
		"%s:0" % output_id: add_id,
	}
	for link_value: Variant in links:
		var link: Dictionary = link_value as Dictionary
		var key: String = "%s:%d" % [String(link.get("to", "")), int(link.get("to_port", -1))]
		if String(expected.get(key, "")) != String(link.get("from", "")):
			return false
		expected.erase(key)
	return expected.is_empty()


func stats() -> Dictionary:
	var out: Dictionary = super.stats()
	out["production_geomorph_controls"] = _production_controls.duplicate(true)
	out["production_geomorph_contract_version"] = GEOMORPH_CONTRACT.CONTRACT_VERSION
	return out
