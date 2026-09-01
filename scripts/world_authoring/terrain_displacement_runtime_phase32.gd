extends "res://scripts/world_authoring/terrain_displacement_runtime_phase31.gd"
## Phase 32: bind the serialized production geomorph settings directly into the
## authoritative cached terrain shader. The visible production Shape graph remains
## bytecode-free while it is an identity graph; settings are ordinary uniforms.

const GRAPH_SCRIPT := preload(
	"res://scripts/world_authoring/model/terrain_shader_graph_definition.gd")
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
	var c: Dictionary = _production_controls
	material.set_shader_parameter("u_detail_strength",
		maxf(float(c.get("detail_strength", 1.0)), 0.0))
	material.set_shader_parameter("u_geomorph_warp_strength",
		maxf(float(c.get("warp_strength", 1.0)), 0.0))
	material.set_shader_parameter("u_geomorph_broad_strength",
		maxf(float(c.get("broad_strength", 1.0)), 0.0))
	material.set_shader_parameter("u_geomorph_mountain_strength",
		maxf(float(c.get("mountain_strength", 1.0)), 0.0))
	material.set_shader_parameter("u_geomorph_mid_strength",
		maxf(float(c.get("mid_strength", 1.0)), 0.0))
	material.set_shader_parameter("u_geomorph_channel_strength",
		maxf(float(c.get("channel_strength", 1.0)), 0.0))
	material.set_shader_parameter("u_geomorph_deposit_strength",
		maxf(float(c.get("deposit_strength", 1.0)), 0.0))
	material.set_shader_parameter("u_geomorph_fine_strength",
		maxf(float(c.get("fine_strength", 1.0)), 0.0))
	material.set_shader_parameter("u_geomorph_dune_strength",
		maxf(float(c.get("dune_strength", 1.0)), 0.0))
	material.set_shader_parameter("u_geomorph_glacial_strength",
		maxf(float(c.get("glacial_strength", 1.0)), 0.0))
	if bool(c.get("override_seed", false)):
		material.set_shader_parameter("u_detail_seed", maxi(1, int(c.get("detail_seed", 1337))))


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
	return out
