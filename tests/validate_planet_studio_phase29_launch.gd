extends Node
## Runtime regression for Phase 29's simple shader UX and production-graph migration.
## The old synthetic locked production node must be gone: current terrain/PBR
## sources are serialized graph nodes and absolute shape editing must preserve the
## existing terrain when unchanged.

const EDITOR_SCRIPT := preload(
	"res://scripts/world_authoring/world_authoring_editor_live_phase29.gd")
const DISP_RUNTIME_SCRIPT := preload(
	"res://scripts/world_authoring/terrain_displacement_runtime_phase29.gd")

class DummyWorld extends Node3D:
	var player: Node = null

class DummyRuntimeHost extends Node:
	var _detailed_runtime_body_id: String = ""


func _ready() -> void:
	call_deferred("_run")


func _run() -> void:
	OS.set_environment("ASTERRA_AUTHORING_RECOVERY_PATH",
		"user://world_authoring/tests/phase29_recovery_%d.tres" % OS.get_process_id())
	var world := DummyWorld.new()
	world.name = "Phase29SmokeWorld"
	add_child(world)
	var host := DummyRuntimeHost.new()
	host.name = "PlanetStudioRuntimeHost"
	world.add_child(host)
	var layer := CanvasLayer.new()
	layer.name = "PlanetStudioLiveLayer"
	host.add_child(layer)
	var editor: Control = EDITOR_SCRIPT.new() as Control
	if editor == null:
		_fail("Phase 29 editor failed to instantiate.")
		return
	editor.name = "PlanetStudioLive"
	editor.call("bind_world", world)
	layer.add_child(editor)
	await _frames(3)

	editor.call("_show_category", "SHADERS")
	await _frames(5)
	if not is_instance_valid(editor) or not editor.is_inside_tree():
		_fail("Phase 29 editor left the tree while opening Terrain shaders.")
		return
	if _has_text(editor, "1 — Target") or _has_text(editor, "3 — Build live terrain flow"):
		_fail("Phase 29 still exposes the Phase 28 implementation wizard.")
		return
	if _find_graph_node(editor, "PRODUCTION TERRAIN (ALWAYS PRESENT)") != null:
		_fail("Phase 29 still creates the synthetic locked production terrain node.")
		return
	if _find_graph_node(editor, "CURRENT TERRAIN HEIGHT") == null:
		_fail("Phase 29 Shape graph does not expose current terrain as a real node.")
		return
	if _find_graph_node(editor, "FINAL TERRAIN") == null:
		_fail("Phase 29 Shape graph has no Final Terrain node.")
		return

	var session: Object = editor.get("_session") as Object
	var terrain: Resource = session.call("active_terrain_profile") as Resource if session != null else null
	if terrain == null:
		_fail("Phase 29 has no active terrain profile.")
		return
	var shape_slot: Resource = terrain.call("find_shader_slot", "production-terrain-shape") as Resource
	var surface_slot: Resource = terrain.call("find_shader_slot", "production-terrain-surface") as Resource
	if shape_slot == null or surface_slot == null:
		_fail("Phase 29 did not migrate production shape/surface into serialized slots.")
		return
	var shape_graph: Resource = shape_slot.get(&"graph") as Resource
	if shape_graph == null or int(shape_graph.get(&"displacement_output_mode")) != 1:
		_fail("Production Shape graph is not absolute-height authoritative.")
		return

	# Identity production graph: final absolute height == incoming production height,
	# therefore the authored delta sent back to the clipmap/contact stack must be 0.
	var runtime: Node = DISP_RUNTIME_SCRIPT.new() as Node
	add_child(runtime)
	runtime.call("compile_from_terrain", terrain)
	var identity_delta: float = float(runtime.call("evaluate_height",
		Vector3(1.0, 0.0, 0.0), 123.0, 0, 7, 0.0))
	if absf(identity_delta) > 0.0001:
		_fail("Absolute production Shape identity changed existing terrain: %f." % identity_delta)
		return

	# Insert +20 m into the actual production graph. The compiler must emit +20 m,
	# not 143 m, proving the absolute result is converted back to a renderer delta.
	var output_id: String = ""
	var source_id: String = ""
	for node_value: Variant in shape_graph.get(&"nodes") as Array:
		var node_data: Dictionary = node_value as Dictionary
		var node_type: String = String(node_data.get("type", ""))
		if node_type == "OUTPUT_DISPLACEMENT":
			output_id = String(node_data.get("id", ""))
		elif node_type == "GAME_INPUT":
			var parameters: Dictionary = node_data.get("parameters", {}) as Dictionary
			if String(parameters.get("source", "")) == "terrain_height_m":
				source_id = String(node_data.get("id", ""))
	if output_id.is_empty() or source_id.is_empty():
		_fail("Production Shape graph is missing its serialized source/output nodes.")
		return
	shape_graph.call("disconnect_nodes", source_id, 0, output_id, 0)
	var amount_id: String = String(shape_graph.call("add_node", "CONSTANT_FLOAT",
		Vector2(260.0, 300.0), {"value":20.0}))
	var add_id: String = String(shape_graph.call("add_node", "ADD",
		Vector2(360.0, 180.0), {}))
	shape_graph.call("connect_nodes", source_id, 0, add_id, 0)
	shape_graph.call("connect_nodes", amount_id, 0, add_id, 1)
	shape_graph.call("connect_nodes", add_id, 0, output_id, 0)
	runtime.call("compile_from_terrain", terrain)
	var edited_delta: float = float(runtime.call("evaluate_height",
		Vector3(1.0, 0.0, 0.0), 123.0, 0, 7, 0.0))
	if absf(edited_delta - 20.0) > 0.001:
		_fail("Absolute production Shape edit expected +20 m, got %f." % edited_delta)
		return

	# Surface is equally direct: current PBR channels are serialized inputs, including
	# Phase 29's sixth Specular channel.
	editor.set("_phase28_domain", 1)
	editor.set("_phase28_selected_slot_id", "")
	editor.call("_refresh_current_category")
	await _frames(5)
	for title: String in ["CURRENT ALBEDO", "CURRENT NORMAL", "CURRENT ROUGHNESS",
			"CURRENT METALLIC", "CURRENT AO", "CURRENT SPECULAR", "FINAL SURFACE"]:
		if _find_graph_node(editor, title) == null:
			_fail("Phase 29 Surface graph is missing active node '%s'." % title)
			return
	if _find_graph_node(editor, "PRODUCTION TEXTURES / PBR (ALWAYS PRESENT)") != null:
		_fail("Phase 29 Surface still contains the synthetic locked PBR node.")
		return

	print("PLANET_STUDIO_PHASE29_LAUNCH_OK: production shape + surface are editable graph nodes")
	runtime.queue_free()
	editor.queue_free()
	await _frames(2)
	get_tree().quit(0)


func _frames(count: int) -> void:
	for _index: int in count:
		await get_tree().process_frame


func _fail(message: String) -> void:
	push_error(message)
	print("PLANET_STUDIO_PHASE29_LAUNCH_FAILED: %s" % message)
	get_tree().quit(1)


func _has_text(root: Node, text: String) -> bool:
	for node: Node in root.find_children("*", "Label", true, false):
		var label := node as Label
		if label != null and label.text.contains(text):
			return true
	for node: Node in root.find_children("*", "Button", true, false):
		var button := node as Button
		if button != null and button.text.contains(text):
			return true
	return false


func _find_graph_node(root: Node, title: String) -> GraphNode:
	for node_value: Node in root.find_children("*", "GraphNode", true, false):
		var graph_node := node_value as GraphNode
		if graph_node != null and graph_node.title == title:
			return graph_node
	return null
