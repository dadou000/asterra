extends "res://scripts/world_authoring/terrain_material_runtime_phase30.gd"
## Phase 31: production-stage Surface graph compiler.
##
## The mature renderer remains the source of truth for its production PBR result,
## but that result and its classifier outputs are now explicit graph nodes. Phase
## 32 adds unconnected production settings nodes; those describe/bind renderer
## stages and do not make the canonical surface pass-through bytecode-active.

# Keep in lock-step with terrain_author_material_bytecode.gdshaderinc.
const OP_CHANNEL_R := 61
const OP_CHANNEL_G := 62
const OP_CHANNEL_B := 63
const OP_CHANNEL_A := 64
const OP_COMBINE_RGB := 65
const OP_SATURATE := 66
const OP_ONE_MINUS := 67

const PRODUCTION_SETTING_TYPES: Array[String] = [
	"PRODUCTION_CLASSIFIER_SETTINGS",
	"PRODUCTION_MICRORELIEF_SETTINGS",
	"PRODUCTION_ANTITILE_SETTINGS",
	"PRODUCTION_ROCK_PBR_SETTINGS",
	"PRODUCTION_SCAN_PBR_SETTINGS",
	"PRODUCTION_SCAN_TEXTURES",
]


func compile_from_terrain(terrain: Resource) -> Dictionary:
	if terrain == null:
		return super.compile_from_terrain(terrain)
	var skipped: Array[Resource] = []
	var slots_value: Variant = terrain.get(&"material_slots")
	if slots_value is Array:
		for slot_value: Variant in slots_value as Array:
			var slot: Resource = slot_value as Resource
			if slot != null and bool(slot.get(&"enabled")) and _is_identity_surface_slot(slot):
				slot.set(&"enabled", false)
				skipped.append(slot)
	var result: Dictionary = super.compile_from_terrain(terrain)
	for slot: Resource in skipped:
		slot.set(&"enabled", true)
	_fingerprint = profile_fingerprint(terrain)
	result["production_identity_skipped"] = not skipped.is_empty()
	return result


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
	var seen_settings: Dictionary = {}
	for node_value: Variant in nodes:
		if not (node_value is Dictionary):
			return false
		var node: Dictionary = node_value as Dictionary
		var node_id: String = String(node.get("id", ""))
		var node_type: String = String(node.get("type", ""))
		if node_type == "OUTPUT_MATERIAL":
			output_id = node_id
			continue
		if PRODUCTION_SETTING_TYPES.has(node_type):
			if seen_settings.has(node_type):
				return false
			seen_settings[node_type] = true
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


func _identity_surface_channel(node: Dictionary) -> int:
	var node_type: String = String(node.get("type", ""))
	match node_type:
		"PRODUCTION_ALBEDO": return 0
		"PRODUCTION_NORMAL": return 1
		"PRODUCTION_ROUGHNESS": return 2
		"PRODUCTION_METALLIC": return 3
		"PRODUCTION_AO": return 4
		"PRODUCTION_SPECULAR": return 5
		"GAME_INPUT":
			var parameters: Dictionary = node.get("parameters", {}) as Dictionary
			match String(parameters.get("source", "")):
				"base_albedo": return 0
				"base_normal": return 1
				"base_roughness": return 2
				"base_metallic": return 3
				"base_ao": return 4
				"base_specular": return 5
	return -1


func _compile_node(node_id: String, nodes: Dictionary, inputs: Dictionary,
		memo: Dictionary, visiting: Dictionary, graph_seed: int) -> int:
	if memo.has(node_id):
		return int(memo[node_id])
	if not nodes.has(node_id):
		return super._compile_node(node_id, nodes, inputs, memo, visiting, graph_seed)
	var node: Dictionary = nodes[node_id] as Dictionary
	var node_type: String = String(node.get("type", ""))
	var result: int = -1
	match node_type:
		"PRODUCTION_ALBEDO": result = _append_instruction(OP_INPUT_BASE_ALBEDO)
		"PRODUCTION_NORMAL": result = _append_instruction(OP_INPUT_BASE_NORMAL)
		"PRODUCTION_ROUGHNESS": result = _append_instruction(OP_INPUT_BASE_ROUGHNESS)
		"PRODUCTION_METALLIC": result = _append_instruction(OP_INPUT_BASE_METALLIC)
		"PRODUCTION_AO": result = _append_instruction(OP_INPUT_BASE_AO)
		"PRODUCTION_SPECULAR": result = _append_instruction(OP_INPUT_BASE_SPECULAR)
		"CLASSIFIER_PRIMARY": result = _append_instruction(OP_INPUT_PRIMARY)
		"CLASSIFIER_SECONDARY": result = _append_instruction(OP_INPUT_SECONDARY)
		"CHANNEL_R", "CHANNEL_G", "CHANNEL_B", "CHANNEL_A":
			var source: int = _compile_input(node_id, 0, nodes, inputs,
				memo, visiting, graph_seed)
			var op: int = OP_CHANNEL_R
			if node_type == "CHANNEL_G": op = OP_CHANNEL_G
			elif node_type == "CHANNEL_B": op = OP_CHANNEL_B
			elif node_type == "CHANNEL_A": op = OP_CHANNEL_A
			result = _append_instruction(op, source)
		"COMBINE_RGB":
			result = _append_instruction(OP_COMBINE_RGB,
				_compile_input(node_id, 0, nodes, inputs, memo, visiting, graph_seed),
				_compile_input(node_id, 1, nodes, inputs, memo, visiting, graph_seed),
				_compile_input(node_id, 2, nodes, inputs, memo, visiting, graph_seed))
		"SATURATE":
			result = _append_instruction(OP_SATURATE,
				_compile_input(node_id, 0, nodes, inputs, memo, visiting, graph_seed))
		"ONE_MINUS":
			result = _append_instruction(OP_ONE_MINUS,
				_compile_input(node_id, 0, nodes, inputs, memo, visiting, graph_seed))
		_:
			return super._compile_node(node_id, nodes, inputs, memo, visiting, graph_seed)
	memo[node_id] = result
	return result
