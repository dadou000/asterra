extends "res://scripts/world_authoring/world_authoring_editor_live_phase30.gd"
## Phase 31: renderer-owned terrain stages become explicit graph sources.
##
## Shape keeps the exact generated + sculpt composition introduced in Phase 30.
## Surface now shows the six production PBR channels as dedicated removable nodes,
## while raw world/classifier vectors are available from the categorized node picker.

const PHASE31_GRAPH_EDITOR := preload(
	"res://scripts/world_authoring/terrain_graph_editor_phase31.gd")


func _phase29_ensure_production_graphs(terrain: Resource) -> void:
	super._phase29_ensure_production_graphs(terrain)
	var surface: Resource = terrain.call("find_shader_slot", PRODUCTION_SURFACE_SLOT_ID) as Resource
	if surface != null and _phase31_is_legacy_identity_surface(surface):
		var graph: Resource = surface.get(&"graph") as Resource
		if graph != null:
			graph.call("create_production_graph", SHADER_SLOT_MODEL.Domain.MATERIAL)
			graph.set(&"display_name", "Base Surface Graph")
			_session.call("_mark_dirty", WorldAuthoringSession.ApplyScope.GRAPH)


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
	if output_id.is_empty() or channel_by_id.size() != 6:
		return false
	for link_value: Variant in links:
		var link: Dictionary = link_value as Dictionary
		if String(link.get("to", "")) != output_id:
			return false
		var from_id: String = String(link.get("from", ""))
		if not channel_by_id.has(from_id) or int(channel_by_id[from_id]) != int(link.get("to_port", -1)):
			return false
	return true


func _phase29_build_graph_editor(slot: Resource) -> void:
	var graph_editor := PHASE31_GRAPH_EDITOR.new()
	graph_editor.custom_minimum_size = Vector2(1080.0, 680.0)
	_workspace.add_child(graph_editor)
	graph_editor.call_deferred("setup", _session, slot,
		Callable(self, "_refresh_current_category"))
	var hint := Label.new()
	hint.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	if _phase28_domain == SHADER_SLOT_MODEL.Domain.DISPLACEMENT:
		hint.text = "GENERATED TERRAIN and SCULPT / EDIT DELTA are the real production shape inputs. Delete, scale, subtract, mix or replace either source; FINAL TERRAIN remains shared by rendering and contact physics."
	else:
		hint.text = "The production PBR channels are explicit removable nodes. Production stages, raw soil/surface/geology/climate/hydrology/landform data, classifier weights, textures, channel tools and math can all be wired directly to FINAL SURFACE."
	hint.modulate = Color(0.58, 0.69, 0.78)
	_workspace.add_child(hint)
