extends Node
## Phase 31+ regression: production displacement is visible as the real native
## contribution graph, exact bypasses stay on the resident GPU implementation,
## PBR sources remain ordinary categorized graph nodes, and raw classifier vectors
## can still be split/recombined by the live material compiler.

const EDITOR_SCRIPT := preload(
	"res://scripts/world_authoring/world_authoring_editor_live_phase31.gd")
const DISP_RUNTIME_SCRIPT := preload(
	"res://scripts/world_authoring/terrain_displacement_runtime_phase36.gd")
const MAT_RUNTIME_SCRIPT := preload(
	"res://scripts/world_authoring/terrain_material_runtime_phase31.gd")
const GRAPH_MODEL := preload(
	"res://scripts/world_authoring/model/terrain_shader_graph_definition.gd")
const NATIVE := preload(
	"res://scripts/world_authoring/model/terrain_production_geomorph_graph.gd")

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

	for required_title: String in [
		"PRODUCTION · WORLD CONTEXT",
		"PRODUCTION · BROAD RELIEF",
		"PRODUCTION · MOUNTAINS",
		"PRODUCTION · MID RELIEF",
		"PRODUCTION · CHANNELS / INCISION",
		"PRODUCTION · DEPOSITION",
		"PRODUCTION · FINE DETAIL",
		"PRODUCTION · DUNES",
		"PRODUCTION · MICRO RELIEF",
		"PRODUCTION · NATIVE DETAIL MERGE",
		"PRODUCTION · GLACIAL SHAPING",
		"PRODUCTION · MACRO + DETAIL",
		"SCULPT / EDIT DELTA",
		"Add",
		"FINAL TERRAIN",
	]:
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
	if not NATIVE.is_canonical_structural_graph(shape_graph):
		_fail("Phase 31 Shape graph did not migrate to canonical native contribution/merge topology.")
		return
	if _has_node_type(shape_graph, NATIVE.LEGACY_GENERATED_TYPE):
		_fail("Opaque PRODUCTION_GENERATED_HEIGHT survived native production migration.")
		return
	if not _has_node_type(shape_graph, NATIVE.MERGE_TYPE):
		_fail("Phase 31 Shape graph is missing Native Detail Merge.")
		return
	for stage_id: String in NATIVE.SCHEMA.ordered_stage_ids():
		if not _has_node_type(shape_graph, NATIVE.stage_node_type(stage_id)):
			_fail("Phase 31 Shape graph is missing native stage '%s'." % stage_id)
			return

	var disp_runtime: Node = DISP_RUNTIME_SCRIPT.new() as Node
	add_child(disp_runtime)
	var disp_stats: Dictionary = disp_runtime.call("compile_from_terrain", terrain)
	if not bool(disp_stats.get("candidate_valid", false)) \
			or bool(disp_stats.get("active", true)) \
			or int(disp_stats.get("instructions", -1)) != 0:
		_fail("Canonical native production Shape graph should remain GPU-resident and bytecode-neutral.")
		return

	# Disconnect only Channels from its dedicated merge input. Deposition stays on
	# its own branch and must therefore remain active without rerouting height.
	var merge_id: String = _node_id(shape_graph, NATIVE.MERGE_TYPE)
	var channel_id: String = _node_id(shape_graph, NATIVE.stage_node_type("channel"))
	var channel_port: int = NATIVE.merge_port_for_stage("channel")
	if merge_id.is_empty() or channel_id.is_empty() or channel_port < 0:
		_fail("Phase 31 contribution-bypass fixture is incomplete.")
		return
	if not bool(shape_graph.call("disconnect_nodes", channel_id, 0, merge_id, channel_port)):
		_fail("Phase 31 could not disconnect the Channel contribution.")
		return
	var bypass_stats: Dictionary = disp_runtime.call("compile_from_terrain", terrain)
	if not bool(bypass_stats.get("candidate_valid", false)) \
			or bool(bypass_stats.get("active", true)) \
			or int(bypass_stats.get("instructions", -1)) != 0:
		_fail("Phase 31 native contribution bypass escaped the resident zero-bytecode production path.")
		return
	var bypass_controls: Dictionary = bypass_stats.get("production_geomorph_controls", {}) as Dictionary
	if not is_zero_approx(float(bypass_controls.get("channel_strength", -1.0))) \
			or is_zero_approx(float(bypass_controls.get("deposit_strength", 0.0))):
		_fail("Phase 31 Channel bypass did not preserve exact Deposition semantics.")
		return

	# Restore the canonical native Shape graph before checking the Surface domain.
	var restored_controls: Dictionary = NATIVE.extract_controls(shape_graph)
	NATIVE.build_canonical_graph(shape_graph, restored_controls)

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
		var channel_id_value: String = String(surface_graph.call("add_node", channel_type,
			Vector2(330, 590 + index * 95), {}))
		surface_graph.call("connect_nodes", classifier_id, 0, channel_id_value, 0)
		channel_ids.append(channel_id_value)
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

	print("PLANET_STUDIO_PHASE31_LAUNCH_OK: native contribution merge, exact bypass lowering and editable PBR graph launch cleanly")
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
