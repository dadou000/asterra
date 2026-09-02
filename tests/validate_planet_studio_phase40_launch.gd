extends Node
## Phase 40 semantic smoke test through the current PlanetStudio activation path.
##
## Phase 45 is the current successor wrapper. This regression still proves that the
## beginner-readable Phase 40 resident linear-composition graph remains intact and
## bytecode-neutral after the simplified UI, spatial masks and terrain presets are
## activated. The test explicitly enters Advanced before inspecting graph nodes,
## because the current product correctly opens Terrain in Simple mode.

const PLANET_STUDIO_SCENE := preload("res://scenes/world_authoring/PlanetStudio.tscn")
const EXPECTED_EDITOR_PATH := "res://scripts/world_authoring/world_authoring_editor_live_phase45.gd"
const NATIVE := preload(
	"res://scripts/world_authoring/model/terrain_production_geomorph_graph.gd")
const LOWERING := preload(
	"res://scripts/world_authoring/model/terrain_production_geomorph_lowering_phase40.gd")
const RUNTIME := preload(
	"res://scripts/world_authoring/terrain_displacement_runtime_phase41.gd")

class DummyWorld extends Node3D:
	var player: Node = null

class DummyRuntimeHost extends Node:
	var _detailed_runtime_body_id: String = ""


func _ready() -> void:
	call_deferred("_run")


func _run() -> void:
	OS.set_environment("ASTERRA_AUTHORING_RECOVERY_PATH",
		"user://world_authoring/tests/phase40_recovery_%d.tres" % OS.get_process_id())
	var world := DummyWorld.new()
	world.name = "Phase40SmokeWorld"
	add_child(world)
	var host := DummyRuntimeHost.new()
	host.name = "PlanetStudioRuntimeHost"
	world.add_child(host)
	var layer := CanvasLayer.new()
	layer.name = "PlanetStudioLiveLayer"
	host.add_child(layer)

	var editor: Control = PLANET_STUDIO_SCENE.instantiate() as Control
	if editor == null:
		_fail("Actual PlanetStudio.tscn failed to instantiate.")
		return
	var script: Script = editor.get_script() as Script
	if script == null or script.resource_path != EXPECTED_EDITOR_PATH:
		_fail("PlanetStudio.tscn is not using the current Phase 45 live editor wrapper.")
		return
	editor.call("bind_world", world)
	layer.add_child(editor)
	await _frames(3)
	editor.call("_show_category", "SHADERS")
	await _frames(5)

	var session: Object = editor.get("_session") as Object
	var terrain: Resource = session.call("active_terrain_profile") as Resource if session != null else null
	if terrain == null:
		_fail("Current Planet Studio has no active terrain profile.")
		return
	var shape_slot: Resource = terrain.call("find_shader_slot", NATIVE.PRODUCTION_SHAPE_SLOT_ID) as Resource
	if shape_slot == null:
		_fail("Current Planet Studio has no Base Terrain Shape slot.")
		return
	var graph: Resource = shape_slot.get(&"graph") as Resource
	if graph == null or not NATIVE.has_merge_node(graph):
		_fail("Base Terrain Shape did not migrate to the native contribution/merge graph.")
		return

	# Build a single-stage Scale path. The saved node types stay generic, while the
	# inherited Phase 40 advanced presentation uses terrain-language controls.
	var merge_id: String = _node_id(graph, NATIVE.MERGE_TYPE)
	var mountain_id: String = _node_id(graph, NATIVE.stage_node_type("mountain"))
	var mountain_port: int = NATIVE.merge_port_for_stage("mountain")
	var scale_id: String = String(graph.call("add_node", "MULTIPLY", Vector2(700, 360), {}))
	var scale_factor_id: String = String(graph.call("add_node", "CONSTANT_FLOAT",
		Vector2(520, 520), {"value":0.5}))
	if not bool(graph.call("disconnect_nodes", mountain_id, 0, merge_id, mountain_port)) \
			or not bool(graph.call("connect_nodes", mountain_id, 0, scale_id, 0)) \
			or not bool(graph.call("connect_nodes", scale_factor_id, 0, scale_id, 1)) \
			or not bool(graph.call("connect_nodes", scale_id, 0, merge_id, mountain_port)):
		_fail("Could not build valid Phase 40 Scale Contribution fixture.")
		return

	# Build a two-stage constant Blend. Multi-stage results deliberately use Custom
	# Group rather than pretending to be one native stage's dedicated contribution.
	var broad_id: String = _node_id(graph, NATIVE.stage_node_type("broad"))
	var mid_id: String = _node_id(graph, NATIVE.stage_node_type("mid"))
	var mix_id: String = String(graph.call("add_node", "MIX", Vector2(760, 720), {}))
	var blend_id: String = String(graph.call("add_node", "CONSTANT_FLOAT",
		Vector2(560, 920), {"value":0.25}))
	if not bool(graph.call("disconnect_nodes", broad_id, 0, merge_id,
			NATIVE.merge_port_for_stage("broad"))) \
			or not bool(graph.call("disconnect_nodes", mid_id, 0, merge_id,
				NATIVE.merge_port_for_stage("mid"))) \
			or not bool(graph.call("connect_nodes", broad_id, 0, mix_id, 0)) \
			or not bool(graph.call("connect_nodes", mid_id, 0, mix_id, 1)) \
			or not bool(graph.call("connect_nodes", blend_id, 0, mix_id, 2)) \
			or not bool(graph.call("connect_nodes", mix_id, 0, merge_id,
				LOWERING.group_merge_port_start())):
		_fail("Could not build valid Phase 40 Blend Contributions fixture.")
		return

	var plan: Dictionary = LOWERING.contribution_merge_plan(graph)
	if not bool(plan.get("valid", false)) or not bool(plan.get("has_mix", false)) \
			or not bool(plan.get("has_multiply", false)):
		_fail("Phase 40 Scale/Blend fixture is not recognized by exact symbolic lowering.")
		return

	# The current editor intentionally opens Terrain in Simple mode. Enter Advanced
	# before inspecting the exact resident graph; this should reveal the same Resource
	# rather than translating or rebuilding it into another terrain representation.
	editor.set("_phase43_advanced_terrain", true)
	editor.call("_refresh_current_category")
	await _frames(5)
	editor.call("_phase28_focus_existing_slot", shape_slot)
	await _frames(5)
	for required_title: String in [
		"SCALE CONTRIBUTION",
		"SCALE FACTOR",
		"BLEND CONTRIBUTIONS",
		"BLEND AMOUNT",
		"PRODUCTION · NATIVE DETAIL MERGE",
	]:
		if _find_graph_node(editor, required_title) == null:
			_fail("Current Base Terrain graph is missing inherited Phase 40 node '%s'." % required_title)
			return
	var merge_node: GraphNode = _find_graph_node(editor, "PRODUCTION · NATIVE DETAIL MERGE")
	for group_index: int in LOWERING.group_merge_port_count():
		if not _graph_node_has_text(merge_node, "Custom Group %d" % (group_index + 1)):
			_fail("Native Detail Merge is missing Custom Group %d." % (group_index + 1))
			return

	# Newer wrappers must not introduce a second program for native production stages.
	var runtime: Node = RUNTIME.new() as Node
	add_child(runtime)
	var compiled: Dictionary = runtime.call("compile_from_terrain", terrain) as Dictionary
	if not bool(compiled.get("candidate_valid", false)) \
			or bool(compiled.get("active", true)) \
			or int(compiled.get("instructions", -1)) != 0:
		_fail("Current activation changed Phase 40 resident Scale/Blend into authored bytecode.")
		return
	var controls: Dictionary = compiled.get("production_geomorph_controls", {}) as Dictionary
	if not is_equal_approx(float(controls.get("mountain_strength", 0.0)), 0.5):
		_fail("Scale Contribution did not reach the resident Mountain control exactly once.")
		return
	if not is_equal_approx(float(controls.get("broad_strength", 0.0)), 0.75) \
			or not is_equal_approx(float(controls.get("mid_strength", 0.0)), 0.25):
		_fail("Blend Contributions did not reduce to the expected resident coefficients.")
		return

	print("PLANET_STUDIO_PHASE40_LAUNCH_OK: Phase 45 activation preserves Phase 40 friendly Scale/Blend graph as resident zero-bytecode terrain")
	runtime.queue_free()
	editor.queue_free()
	await _frames(2)
	get_tree().quit(0)


func _node_id(graph: Resource, node_type: String) -> String:
	if graph == null:
		return ""
	for node_value: Variant in graph.get(&"nodes") as Array:
		if not (node_value is Dictionary):
			continue
		var node: Dictionary = node_value as Dictionary
		if String(node.get("type", "")) == node_type:
			return String(node.get("id", ""))
	return ""


func _find_graph_node(root: Node, title: String) -> GraphNode:
	for node_value: Node in root.find_children("*", "GraphNode", true, false):
		var graph_node := node_value as GraphNode
		if graph_node != null and graph_node.title == title:
			return graph_node
	return null


func _graph_node_has_text(node: GraphNode, text: String) -> bool:
	if node == null:
		return false
	for child: Node in node.find_children("*", "Label", true, false):
		var label := child as Label
		if label != null and label.text == text:
			return true
	return false


func _frames(count: int) -> void:
	for _index: int in count:
		await get_tree().process_frame


func _fail(message: String) -> void:
	push_error(message)
	print("PLANET_STUDIO_PHASE40_LAUNCH_FAILED: %s" % message)
	get_tree().quit(1)
