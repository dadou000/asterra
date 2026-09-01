extends Node
## Phase 31 regression: production displacement and PBR are visible as ordinary
## categorized nodes, identity graphs stay GPU-neutral, and raw classifier vectors
## can be split/recombined by the live material compiler.

const EDITOR_SCRIPT := preload(
	"res://scripts/world_authoring/world_authoring_editor_live_phase31.gd")
const DISP_RUNTIME_SCRIPT := preload(
	"res://scripts/world_authoring/terrain_displacement_runtime_phase31.gd")
const MAT_RUNTIME_SCRIPT := preload(
	"res://scripts/world_authoring/terrain_material_runtime_phase31.gd")
const GRAPH_MODEL := preload(
	"res://scripts/world_authoring/model/terrain_shader_graph_definition.gd")

class DummyWorld extends Node3D:
	var player: Node = null

class DummyRuntimeHost extends Node:
	var _detailed_runtime_body_id: String = ""


func _ready() -> void:
	call_deferred("_run")


func _run() -> void:
	OS.set_environment("ASTERRA_AUTHORING_RECOVERY_PATH",
		"user://world_authoring/tests/phase31_recovery_%d.tres" % OS.get_process_id())
	var world := DummyWorld.new()
	world.name = "Phase31SmokeWorld"
	add_child(world)
	var host := DummyRuntimeHost.new()
	host.name = "PlanetStudioRuntimeHost"
	world.add_child(host)
	var layer := CanvasLayer.new()
	layer.name = "PlanetStudioLiveLayer"
	host.add_child(layer)
	var editor: Control = EDITOR_SCRIPT.new() as Control
	if editor == null:
		_fail("Phase 31 editor failed to instantiate.")
		return
	editor.name = "PlanetStudioLive"
	editor.call("bind_world", world)
	layer.add_child(editor)
	await _frames(3)
	editor.call("_show_category", "SHADERS")
	await _frames(5)

	for required_title: String in ["GENERATED TERRAIN", "SCULPT / EDIT DELTA", "Add", "FINAL TERRAIN"]:
		if _find_graph_node(editor, required_title) == null:
			_fail("Phase 31 Base Terrain graph is missing '%s'." % required_title)
			return

	var session: Object = editor.get("_session") as Object
	var terrain: Resource = session.call("active_terrain_profile") as Resource if session != null else null
	if terrain == null:
		_fail("Phase 31 has no active terrain profile.")
		return
	var shape_slot: Resource = terrain.call("find_shader_slot", "production-terrain-shape") as Resource
	var surface_slot: Resource = terrain.call("find_shader_slot", "production-terrain-surface") as Resource
	if shape_slot == null or surface_slot == null:
		_fail("Phase 31 production shape/surface slots are missing.")
		return

	var shape_graph: Resource = shape_slot.get(&"graph") as Resource
	if not _has_node_type(shape_graph, "PRODUCTION_GENERATED_HEIGHT") \
			or not _has_node_type(shape_graph, "PRODUCTION_SCULPT_DELTA"):
		_fail("Phase 31 Shape graph did not migrate to explicit production-stage nodes.")
		return
	var disp_runtime: Node = DISP_RUNTIME_SCRIPT.new() as Node
	add_child(disp_runtime)
	var disp_stats: Dictionary = disp_runtime.call("compile_from_terrain", terrain)
	if bool(disp_stats.get("active", true)):
		_fail("Identity production Shape graph should stay outside the bytecode interpreter.")
		return

	# The exact rendered/contact baseline is generated + sculpt. Removing sculpt from
	# the graph must therefore ask the inherited renderer for -7 m adjustment.
	var generated_id: String = _node_id(shape_graph, "PRODUCTION_GENERATED_HEIGHT")
	var sculpt_id: String = _node_id(shape_graph, "PRODUCTION_SCULPT_DELTA")
	var add_id: String = _node_id(shape_graph, "ADD")
	shape_graph.call("disconnect_nodes", sculpt_id, 0, add_id, 1)
	var zero_id: String = String(shape_graph.call("add_node", "CONSTANT_FLOAT",
		Vector2(170, 390), {"value":0.0}))
	shape_graph.call("connect_nodes", zero_id, 0, add_id, 1)
	disp_runtime.call("compile_from_terrain", terrain)
	var remove_sculpt_delta: float = float(disp_runtime.call("evaluate_height",
		Vector3(1, 0, 0), 123.0, 0, 7, 0.0, 7.0))
	if absf(remove_sculpt_delta + 7.0) > 0.001:
		_fail("Explicit Sculpt stage expected -7 m adjustment when removed, got %f." % remove_sculpt_delta)
		return

	# Restore the canonical Shape graph before checking the Surface domain.
	shape_graph.call("create_production_stage_graph", 0)

	# Surface graph: six real PBR production sources, not a synthetic locked block.
	editor.call("_phase28_focus_existing_slot", surface_slot)
	await _frames(5)
	for required_title: String in [
		"PRODUCTION ALBEDO", "PRODUCTION NORMAL", "PRODUCTION ROUGHNESS",
		"PRODUCTION METALLIC", "PRODUCTION AO", "PRODUCTION SPECULAR", "FINAL SURFACE"]:
		if _find_graph_node(editor, required_title) == null:
			_fail("Phase 31 Base Surface graph is missing '%s'." % required_title)
			return

	var production_catalog: Array[String] = GRAPH_MODEL.node_catalog(
		GRAPH_MODEL.Domain.MATERIAL, GRAPH_MODEL.CATEGORY_PRODUCTION)
	for node_type: String in ["PRODUCTION_ALBEDO", "PRODUCTION_NORMAL", "PRODUCTION_SPECULAR"]:
		if not production_catalog.has(node_type):
			_fail("Production node category is missing %s." % node_type)
			return
	var world_inputs: Array[String] = GRAPH_MODEL.game_input_options(GRAPH_MODEL.Domain.MATERIAL)
	for source: String in ["soil", "surface", "geology", "structure", "climate", "landform", "rock_mix"]:
		if not world_inputs.has(source):
			_fail("Surface graph does not expose raw production context '%s'." % source)
			return

	var mat_runtime: Node = MAT_RUNTIME_SCRIPT.new() as Node
	add_child(mat_runtime)
	var identity_stats: Dictionary = mat_runtime.call("compile_from_terrain", terrain)
	if bool(identity_stats.get("active", true)) \
			or not bool(identity_stats.get("production_identity_skipped", false)):
		_fail("Identity production Surface graph is not GPU-neutral.")
		return

	# Rebuild albedo from the real classifier's primary vec4 weights. This exercises
	# explicit classifier access plus R/G/B extraction and RGB recombination.
	var surface_graph: Resource = surface_slot.get(&"graph") as Resource
	var output_id: String = _node_id(surface_graph, "OUTPUT_MATERIAL")
	var old_albedo_id: String = _node_id(surface_graph, "PRODUCTION_ALBEDO")
	surface_graph.call("disconnect_nodes", old_albedo_id, 0, output_id, 0)
	var classifier_id: String = String(surface_graph.call("add_node",
		"CLASSIFIER_PRIMARY", Vector2(80, 650), {}))
	var channel_ids: Array[String] = []
	for index: int in 3:
		var channel_type: String = ["CHANNEL_R", "CHANNEL_G", "CHANNEL_B"][index]
		var channel_id: String = String(surface_graph.call("add_node", channel_type,
			Vector2(330, 590 + index * 95), {}))
		surface_graph.call("connect_nodes", classifier_id, 0, channel_id, 0)
		channel_ids.append(channel_id)
	var combine_id: String = String(surface_graph.call("add_node",
		"COMBINE_RGB", Vector2(540, 660), {}))
	for index: int in 3:
		surface_graph.call("connect_nodes", channel_ids[index], 0, combine_id, index)
	surface_graph.call("connect_nodes", combine_id, 0, output_id, 0)
	var compiled_stats: Dictionary = mat_runtime.call("compile_from_terrain", terrain)
	if not bool(compiled_stats.get("active", false)):
		_fail("Edited production Surface graph did not compile to a live material program.")
		return
	for warning_value: Variant in compiled_stats.get("warnings", PackedStringArray()):
		var warning: String = String(warning_value)
		if warning.contains("Unsupported") or warning.contains("exceeded"):
			_fail("Phase 31 material compiler warning: %s" % warning)
			return

	print("PLANET_STUDIO_PHASE31_LAUNCH_OK: production shape/PBR stages are categorized editable graph nodes")
	mat_runtime.queue_free()
	disp_runtime.queue_free()
	editor.queue_free()
	await _frames(2)
	get_tree().quit(0)


func _has_node_type(graph: Resource, node_type: String) -> bool:
	return not _node_id(graph, node_type).is_empty()


func _node_id(graph: Resource, node_type: String) -> String:
	if graph == null:
		return ""
	for node_value: Variant in graph.get(&"nodes") as Array:
		var node: Dictionary = node_value as Dictionary
		if String(node.get("type", "")) == node_type:
			return String(node.get("id", ""))
	return ""


func _frames(count: int) -> void:
	for _index: int in count:
		await get_tree().process_frame


func _fail(message: String) -> void:
	push_error(message)
	print("PLANET_STUDIO_PHASE31_LAUNCH_FAILED: %s" % message)
	get_tree().quit(1)


func _find_graph_node(root: Node, title: String) -> GraphNode:
	for node_value: Node in root.find_children("*", "GraphNode", true, false):
		var graph_node := node_value as GraphNode
		if graph_node != null and graph_node.title == title:
			return graph_node
	return null
