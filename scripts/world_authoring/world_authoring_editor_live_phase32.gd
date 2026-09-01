extends "res://scripts/world_authoring/world_authoring_editor_live_phase31.gd"
## Phase 32: expose controls for the production terrain implementation itself.
## Untouched Phase 31 identity graphs are upgraded once; authored/custom graph
## topology is never replaced by this migration.

const PHASE32_GRAPH_EDITOR := preload(
	"res://scripts/world_authoring/terrain_graph_editor_phase32.gd")
const PHASE32_GRAPH_SCRIPT := preload(
	"res://scripts/world_authoring/model/terrain_shader_graph_definition.gd")


func _phase29_ensure_production_graphs(terrain: Resource) -> void:
	super._phase29_ensure_production_graphs(terrain)
	var migrated: bool = false
	var shape: Resource = terrain.call("find_shader_slot", PRODUCTION_SHAPE_SLOT_ID) as Resource
	if shape != null and _phase32_is_phase31_identity_shape(shape):
		_phase32_install_full_production_graph(shape, SHADER_SLOT_MODEL.Domain.DISPLACEMENT)
		migrated = true
	var surface: Resource = terrain.call("find_shader_slot", PRODUCTION_SURFACE_SLOT_ID) as Resource
	if surface != null and _phase32_is_phase31_identity_surface(surface):
		_phase32_install_full_production_graph(surface, SHADER_SLOT_MODEL.Domain.MATERIAL)
		migrated = true
	if migrated:
		_session.call("_mark_dirty", WorldAuthoringSession.ApplyScope.GRAPH)


func _phase32_install_full_production_graph(slot: Resource, domain: int) -> void:
	var graph: Resource = slot.get(&"graph") as Resource if slot != null else null
	if graph == null:
		return
	graph.call("create_production_stage_graph", domain)
	graph.set(&"display_name", "Base Terrain Graph" if domain == SHADER_SLOT_MODEL.Domain.DISPLACEMENT \
		else "Base Surface Graph")


func _phase32_is_phase31_identity_shape(slot: Resource) -> bool:
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
		match String(node.get("type", "")):
			"PRODUCTION_GENERATED_HEIGHT": generated_id = String(node.get("id", ""))
			"PRODUCTION_SCULPT_DELTA": sculpt_id = String(node.get("id", ""))
			"ADD": add_id = String(node.get("id", ""))
			"OUTPUT_DISPLACEMENT": output_id = String(node.get("id", ""))
			_: return false
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


func _phase32_is_phase31_identity_surface(slot: Resource) -> bool:
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


func _phase29_build_graph_editor(slot: Resource) -> void:
	var graph_editor := PHASE32_GRAPH_EDITOR.new()
	graph_editor.custom_minimum_size = Vector2(1180.0, 760.0)
	_workspace.add_child(graph_editor)
	graph_editor.call_deferred("setup", _session, slot,
		Callable(self, "_refresh_current_category"))
	var hint := Label.new()
	hint.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	if _phase28_domain == SHADER_SLOT_MODEL.Domain.DISPLACEMENT:
		hint.text = "The production Shape graph now contains GENERATED TERRAIN, its real geomorph band controls, SCULPT / EDIT DELTA and FINAL TERRAIN. The default is the existing renderer exactly; changing a production setting changes that renderer in place and remains shared with contact physics."
	else:
		hint.text = "The production Surface graph exposes final PBR channels plus classifier, microrelief, anti-tiling, rock PBR, scanned PBR and the exact scan texture resources. Raw world-data and ordinary graph math/texture nodes remain available for replacing any stage."
	hint.modulate = Color(0.58, 0.69, 0.78)
	_workspace.add_child(hint)
