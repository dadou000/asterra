extends "res://scripts/world_authoring/world_authoring_editor_live_phase30.gd"
## Phase 31/37: renderer-owned terrain stages are explicit graph sources and
## production controls. Untouched older identity graphs are upgraded once; custom
## graph topology is never replaced. Phase 37 keeps Simple/Detailed/Node Graph plus
## staged Preview / Applied Original controls while exposing the resident production
## geomorph stages structurally.

const ACTIVE_GRAPH_EDITOR := preload(
	"res://scripts/world_authoring/terrain_graph_editor_phase37.gd")


func _phase29_create_production_slot(terrain: Resource, domain: int,
		slot_id: String, display_name: String) -> Resource:
	var slot: Resource = super._phase29_create_production_slot(
		terrain, domain, slot_id, display_name)
	if slot != null:
		_phase31_install_production_stage_graph(slot, domain)
	return slot


func _phase29_ensure_production_graphs(terrain: Resource) -> void:
	super._phase29_ensure_production_graphs(terrain)
	var migrated: bool = false
	var shape: Resource = terrain.call("find_shader_slot", PRODUCTION_SHAPE_SLOT_ID) as Resource
	if shape != null and (_phase31_is_legacy_identity_shape(shape) \
			or _phase32_is_stage_identity_shape(shape)):
		_phase31_install_production_stage_graph(shape, SHADER_SLOT_MODEL.Domain.DISPLACEMENT)
		migrated = true
	var surface: Resource = terrain.call("find_shader_slot", PRODUCTION_SURFACE_SLOT_ID) as Resource
	if surface != null and (_phase31_is_legacy_identity_surface(surface) \
			or _phase32_is_stage_identity_surface(surface)):
		_phase31_install_production_stage_graph(surface, SHADER_SLOT_MODEL.Domain.MATERIAL)
		migrated = true
	if migrated:
		_session.call("_mark_dirty", WorldAuthoringSession.ApplyScope.GRAPH)


func _phase31_install_production_stage_graph(slot: Resource, domain: int) -> void:
	var graph: Resource = slot.get(&"graph") as Resource if slot != null else null
	if graph == null:
		return
	if graph.has_method("create_production_stage_graph"):
		graph.call("create_production_stage_graph", domain)
	else:
		graph.call("create_production_graph", domain)
	graph.set(&"display_name", "Base Terrain Graph" if domain == SHADER_SLOT_MODEL.Domain.DISPLACEMENT \
		else "Base Surface Graph")


func _phase31_is_legacy_identity_shape(slot: Resource) -> bool:
	if slot == null or String(slot.get(&"slot_id")) != PRODUCTION_SHAPE_SLOT_ID:
		return false
	var graph: Resource = slot.get(&"graph") as Resource
	if graph == null or int(graph.get(&"displacement_output_mode")) != 1:
		return false
	var nodes: Array = graph.get(&"nodes") as Array
	var links: Array = graph.get(&"links") as Array
	if nodes.size() != 4 or links.size() != 3:
		return false
	var generated_id: String = ""
	var sculpt_id: String = ""
	var add_id: String = ""
	var output_id: String = ""
	for node_value: Variant in nodes:
		var node: Dictionary = node_value as Dictionary
		var node_type: String = String(node.get("type", ""))
		var node_id: String = String(node.get("id", ""))
		if node_type == "GAME_INPUT":
			var parameters: Dictionary = node.get("parameters", {}) as Dictionary
			match String(parameters.get("source", "")):
				"generated_height_m": generated_id = node_id
				"sculpt_delta_m": sculpt_id = node_id
		elif node_type == "ADD":
			add_id = node_id
		elif node_type == "OUTPUT_DISPLACEMENT":
			output_id = node_id
	if generated_id.is_empty() or sculpt_id.is_empty() or add_id.is_empty() or output_id.is_empty():
		return false
	return _phase31_identity_links(links, generated_id, sculpt_id, add_id, output_id)


func _phase32_is_stage_identity_shape(slot: Resource) -> bool:
	if slot == null or String(slot.get(&"slot_id")) != PRODUCTION_SHAPE_SLOT_ID:
		return false
	var graph: Resource = slot.get(&"graph") as Resource
	if graph == null or int(graph.get(&"displacement_output_mode")) != 1:
		return false
	var nodes: Array = graph.get(&"nodes") as Array
	var links: Array = graph.get(&"links") as Array
	if nodes.size() != 4 or links.size() != 3:
		return false
	var generated_id: String = ""
	var sculpt_id: String = ""
	var add_id: String = ""
	var output_id: String = ""
	for node_value: Variant in nodes:
		var node: Dictionary = node_value as Dictionary
		var node_id: String = String(node.get("id", ""))
		match String(node.get("type", "")):
			"PRODUCTION_GENERATED_HEIGHT": generated_id = node_id
			"PRODUCTION_SCULPT_DELTA": sculpt_id = node_id
			"ADD": add_id = node_id
			"OUTPUT_DISPLACEMENT": output_id = node_id
			_: return false
	if generated_id.is_empty() or sculpt_id.is_empty() or add_id.is_empty() or output_id.is_empty():
		return false
	return _phase31_identity_links(links, generated_id, sculpt_id, add_id, output_id)


func _phase31_identity_links(links: Array, generated_id: String, sculpt_id: String,
		add_id: String, output_id: String) -> bool:
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


func _phase31_is_legacy_identity_surface(slot: Resource) -> bool:
	if slot == null or String(slot.get(&"slot_id")) != PRODUCTION_SURFACE_SLOT_ID:
		return false
	var graph: Resource = slot.get(&"graph") as Resource
	if graph == null:
		return false
	var nodes: Array = graph.get(&"nodes") as Array
	var links: Array = graph.get(&"links") as Array
	if nodes.size() != 7 or links.size() != 6:
		return false
	var output_id: String = ""
	var channel_by_id: Dictionary = {}
	for node_value: Variant in nodes:
		var node: Dictionary = node_value as Dictionary
		var node_type: String = String(node.get("type", ""))
		var node_id: String = String(node.get("id", ""))
		if node_type == "OUTPUT_MATERIAL":
			output_id = node_id
			continue
		if node_type != "GAME_INPUT":
			return false
		var parameters: Dictionary = node.get("parameters", {}) as Dictionary
		var source: String = String(parameters.get("source", ""))
		var channel: int = ["base_albedo", "base_normal", "base_roughness",
			"base_metallic", "base_ao", "base_specular"].find(source)
		if channel < 0:
			return false
		channel_by_id[node_id] = channel
	return _phase31_surface_identity_links(links, output_id, channel_by_id)


func _phase32_is_stage_identity_surface(slot: Resource) -> bool:
	if slot == null or String(slot.get(&"slot_id")) != PRODUCTION_SURFACE_SLOT_ID:
		return false
	var graph: Resource = slot.get(&"graph") as Resource
	if graph == null:
		return false
	var nodes: Array = graph.get(&"nodes") as Array
	var links: Array = graph.get(&"links") as Array
	if nodes.size() != 7 or links.size() != 6:
		return false
	var output_id: String = ""
	var channel_by_id: Dictionary = {}
	for node_value: Variant in nodes:
		var node: Dictionary = node_value as Dictionary
		var node_id: String = String(node.get("id", ""))
		var channel: int = -1
		match String(node.get("type", "")):
			"OUTPUT_MATERIAL":
				output_id = node_id
				continue
			"PRODUCTION_ALBEDO": channel = 0
			"PRODUCTION_NORMAL": channel = 1
			"PRODUCTION_ROUGHNESS": channel = 2
			"PRODUCTION_METALLIC": channel = 3
			"PRODUCTION_AO": channel = 4
			"PRODUCTION_SPECULAR": channel = 5
			_: return false
		channel_by_id[node_id] = channel
	return _phase31_surface_identity_links(links, output_id, channel_by_id)


func _phase31_surface_identity_links(links: Array, output_id: String,
		channel_by_id: Dictionary) -> bool:
	if output_id.is_empty() or channel_by_id.size() != 6:
		return false
	var seen: Dictionary = {}
	for link_value: Variant in links:
		var link: Dictionary = link_value as Dictionary
		if String(link.get("to", "")) != output_id:
			return false
		var from_id: String = String(link.get("from", ""))
		if not channel_by_id.has(from_id):
			return false
		var channel: int = int(channel_by_id[from_id])
		if channel != int(link.get("to_port", -1)) or seen.has(channel):
			return false
		seen[channel] = true
	return seen.size() == 6


func _phase29_reset_production_graph(slot: Resource) -> void:
	if slot == null:
		return
	var slot_id: String = String(slot.get(&"slot_id"))
	if slot_id != PRODUCTION_SHAPE_SLOT_ID and slot_id != PRODUCTION_SURFACE_SLOT_ID:
		super._phase29_reset_production_graph(slot)
		return
	var domain: int = int(slot.get(&"domain"))
	_session.stage_action("Reset production terrain graph", func() -> void:
		_phase31_install_production_stage_graph(slot, domain)
	, WorldAuthoringSession.ApplyScope.GRAPH)
	_refresh_current_category()


func _phase29_build_graph_editor(slot: Resource) -> void:
	var graph_editor := ACTIVE_GRAPH_EDITOR.new()
	graph_editor.custom_minimum_size = Vector2(1180.0, 760.0)
	_workspace.add_child(graph_editor)
	graph_editor.call_deferred("setup", _session, slot,
		Callable(self, "_refresh_current_category"))
	var hint := Label.new()
	hint.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	if _phase28_domain == SHADER_SLOT_MODEL.Domain.DISPLACEMENT:
		hint.text = "SIMPLE and DETAILED edit the same exact production-stage parameters as NODE GRAPH. PREVIEW shows staged edits through the authoritative render/contact path; HOLD ORIGINAL switches back to the last applied snapshot without discarding work. Canonical production stages stay on the resident zero-bytecode shader path; incomplete structural rewires are rejected and keep the last valid terrain active."
	else:
		hint.text = "The existing production surface is exposed through PBR outputs plus classifier, classifier thresholds, palette/materials, microrelief, anti-tiling, geology rock PBR, scanned PBR, exact scan textures, raw world fields and graph math/texture nodes. Reset Flow restores the current production defaults."
	hint.modulate = Color(0.58, 0.69, 0.78)
	_workspace.add_child(hint)
