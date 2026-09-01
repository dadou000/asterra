extends Node
## Phase 30 runtime regression: the former locked "Generated height + sculpt"
## production block must be decomposed into ordinary serialized nodes, and the
## absolute compiler must remain identity-neutral with a non-zero sculpt baseline.

const EDITOR_SCRIPT := preload(
	"res://scripts/world_authoring/world_authoring_editor_live_phase30.gd")
const DISP_RUNTIME_SCRIPT := preload(
	"res://scripts/world_authoring/terrain_displacement_runtime_phase30.gd")

class DummyWorld extends Node3D:
	var player: Node = null

class DummyRuntimeHost extends Node:
	var _detailed_runtime_body_id: String = ""


func _ready() -> void:
	call_deferred("_run")


func _run() -> void:
	OS.set_environment("ASTERRA_AUTHORING_RECOVERY_PATH",
		"user://world_authoring/tests/phase30_recovery_%d.tres" % OS.get_process_id())
	var world := DummyWorld.new()
	world.name = "Phase30SmokeWorld"
	add_child(world)
	var host := DummyRuntimeHost.new()
	host.name = "PlanetStudioRuntimeHost"
	world.add_child(host)
	var layer := CanvasLayer.new()
	layer.name = "PlanetStudioLiveLayer"
	host.add_child(layer)
	var editor: Control = EDITOR_SCRIPT.new() as Control
	if editor == null:
		_fail("Phase 30 editor failed to instantiate.")
		return
	editor.name = "PlanetStudioLive"
	editor.call("bind_world", world)
	layer.add_child(editor)
	await _frames(3)
	editor.call("_show_category", "SHADERS")
	await _frames(5)

	for required_title: String in ["GENERATED TERRAIN", "SCULPT / EDIT DELTA", "ADD", "FINAL TERRAIN"]:
		if _find_graph_node(editor, required_title) == null:
			_fail("Base Terrain graph is missing '%s'." % required_title)
			return
	if _find_graph_node(editor, "PRODUCTION TERRAIN (ALWAYS PRESENT)") != null:
		_fail("Synthetic locked production node returned in Phase 30.")
		return

	var session: Object = editor.get("_session") as Object
	var terrain: Resource = session.call("active_terrain_profile") as Resource if session != null else null
	var shape_slot: Resource = terrain.call("find_shader_slot", "production-terrain-shape") as Resource \
		if terrain != null else null
	var graph: Resource = shape_slot.get(&"graph") as Resource if shape_slot != null else null
	if graph == null:
		_fail("Phase 30 has no serialized Base Terrain graph.")
		return
	var generated_id: String = ""
	var sculpt_id: String = ""
	var add_id: String = ""
	var output_id: String = ""
	for node_value: Variant in graph.get(&"nodes") as Array:
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
		_fail("Serialized Base Terrain graph did not decompose generated/sculpt stages.")
		return

	var runtime: Node = DISP_RUNTIME_SCRIPT.new() as Node
	add_child(runtime)
	runtime.call("compile_from_terrain", terrain)
	# Current surface is generated=123 + sculpt=7. An unchanged absolute production
	# graph must emit zero adjustment because the inherited renderer/contact path adds
	# that same baseline normally.
	var identity_delta: float = float(runtime.call("evaluate_height",
		Vector3(1, 0, 0), 123.0, 0, 7, 0.0, 7.0))
	if absf(identity_delta) > 0.0001:
		_fail("Generated+sculpt identity produced %f m delta." % identity_delta)
		return

	# Remove sculpt from the graph: final desired terrain becomes generated-only.
	# Relative to the existing +7 m sculpt baseline, authored adjustment must be -7.
	graph.call("disconnect_nodes", sculpt_id, 0, add_id, 1)
	var zero_id: String = String(graph.call("add_node", "CONSTANT_FLOAT",
		Vector2(170, 390), {"value":0.0}))
	graph.call("connect_nodes", zero_id, 0, add_id, 1)
	runtime.call("compile_from_terrain", terrain)
	var remove_sculpt_delta: float = float(runtime.call("evaluate_height",
		Vector3(1, 0, 0), 123.0, 0, 7, 0.0, 7.0))
	if absf(remove_sculpt_delta + 7.0) > 0.001:
		_fail("Removing Sculpt node expected -7 m adjustment, got %f." % remove_sculpt_delta)
		return

	print("PLANET_STUDIO_PHASE30_LAUNCH_OK: generated + sculpt production base is ordinary editable nodes")
	runtime.queue_free()
	editor.queue_free()
	await _frames(2)
	get_tree().quit(0)


func _frames(count: int) -> void:
	for _index: int in count:
		await get_tree().process_frame


func _fail(message: String) -> void:
	push_error(message)
	print("PLANET_STUDIO_PHASE30_LAUNCH_FAILED: %s" % message)
	get_tree().quit(1)


func _find_graph_node(root: Node, title: String) -> GraphNode:
	for node_value: Node in root.find_children("*", "GraphNode", true, false):
		var graph_node := node_value as GraphNode
		if graph_node != null and graph_node.title == title:
			return graph_node
	return null
