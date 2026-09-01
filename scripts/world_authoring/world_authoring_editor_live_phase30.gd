extends "res://scripts/world_authoring/world_authoring_editor_live_phase29.gd"
## Phase 30: decompose the old "Generated height + sculpt" production block.
##
## Base Terrain now serializes the same composition as ordinary graph nodes:
##
##   GENERATED TERRAIN ----\
##                         ADD ----> FINAL TERRAIN
##   SCULPT / EDIT DELTA --/
##
## Both source nodes and the Add node are normal editable/deletable graph nodes.
## The Phase 30 GPU/CPU compiler keeps this identity-neutral relative to the mature
## clipmap/contact pipeline, so the graph can safely reroute either source.

const PHASE30_GRAPH_EDITOR := preload(
	"res://scripts/world_authoring/terrain_graph_editor_phase30.gd")


func _phase29_ensure_production_graphs(terrain: Resource) -> void:
	super._phase29_ensure_production_graphs(terrain)
	var shape: Resource = terrain.call("find_shader_slot", PRODUCTION_SHAPE_SLOT_ID) as Resource
	if shape != null and _phase30_is_phase29_identity_shape(shape):
		_phase30_install_shape_flow(shape)
		_session.call("_mark_dirty", WorldAuthoringSession.ApplyScope.GRAPH)


func _phase29_create_production_slot(terrain: Resource, domain: int,
		slot_id: String, display_name: String) -> Resource:
	var slot: Resource = super._phase29_create_production_slot(
		terrain, domain, slot_id, display_name)
	if slot != null and domain == SHADER_SLOT_MODEL.Domain.DISPLACEMENT:
		_phase30_install_shape_flow(slot)
	return slot


func _phase30_is_phase29_identity_shape(slot: Resource) -> bool:
	if slot == null or String(slot.get(&"slot_id")) != PRODUCTION_SHAPE_SLOT_ID:
		return false
	var graph: Resource = slot.get(&"graph") as Resource
	if graph == null:
		return false
	var nodes: Array = graph.get(&"nodes") as Array
	var links: Array = graph.get(&"links") as Array
	if nodes.size() != 2 or links.size() != 1:
		return false
	var has_output: bool = false
	var has_generated: bool = false
	for node_value: Variant in nodes:
		var node: Dictionary = node_value as Dictionary
		var node_type: String = String(node.get("type", ""))
		if node_type == "OUTPUT_DISPLACEMENT":
			has_output = true
		elif node_type == "GAME_INPUT":
			var parameters: Dictionary = node.get("parameters", {}) as Dictionary
			var source: String = String(parameters.get("source", ""))
			has_generated = source == "terrain_height_m" or source == "generated_height_m"
	return has_output and has_generated


func _phase30_install_shape_flow(slot: Resource) -> void:
	var graph: Resource = slot.get(&"graph") as Resource if slot != null else null
	if graph == null:
		return
	graph.call("create_production_graph", SHADER_SLOT_MODEL.Domain.DISPLACEMENT)
	graph.set(&"display_name", "Base Terrain Graph")
	var generated_id: String = ""
	var output_id: String = ""
	for node_value: Variant in graph.get(&"nodes") as Array:
		var node: Dictionary = node_value as Dictionary
		var node_type: String = String(node.get("type", ""))
		if node_type == "GAME_INPUT":
			generated_id = String(node.get("id", ""))
		elif node_type == "OUTPUT_DISPLACEMENT":
			output_id = String(node.get("id", ""))
	if generated_id.is_empty() or output_id.is_empty():
		return
	graph.call("set_node_parameter", generated_id, "source", "generated_height_m")
	graph.call("set_node_position", generated_id, Vector2(70.0, 100.0))
	graph.call("set_node_position", output_id, Vector2(720.0, 205.0))
	graph.call("disconnect_nodes", generated_id, 0, output_id, 0)
	var sculpt_id: String = String(graph.call("add_node", "GAME_INPUT",
		Vector2(70.0, 320.0), {"source":"sculpt_delta_m"}))
	var add_id: String = String(graph.call("add_node", "ADD",
		Vector2(390.0, 205.0), {}))
	graph.call("connect_nodes", generated_id, 0, add_id, 0)
	graph.call("connect_nodes", sculpt_id, 0, add_id, 1)
	graph.call("connect_nodes", add_id, 0, output_id, 0)


func _phase29_reset_production_graph(slot: Resource) -> void:
	if slot == null or String(slot.get(&"slot_id")) != PRODUCTION_SHAPE_SLOT_ID:
		super._phase29_reset_production_graph(slot)
		return
	_session.stage_action("Reset production terrain graph", func() -> void:
		_phase30_install_shape_flow(slot)
	, WorldAuthoringSession.ApplyScope.GRAPH)
	_refresh_current_category()


func _phase29_build_graph_editor(slot: Resource) -> void:
	var graph_editor := PHASE30_GRAPH_EDITOR.new()
	graph_editor.custom_minimum_size = Vector2(1080.0, 680.0)
	_workspace.add_child(graph_editor)
	graph_editor.call_deferred("setup", _session, slot,
		Callable(self, "_refresh_current_category"))
	var hint := Label.new()
	hint.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	if _phase28_domain == SHADER_SLOT_MODEL.Domain.DISPLACEMENT:
		hint.text = "This is the active shape flow. GENERATED TERRAIN and SCULPT / EDIT DELTA are ordinary nodes: delete, scale, subtract, mix or replace either one. What reaches FINAL TERRAIN is the rendered/contact terrain."
	else:
		hint.text = "This is the active surface flow. Current PBR channels and world data are graph inputs. Add texture, classification, math or PBR nodes and wire the result to FINAL SURFACE."
	hint.modulate = Color(0.58, 0.69, 0.78)
	_workspace.add_child(hint)
